"""Terminal-owned update transaction; independent of the old GTK process."""

import fcntl
import hashlib
import json
import os
from pathlib import Path
import shlex
import socket
import struct
import subprocess
import sys
import tempfile
import termios
import time

from processes import UpdateError, alive, capture_tab, identity, same_tabs, tmux_query


def digest(path):
    with open(path, "rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def verify_binary(pid, expected):
    actual = os.stat(f"/proc/{pid}/exe")
    if (actual.st_dev, actual.st_ino) != expected:
        raise UpdateError("The replacement is not running the pinned terminal executable")


def resolve_launch(path, kind):
    target = Path(path).resolve(strict=True)
    if kind == "repo-wrapper" and target.name == "run-patched-xfce4-terminal.sh":
        target = (target.parent / "build/terminal/xfce4-terminal").resolve(strict=True)
    return str(target)


def exact_output(saved, current):
    if not saved or any(saved.get(key, 0) <= 0 for key in ("width", "height", "scale")):
        raise UpdateError("Saved monitor geometry is unavailable")
    if saved != current:
        raise UpdateError("Monitor geometry or scale changed; original left untouched")


def placement_matches(saved, actual, minimized):
    exact_output(saved.get("output"), actual.get("output"))
    expected = saved["normal"] if saved["maximized"] or saved["fullscreen"] else saved["geometry"]
    found = actual["normal"] if saved["maximized"] or saved["fullscreen"] else actual["geometry"]
    if not expected or not found or any(abs(expected[key] - found[key]) > 2 for key in ("x", "y", "width", "height")):
        raise UpdateError(f"Replacement geometry differs: expected {expected}, received {found}")
    for key in ("maximized", "fullscreen", "desktops", "activities", "keepAbove", "keepBelow",
                "noBorder", "shaded", "skipTaskbar", "skipPager", "skipSwitcher"):
        if saved[key] != actual[key]:
            raise UpdateError(f"Replacement window state differs: {key}")
    if actual["minimized"] != minimized:
        raise UpdateError("Replacement minimization state differs")


def terminal_arguments(tabs, gate_commands, columns=0, rows=0):
    # --window appends a second window to the initial standalone window.
    arguments = ["--disable-server", "--minimize"]
    if columns > 0 and rows > 0:
        arguments.append(f"--geometry={columns}x{rows}")
    for index, (tab, command) in enumerate(zip(tabs, gate_commands, strict=True)):
        if index:
            arguments.append("--tab")
        arguments.extend(["--working-directory", tab["cwd"]])
        if tab["active"]:
            arguments.append("--active-tab")
        arguments.extend(["--command", shlex.join(command)])
    return arguments


class StartupGates:
    """Private sockets authenticate the actual child PID, not a guessed shell."""

    def __init__(self, directory, tabs):
        self.sockets = []
        self.connections = {}
        for index, tab in enumerate(tabs):
            if tab.get("tmux"):
                self.sockets.append(None)
                continue
            listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            listener.settimeout(0.05)
            listener.bind(str(Path(directory) / f"gate-{index}"))
            os.chmod(listener.getsockname(), 0o600)
            listener.listen(1)
            self.sockets.append(listener)

    def commands(self, tabs, helper):
        return [tab["argv"] if listener is None else
                [sys.executable, helper, "--gate", listener.getsockname(), "--", *tab["argv"]]
                for listener, tab in zip(self.sockets, tabs, strict=True)]

    def ready(self, records):
        if len(records) != len(self.sockets):
            return False
        # A connected gate may precede VTE's async spawn PID publication. Do not
        # consume (and then reject) a valid connection against an unknown PID.
        if any(not isinstance(record.get("child_pid"), int) or record["child_pid"] <= 1 for record in records):
            return False
        for index, (listener, record) in enumerate(zip(self.sockets, records, strict=True)):
            if listener is None:
                continue
            if index not in self.connections:
                try:
                    connection, _ = listener.accept()
                except socket.timeout:
                    return False
                pid, uid, _ = struct.unpack("3i", connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
                if uid != os.getuid() or pid != record["child_pid"]:
                    connection.close()
                    raise UpdateError("Replacement startup child identity did not match its private gate")
                self.connections[index] = connection
        return True

    def release(self):
        if len(self.connections) != sum(listener is not None for listener in self.sockets):
            raise UpdateError("Replacement startup gates are not ready")
        for connection in self.connections.values():
            connection.sendall(b"S")

    def close(self):
        for connection in self.connections.values():
            connection.close()
        for listener in self.sockets:
            if listener is None:
                continue
            name = listener.getsockname()
            listener.close()
            Path(name).unlink(missing_ok=True)


def gate(path, arguments):
    if not arguments or not os.path.isabs(arguments[0]):
        raise UpdateError("A fixed executable is required for terminal startup")
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(120)
        connection.connect(path)
        if connection.recv(1) != b"S":
            return 125
    # Old desktop activation tokens must not leak into restarted applications.
    environment = os.environ.copy()
    environment.pop("XDG_ACTIVATION_TOKEN", None)
    environment.pop("DESKTOP_STARTUP_ID", None)
    environment.pop("XFCE4_TERMINAL_UPDATE_SERVICE", None)
    os.execve(arguments[0], arguments, environment)


class UpdateJob:
    def __init__(self, request, backend, helper):
        self.request = request
        self.backend = backend
        self.helper = str(Path(helper).resolve())
        self.closed = False
        self.replacement = None
        self.backup = None

    def publish(self, phase, message, **values):
        self.backend.publish({"phase": phase, "message": message, "original_closed": self.closed,
                              "backup": str(self.backup) if self.backup else None, **values})

    def original_records(self):
        records = self.backend.records(self.request["owner_bus"])
        selected = [record for record in records if record["window_uuid"] == self.request["window_uuid"]]
        if not 0 < len(selected) <= 32 or sum(bool(tab["active"]) for tab in selected) != 1:
            raise UpdateError("The window's tabs or active tab cannot be identified uniquely")
        if any(not tab.get("input_enabled", True) for tab in selected):
            raise UpdateError("A terminal tab has input disabled; original left untouched")
        return selected

    def preflight(self, records, tabs, owner, saved, binary, pinned):
        if not alive(owner) or not same_tabs(records, self.original_records()):
            raise UpdateError("Original terminal tabs or process changed; original left untouched")
        current = os.stat(binary)
        if (current.st_dev, current.st_ino, current.st_size, current.st_mtime_ns) != pinned:
            raise UpdateError("The latest terminal binary changed after preview")
        if resolve_launch(self.request["launch_path"], self.request["launch_kind"]) != binary:
            raise UpdateError("The terminal launch entry changed after preview")
        current_window = self.backend.kwin("capture", pid=owner["pid"], id=saved["id"])
        placement_matches(saved, current_window, saved["minimized"])
        for tab in tabs:
            if not alive(tab["process"]):
                raise UpdateError("An original application changed after preview")
            if tab.get("tmux"):
                session = tab["tmux"]
                if not alive(session["server"]):
                    raise UpdateError("The tmux server changed after preview")
                created = tmux_query(tab["argv"][0], session["socket"], "display-message", "-p", "-t",
                                     session["session"], "#{session_created}").strip()
                if created != session["created"]:
                    raise UpdateError("The tmux session changed after preview")

    def save_backup(self, tabs, saved, binary):
        base = Path(os.environ.get("XDG_STATE_HOME", str(Path.home() / ".local/state"))) / "xfce4-terminal/updates"
        base.mkdir(mode=0o700, parents=True, exist_ok=True)
        if base.stat().st_uid != os.getuid() or base.is_symlink():
            raise UpdateError("The terminal update backup directory is not private")
        os.chmod(base, 0o700)
        self.backup = base / f"{self.request['job_id']}.json"
        descriptor = os.open(self.backup, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        with os.fdopen(descriptor, "w") as stream:
            json.dump({"schema": 1, "executable": binary, "window": saved,
                       "tabs": [{key: tab[key] for key in ("cwd", "argv", "active", "kind")} for tab in tabs]}, stream)
            stream.flush()
            os.fsync(stream.fileno())
        directory = os.open(base, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)

    def run(self):
        gates = None
        descriptor = None
        directory = None
        try:
            self.publish("preparing", "Inspecting this terminal's tabs and window placement")
            owner = identity(self.request["owner_pid"])
            binary = str(Path(self.request["executable"]).resolve(strict=True))
            descriptor = os.open(binary, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
            metadata = os.fstat(descriptor)
            pinned = (metadata.st_dev, metadata.st_ino, metadata.st_size, metadata.st_mtime_ns)
            if os.pread(descriptor, 4, 0) != b"\x7fELF" or not os.access(binary, os.X_OK):
                raise UpdateError("The replacement is not an executable ELF terminal")
            if digest(binary) == digest(f"/proc/{owner['pid']}/exe"):
                raise UpdateError("This terminal is already running the latest binary")
            records = self.original_records()
            tabs = [capture_tab(record) for record in records]
            saved = self.backend.kwin("capture", pid=owner["pid"], title=self.request["window_title"])
            exact_output(saved.get("output"), saved.get("output"))
            if not saved.get("geometry") or any(saved["geometry"].get(key, 0) <= 0 for key in ("width", "height")):
                raise UpdateError("The original window's geometry is unavailable")
            if (saved["maximized"] or saved["fullscreen"]) and not saved.get("normal"):
                raise UpdateError("The maximized window's normal geometry is unavailable")
            preserves = all(tab.get("tmux") for tab in tabs)
            description = "\n".join(f"{tab['kind']}: {tab['cwd']}" for tab in tabs)
            self.publish("ready", description, preserves_processes=preserves)
            self.backend.wait_for_confirmation(300)
            self.preflight(records, tabs, owner, saved, binary, pinned)
            self.save_backup(tabs, saved, binary)
            self.publish("running", "Preparing the latest terminal behind the original window")
            directory = tempfile.TemporaryDirectory(prefix="xfce-update-", dir=os.environ.get("XDG_RUNTIME_DIR"))
            gates = StartupGates(directory.name, tabs)
            commands = gates.commands(tabs, self.helper)
            columns = rows = 0
            try:
                active = next(record for record in records if record["active"])
                pty = os.open(active["pty"], os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
                try:
                    rows, columns, _, _ = struct.unpack("HHHH", fcntl.ioctl(pty, termios.TIOCGWINSZ, b"\0" * 8))
                finally:
                    os.close(pty)
            except OSError:
                pass
            arguments = [binary, *terminal_arguments(tabs, commands, columns, rows)]
            environment = os.environ.copy()
            environment.pop("XDG_ACTIVATION_TOKEN", None)
            environment.pop("DESKTOP_STARTUP_ID", None)
            environment["XFCE4_TERMINAL_LAUNCH_PATH"] = self.request["launch_path"]
            environment["XFCE4_TERMINAL_LAUNCH_KIND"] = self.request["launch_kind"]
            environment["XFCE4_TERMINAL_UPDATE_SERVICE"] = self.request["service"]
            log = open(Path(directory.name) / "replacement.log", "wb+")
            try:
                self.replacement = subprocess.Popen(arguments, executable=f"/proc/self/fd/{descriptor}",
                    pass_fds=(descriptor,), env=environment, stdin=subprocess.DEVNULL,
                    stdout=log, stderr=log, start_new_session=True)
                replacement_owner = None
                replacement_window = None
                last_error = "The replacement has not registered its terminal control API"
                verified_at = None
                deadline = time.monotonic() + 30
                while time.monotonic() < deadline:
                    self.backend.check_cancelled()
                    if self.replacement.poll() is not None:
                        log.seek(0)
                        detail = log.read(8192).decode(errors="replace").strip()
                        raise UpdateError(f"Replacement exited ({self.replacement.returncode}): {detail}")
                    try:
                        if replacement_owner is None:
                            replacement_owner = self.backend.terminal_owner(self.replacement.pid)
                        verify_binary(self.replacement.pid, pinned[:2])
                        actual_records = self.backend.records(replacement_owner)
                        if len(actual_records) != len(tabs) or sum(bool(row["active"]) for row in actual_records) != 1:
                            raise UpdateError(f"Replacement has {len(actual_records)} tabs in "
                                              f"{len({row['window_uuid'] for row in actual_records})} windows, "
                                              f"{sum(bool(row['active']) for row in actual_records)} active; "
                                              f"expected {len(tabs)} tabs in one window")
                        for row, tab in zip(actual_records, tabs):
                            if os.path.realpath(row["working_directory"]) != tab["cwd"] or row["active"] != tab["active"]:
                                raise UpdateError("Replacement tab working directory or order differs")
                            if tab.get("tmux"):
                                attached = capture_tab(row)
                                if attached.get("tmux") != tab["tmux"]:
                                    raise UpdateError("The replacement did not reattach to the exact live tmux session")
                        if not gates.ready(actual_records):
                            raise UpdateError("Waiting for replacement tab startup gates")
                        replacement_window = self.backend.kwin("place", pid=self.replacement.pid, saved=saved)
                        # Geometry updates are asynchronous: inspect the result in a separate call.
                        actual = self.backend.kwin("capture", pid=self.replacement.pid, id=replacement_window["id"])
                        placement_matches(saved, actual, True)
                        if verified_at is None:
                            verified_at = time.monotonic()
                        if time.monotonic() - verified_at < 0.25:
                            time.sleep(0.1)
                            continue
                        break
                    except (UpdateError, OSError) as error:
                        last_error = str(error)
                        verified_at = None
                        time.sleep(0.1)
                else:
                    raise UpdateError(f"Replacement not ready after 30 seconds: {last_error}. Original left untouched")
                self.preflight(records, tabs, owner, saved, binary, pinned)
                self.publish("closing", "Replacement verified; closing only the original terminal window")
                self.backend.close_original(self.request["window_uuid"])
                self.closed = True
                deadline = time.monotonic() + 30
                while any(alive(tab["process"]) for tab in tabs):
                    if time.monotonic() >= deadline:
                        raise UpdateError("Original applications did not exit; replacement commands were not released")
                    time.sleep(0.05)
                # Every original foreground process (including old tmux clients) has exited.
                self.backend.finish_replacement(replacement_owner)
                gates.release()
                actual = self.backend.kwin("present", pid=self.replacement.pid, id=replacement_window["id"], saved=saved)
                placement_matches(saved, actual, saved["minimized"])
                deadline = time.monotonic() + 30
                while time.monotonic() < deadline:
                    if self.replacement.poll() is not None:
                        raise UpdateError("The replacement terminal exited during application resume; recovery commands were retained")
                    try:
                        resumed = self.backend.records(replacement_owner)
                        if len(resumed) != len(tabs):
                            raise UpdateError("The resumed terminal tab count differs")
                        for record, tab in zip(resumed, tabs):
                            plan = capture_tab(record)
                            if plan["kind"] != tab["kind"] or plan["argv"] != tab["argv"] or plan["cwd"] != tab["cwd"]:
                                raise UpdateError("A replacement application did not resume the expected session")
                        break
                    except (OSError, UpdateError) as error:
                        last_error = str(error)
                        time.sleep(0.1)
                else:
                    raise UpdateError(f"Application resume did not verify: {last_error}. Recovery commands were retained")
                self.publish("complete", "This terminal window was updated in place",
                             replacement_pid=self.replacement.pid, replacement_window_id=actual["id"])
            finally:
                log.close()
        except Exception as error:
            self.publish("failed", str(error))
        finally:
            if gates:
                gates.close()
            if descriptor is not None:
                os.close(descriptor)
            if directory:
                directory.cleanup()
