#!/usr/bin/python3
"""Private, on-demand XFCE Terminal update worker. Not a desktop tracking daemon."""

import json
import os
from pathlib import Path
import secrets
import sys
import tempfile
import threading
import time
import uuid

from core import Replacement, gate
from processes import UpdateError, identity

PATH = "/org/xfce/Terminal/Update"
INTERFACE = "org.xfce.Terminal.Update1"
TERMINAL_PATH = "/org/xfce/Terminal"
TERMINAL_INTERFACE = "org.xfce.Terminal5"
XML = """<node><interface name='org.xfce.Terminal.Update1'>
<method name='GetStatus'><arg type='s' direction='out'/></method>
<method name='Confirm'><arg type='b' direction='in'/><arg type='s' direction='out'/></method>
<method name='Cancel'><arg type='s' direction='out'/></method>
<method name='KWinResult'><arg type='s' direction='in'/><arg type='s' direction='in'/></method>
<method name='Operate'><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='s' direction='out'/></method>
</interface></node>"""


def unpack(value):
    if hasattr(value, "unpack"):
        return unpack(value.unpack())
    if isinstance(value, dict):
        return {key: unpack(item) for key, item in value.items()}
    if isinstance(value, (tuple, list)):
        return [unpack(item) for item in value]
    return value


class Backend:
    def __init__(self, request):
        import gi
        gi.require_version("Gio", "2.0")
        from gi.repository import Gio, GLib
        self.Gio, self.GLib = Gio, GLib
        self.request = request
        self.connection = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        self.lock = threading.Lock()
        self.confirmed = threading.Event()
        self.cancelled = threading.Event()
        self.pending = {}
        self.token = secrets.token_hex(32)
        self.coordinator_owner = None
        self.operations = threading.Lock()
        self.replacement = Replacement(request, self, __file__)
        self.status = {"phase": "preparing", "operation_id": request["job_id"],
                       "window_uuid": request["window_uuid"], "owner_pid": request["owner_pid"]}
        self.directory = tempfile.TemporaryDirectory(prefix="xfce-update-kwin-", dir=os.environ.get("XDG_RUNTIME_DIR"))
        owner = self.dbus("GetConnectionUnixProcessID", "(s)", [request["owner_bus"]])[0]
        user = self.dbus("GetConnectionUnixUser", "(s)", [request["owner_bus"]])[0]
        if owner != request["owner_pid"] or user != os.getuid():
            raise UpdateError("The update request does not belong to its terminal process")
        self.kwin_owner = self.dbus("GetNameOwner", "(s)", ["org.kde.KWin"])[0]
        node = Gio.DBusNodeInfo.new_for_xml(XML)
        self.registration = self.connection.register_object(PATH, node.interfaces[0], self.method, None, None)
        result = self.dbus("RequestName", "(su)", [request["service"], 4])[0]
        if result != 1:
            raise UpdateError("The private update service name is already in use")

    def call(self, service, path, interface, method, signature="()", values=(), timeout=3000):
        result = self.connection.call_sync(service, path, interface, method,
            self.GLib.Variant(signature, tuple(values)), None,
            self.Gio.DBusCallFlags.NO_AUTO_START, timeout, None)
        return unpack(result)

    def dbus(self, method, signature="()", values=()):
        return self.call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                         method, signature, values)

    def method(self, connection, sender, path, interface, method, parameters, invocation):
        try:
            values = parameters.unpack()
            if method == "KWinResult":
                if sender != self.kwin_owner or len(values[1]) > 65536:
                    raise UpdateError("Untrusted KWin placement reply")
                with self.lock:
                    pending = self.pending.get(values[0])
                    if pending:
                        pending[1].update(json.loads(values[1]))
                        pending[0].set()
                invocation.return_value(None)
                return
            if method == "Operate":
                token, action, payload = values
                if (not secrets.compare_digest(token, self.token) or len(payload) > 2 * 1024 * 1024
                        or (self.coordinator_owner is not None and sender != self.coordinator_owner)):
                    raise UpdateError("Untrusted lifecycle operation")
                if self.dbus("GetConnectionUnixUser", "(s)", [sender])[0] != os.getuid():
                    raise UpdateError("The lifecycle worker belongs to another user")
                self.coordinator_owner = sender
                def execute():
                    try:
                        with self.operations:
                            result = self.operate(action, json.loads(payload))
                        reply = dict(ok=result)
                    except Exception as error:
                        reply = dict(error=str(error))
                    invocation.return_value(self.GLib.Variant("(s)", (json.dumps(reply),)))
                threading.Thread(target=execute, name="terminal-staging-operation", daemon=True).start()
                return
            if method != "GetStatus" and sender != self.request["owner_bus"]:
                raise UpdateError("Only the requesting terminal can confirm or cancel this update")
            if method == "Confirm":
                if self.status["phase"] != "ready":
                    raise UpdateError("The update is not awaiting confirmation")
                if values[0]:
                    self.confirmed.set()
                else:
                    self.cancelled.set()
            elif method == "Cancel":
                if self.status["phase"] in ("preparing", "ready"):
                    self.cancelled.set()
            elif method != "GetStatus":
                raise UpdateError("Unknown update method")
            with self.lock:
                payload = json.dumps(self.status)
            invocation.return_value(self.GLib.Variant("(s)", (payload,)))
        except Exception as error:
            invocation.return_dbus_error("org.xfce.Terminal.Update1.Refused", str(error))

    def operate(self, action, value):
        if action == "Publish":
            if value.get("phase") not in ("preparing", "ready", "running", "closing", "complete", "failed"):
                raise UpdateError("Unknown lifecycle phase")
            self.publish(value)
            return {}
        if action == "AwaitConfirmation":
            self.wait_for_confirmation(300)
            return {}
        if action == "Capture": return self.replacement.capture()
        if action == "Preflight": return self.replacement.preflight()
        if action == "Stage": return self.replacement.stage(value)
        if action == "Hold":
            self.hold_original(True)
            return {}
        if action == "Release": return self.replacement.release()
        if action == "Records": return self.replacement.records()
        if action == "Commit": return self.replacement.commit()
        if action in ("Discard", "Recover"): return self.replacement.recover()
        raise UpdateError("Unknown generic terminal operation")

    def publish(self, status):
        with self.lock:
            self.status = {"operation_id": self.request["job_id"],
                           "window_uuid": self.request["window_uuid"],
                           "owner_pid": self.request["owner_pid"], **status}
            payload = json.dumps(self.status)
        try:
            self.save_status(payload)
        except (OSError, UpdateError) as error:
            # Status persistence is secondary; mandatory recovery is saved by the
            # transaction itself before closing any original window.
            print(f"Could not persist terminal update status: {error}", file=sys.stderr, flush=True)
        if status["phase"] == "failed":
            print(payload, file=sys.stderr, flush=True)

    def coordinator_lost(self):
        # Publish/Commit may have finished while the observer was sleeping or
        # checking the bus owner. Never overwrite a final outcome after success.
        with self.operations:
            if self.status["phase"] in ("complete", "failed"):
                return
            try:
                self.replacement.recover()
            except Exception:
                pass
            self.publish(dict(self.status, phase="failed", original_closed=self.replacement.closed,
                message="The lifecycle worker stopped. The original window and recovery records were retained when possible; no automatic restart was attempted."))

    def save_status(self, payload):
        directory = Path(os.environ.get("XDG_STATE_HOME", str(Path.home() / ".local/state"))) / "xfce4-terminal/updates"
        directory.mkdir(mode=0o700, parents=True, exist_ok=True)
        if directory.is_symlink() or directory.stat().st_uid != os.getuid():
            raise UpdateError("The terminal update status directory is not private")
        temporary = directory / (self.request["job_id"] + ".status.tmp")
        descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o600)
        with os.fdopen(descriptor, "w") as stream:
            stream.write(payload)
        temporary.replace(directory / (self.request["job_id"] + ".status.json"))

    def check_cancelled(self):
        if self.cancelled.is_set():
            raise UpdateError("Update cancelled; original left untouched")

    def wait_for_confirmation(self, seconds):
        deadline = time.monotonic() + seconds
        while not self.confirmed.wait(0.1):
            self.check_cancelled()
            if time.monotonic() >= deadline:
                raise UpdateError("Update confirmation expired; original left untouched")
            if not self.dbus("NameHasOwner", "(s)", [self.request["owner_bus"]])[0]:
                raise UpdateError("The requesting terminal closed before confirmation")

    def records(self, owner):
        try:
            return self.call(owner, TERMINAL_PATH, TERMINAL_INTERFACE, "ListTerminals")[0]
        except Exception as error:
            raise UpdateError(f"Terminal control API is unavailable: {error}") from error

    def terminal_owner(self, pid):
        owners = set()
        for name in self.dbus("ListNames")[0]:
            if not (name == "org.xfce.Terminal5" or name.startswith("org.xfce.Terminal5.Instance.")):
                continue
            try:
                if self.dbus("GetConnectionUnixProcessID", "(s)", [name])[0] == pid:
                    owners.add(self.dbus("GetNameOwner", "(s)", [name])[0])
            except Exception:
                continue
        if len(owners) != 1:
            raise UpdateError(f"Expected one control API for replacement PID {pid}; found {len(owners)}")
        return owners.pop()

    def kwin(self, action, **values):
        nonce = uuid.uuid4().hex
        name = f"xfce-terminal-update-{nonce}"
        event, response = threading.Event(), {}
        with self.lock:
            self.pending[nonce] = (event, response)
        request = {"service": self.connection.get_unique_name(), "path": PATH,
                   "nonce": nonce, "action": action, **values}
        script = Path(self.directory.name) / f"{nonce}.js"
        script.write_text("var request = " + json.dumps(request) + ";\n" +
                          Path(__file__).with_name("kwin.js").read_text())
        loaded = False
        try:
            script_id = self.call(self.kwin_owner, "/Scripting", "org.kde.kwin.Scripting", "loadScript",
                                  "(ss)", [str(script), name])[0]
            if script_id < 0:
                raise UpdateError("KWin refused the terminal placement helper")
            loaded = True
            self.call(self.kwin_owner, f"/Scripting/Script{script_id}", "org.kde.kwin.Script", "run")
            if not event.wait(3):
                raise UpdateError(f"KWin did not answer the {action} request")
            if "error" in response:
                raise UpdateError(response["error"])
            if not isinstance(response.get("ok"), dict):
                raise UpdateError("KWin returned invalid window metadata")
            return response["ok"]
        except self.GLib.Error as error:
            raise UpdateError(f"KWin {action} failed: {error.message}") from error
        finally:
            if loaded:
                try:
                    self.call(self.kwin_owner, "/Scripting", "org.kde.kwin.Scripting", "unloadScript", "(s)", [name])
                except self.GLib.Error:
                    pass
            script.unlink(missing_ok=True)
            with self.lock:
                self.pending.pop(nonce, None)

    def close_original(self, window_uuid):
        self.call(self.request["owner_bus"], TERMINAL_PATH, TERMINAL_INTERFACE, "CloseForUpdate",
                  "(ss)", [window_uuid, self.request["service"]])

    def hold_original(self, enabled):
        self.call(self.request["owner_bus"], TERMINAL_PATH, TERMINAL_INTERFACE, "HoldForUpdate",
                  "(ssb)", [self.request["window_uuid"], self.request["service"], enabled])

    def finish_replacement(self, owner):
        self.call(owner, TERMINAL_PATH, TERMINAL_INTERFACE, "FinishUpdate",
                  "(s)", [self.request["service"]])


def serve(request):
    required = ("owner_bus", "owner_pid", "window_uuid", "window_title", "executable", "launch_path", "launch_kind", "job_id", "service")
    if not isinstance(request, dict) or any(key not in request for key in required):
        raise UpdateError("Incomplete terminal update request")
    uuid.UUID(request["window_uuid"])
    uuid.UUID(request["job_id"])
    expected = "org.xfce.Terminal5.Update.i" + request["job_id"].replace("-", "_")
    if request["service"] != expected or request["launch_kind"] not in ("executable", "repo-wrapper"):
        raise UpdateError("Invalid terminal update service or launch metadata")
    identity(request["owner_pid"])
    backend = Backend(request)
    loop = backend.GLib.MainLoop()
    backend.connection.connect("closed", lambda *arguments: (backend.cancelled.set(), loop.quit()))

    def work():
        try:
            value = dict(request, protocol=1, token=backend.token)
            job_id = backend.call("com.terrydaktal.ApplicationLauncher", "/Lifecycle",
                "com.terrydaktal.ApplicationLauncher.Lifecycle1", "BeginWindowUpdate", "(s)",
                [json.dumps(value)], timeout=12000)[0]
        except Exception as error:
            backend.publish(dict(phase="failed", original_closed=False,
                message=f"Lifecycle backend unavailable: {error}. Start or update applicationlauncherd; no original window was closed."))
            return
        while backend.status["phase"] not in ("failed", "complete"):
            time.sleep(0.2)
            if backend.status["phase"] in ("failed", "complete"):
                break
            if backend.coordinator_owner is None:
                try:
                    report = json.loads(backend.call("com.terrydaktal.ApplicationLauncher", "/Lifecycle",
                        "com.terrydaktal.ApplicationLauncher.Lifecycle1", "GetWindowUpdateReport", "(s)", [job_id])[0])
                    if report.get("phase") == "failed":
                        backend.publish(dict(report, original_closed=False))
                        break
                except Exception:
                    pass
            if backend.coordinator_owner is not None and not backend.dbus("NameHasOwner", "(s)", [backend.coordinator_owner])[0]:
                backend.coordinator_lost()
                break
        if backend.status["phase"] == "failed" and backend.status.get("original_closed"):
            def show_failure():
                import gi
                gi.require_version("Gtk", "3.0")
                from gi.repository import Gtk
                if not Gtk.init_check()[0]:
                    return False
                dialog = Gtk.MessageDialog(message_type=Gtk.MessageType.ERROR, buttons=Gtk.ButtonsType.CLOSE,
                                           text="Update in Place")
                dialog.set_title("Update in Place")
                dialog.format_secondary_text(backend.status["message"] + "\n\nRecovery commands: " +
                                             str(backend.status.get("backup", "unavailable")))
                dialog.connect("response", lambda widget, response: (widget.destroy(), loop.quit()))
                dialog.show_all()
                return False
            backend.GLib.idle_add(show_failure)
            backend.GLib.timeout_add_seconds(300, lambda: (loop.quit(), False)[1])
        else:
            backend.GLib.timeout_add_seconds(60, lambda: (loop.quit(), False)[1])

    threading.Thread(target=work, name="terminal-update", daemon=True).start()
    loop.run()
    backend.replacement.cleanup()
    backend.directory.cleanup()


def main(arguments):
    if len(arguments) >= 4 and arguments[0] == "--gate":
        index, options = 2, {}
        while index < len(arguments) and arguments[index] != "--":
            option = arguments[index]
            if option not in ("--environment", "--executable") or option in options or index + 1 >= len(arguments):
                raise UpdateError("Invalid prepared startup gate options")
            options[option] = arguments[index + 1]
            index += 2
        if index >= len(arguments) - 1:
            raise UpdateError("The prepared startup command is missing")
        return gate(arguments[1], arguments[index + 1:], environment=json.loads(options["--environment"]) if "--environment" in options else None,
                    executable=options.get("--executable"))
    if len(arguments) == 2 and arguments[0] == "--serve" and len(arguments[1]) <= 65536:
        serve(json.loads(arguments[1]))
        return 0
    raise UpdateError("This private helper must be started by xfce4-terminal")


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except Exception as error:
        print(f"xfce4-terminal update: {error}", file=sys.stderr)
        sys.exit(1)
