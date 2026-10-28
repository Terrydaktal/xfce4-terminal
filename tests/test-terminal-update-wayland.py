"""Real terminal/tmux handoff under an isolated, software-rendered KWin."""

import copy
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import uuid

SOURCE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SOURCE / "terminal/update"))


def wait_for(callback, seconds=10):
    deadline = time.monotonic() + seconds
    last = None
    while time.monotonic() < deadline:
        try:
            return callback()
        except Exception as error:
            last = error
            time.sleep(0.05)
    raise RuntimeError(f"Timed out: {last}")


def run_session(binary):
    import gi
    gi.require_version("Gio", "2.0")
    from gi.repository import Gio, GLib
    specification = importlib.util.spec_from_file_location("worker", SOURCE / "terminal/update/terminal-update-helper.py")
    worker = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(worker)
    from core import placement_matches
    root = Path(os.environ["XFCE_UPDATE_TEST_ROOT"])
    log = open(root / "kwin.log", "wb")
    compositor = subprocess.Popen(["kwin_wayland", "--virtual", "--width", "1280", "--height", "800",
        "--no-lockscreen", "--no-global-shortcuts", "--no-kactivities", "--socket", "xfce-update-test"],
        stdout=log, stderr=log)
    original = None
    replacement = helper_pid = None
    socket_path = str(root / "tmux.socket")
    backend = None
    failures = []
    loop = GLib.MainLoop()

    def test():
        nonlocal original, replacement, helper_pid, backend
        try:
            connection = Gio.bus_get_sync(Gio.BusType.SESSION, None)
            def kwin_ready():
                if compositor.poll() is not None:
                    raise RuntimeError("Nested KWin exited: " + (root / "kwin.log").read_text(errors="replace")[-5000:])
                return connection.call_sync("org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting",
                    "isScriptLoaded", GLib.Variant("(s)", ("xfce-test",)), None,
                    Gio.DBusCallFlags.NO_AUTO_START, 500, None)
            wait_for(kwin_ready)
            request_id = str(uuid.uuid4())
            backend = worker.Backend(dict(owner_bus=connection.get_unique_name(), owner_pid=os.getpid(),
                window_uuid=str(uuid.uuid4()), job_id=request_id,
                service="org.xfce.Terminal5.Update.i" + request_id.replace("-", "_")))
            assert not backend.dbus("NameHasOwner", "(s)", ["com.terrydaktal.ApplicationLauncher"])[0]
            subprocess.run(["tmux", "-S", socket_path, "-f", "/dev/null", "new-session", "-d", "-s", "fixture",
                            "/bin/sleep 180"], check=True, timeout=5)
            pane = subprocess.check_output(["tmux", "-S", socket_path, "display-message", "-p", "-t", "fixture",
                                             "#{pane_pid}"], timeout=3).decode().strip()
            # A private copy with a changed ELF note represents the old running build.
            old = root / "old/xfce4-terminal"
            old.parent.mkdir()
            shutil.copyfile(binary, old)
            note = subprocess.check_output(["readelf", "-n", str(old)], timeout=3).decode()
            build_id = bytes.fromhex(re.search(r"Build ID: ([0-9a-f]+)", note).group(1))
            contents = bytearray(old.read_bytes())
            contents[contents.index(build_id)] ^= 1
            old.write_bytes(contents)
            old.chmod(0o700)
            environment = os.environ.copy()
            environment["XFCE4_TERMINAL_LAUNCH_PATH"] = str(binary)
            environment["XFCE4_TERMINAL_LAUNCH_KIND"] = "executable"
            shell_mode = os.environ.get("XFCE_UPDATE_TEST_MODE") == "shell"
            command = shlex.join([shutil.which("bash"), "--noprofile", "--norc"] if shell_mode else
                                 [shutil.which("tmux"), "-N", "-S", socket_path, "attach-session", "-t", "fixture"])
            with open(root / "original.log", "wb") as terminal_log:
                arguments = [str(old), "--disable-server", "--working-directory", str(root), "--command", command]
                if shell_mode:
                    (root / "second-tab").mkdir()
                    arguments.extend(["--tab", "--working-directory", str(root / "second-tab"),
                                      "--active-tab", "--command", command])
                original = subprocess.Popen(arguments, env=environment, stdout=terminal_log, stderr=terminal_log)
            owner = wait_for(lambda: backend.terminal_owner(original.pid))
            def capture():
                records = backend.records(owner)
                if len(records) != (2 if shell_mode else 1) or any(row["foreground_pgid"] <= 1 for row in records):
                    raise RuntimeError("Original tmux tab is not ready: " + json.dumps(records))
                return backend.kwin("capture", pid=original.pid)
            saved = wait_for(capture)
            # Reproduce the reported five-pixel offscreen bottom edge in the private display.
            saved["geometry"]["y"] = saved["output"]["y"] + saved["output"]["height"] - saved["geometry"]["height"] + 5
            for _ in range(2):
                backend.kwin("place", pid=original.pid, id=saved["id"], saved=saved)
                time.sleep(0.1)
            backend.kwin("present", pid=original.pid, id=saved["id"], saved=saved)
            time.sleep(0.2)
            captured = backend.kwin("capture", pid=original.pid, id=saved["id"])
            placement_matches(saved, captured, saved["minimized"])
            window_uuid = backend.records(owner)[0]["window_uuid"]
            def available():
                status = json.loads(backend.call(owner, worker.TERMINAL_PATH, worker.TERMINAL_INTERFACE,
                    "GetUpdateStatus", "(s)", [window_uuid])[0])
                if not status["updatable"]:
                    raise RuntimeError("Old terminal has not detected its replacement yet")
                return status
            wait_for(available)
            service = backend.call(owner, worker.TERMINAL_PATH, worker.TERMINAL_INTERFACE,
                "UpdateInPlace", "(s)", [window_uuid])[0]
            def status():
                return json.loads(backend.call(service, worker.PATH, worker.INTERFACE, "GetStatus")[0])
            last = wait_for(status)
            helper_pid = backend.dbus("GetConnectionUnixProcessID", "(s)", [service])[0]
            deadline = time.monotonic() + 75
            while time.monotonic() < deadline:
                last = status()
                if last["phase"] in ("failed", "complete"):
                    break
                time.sleep(0.1)
            if last["phase"] != "complete":
                raise AssertionError(json.dumps(last))
            replacement = last["replacement_pid"]
            original.wait(timeout=5)
            actual = backend.kwin("capture", pid=replacement, id=last["replacement_window_id"])
            placement_matches(saved, actual, saved["minimized"])
            assert subprocess.check_output(["tmux", "-N", "-S", socket_path, "display-message", "-p", "-t", "fixture",
                                            "#{pane_pid}"], timeout=3).decode().strip() == pane
            replacement_records = backend.records(backend.terminal_owner(replacement))
            assert len(replacement_records) == (2 if shell_mode else 1)
            if shell_mode:
                assert (root / "confirmed").read_text() == "confirmed\n"
                assert [row["active"] for row in replacement_records] == [False, True]
                assert [os.path.realpath(row["working_directory"]) for row in replacement_records] == [str(root), str(root / "second-tab")]
            print(json.dumps(dict(result="passed", mode="shell" if shell_mode else "tmux", old_pid=original.pid, replacement_pid=replacement,
                                  geometry=actual["geometry"], tmux_pane_pid=pane)), flush=True)
        except Exception as error:
            failures.append(str(error))
            print("Wayland update test failed: " + str(error), file=sys.stderr, flush=True)
        finally:
            GLib.idle_add(loop.quit)

    threading.Thread(target=test, daemon=True).start()
    GLib.timeout_add_seconds(100, lambda: (failures.append("overall deadline"), loop.quit(), False)[-1])
    loop.run()
    # Every signalled process and socket below was created in this private test session.
    for pid in (replacement, helper_pid):
        if pid:
            try: os.kill(pid, signal.SIGTERM)
            except ProcessLookupError: pass
    if original is not None and original.poll() is None:
        original.terminate()
        original.wait(timeout=5)
    subprocess.run(["tmux", "-N", "-S", socket_path, "kill-server"], timeout=3,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    compositor.terminate()
    compositor.wait(timeout=5)
    log.close()
    if backend:
        backend.directory.cleanup()
    return 1 if failures else 0


def main():
    binary = Path(sys.argv[-1]).resolve()
    if "--session" in sys.argv:
        return run_session(binary)
    if not shutil.which("kwin_wayland") or not shutil.which("tmux"):
        print("Requires KWin Wayland and tmux", file=sys.stderr)
        return 77
    with tempfile.TemporaryDirectory(prefix="xfce-wayland-") as directory:
        root = Path(directory)
        environment = os.environ.copy()
        for key in ("DISPLAY", "WAYLAND_DISPLAY", "TMUX", "TMUX_PANE", "XDG_ACTIVATION_TOKEN", "DESKTOP_STARTUP_ID"):
            environment.pop(key, None)
        for key, name in (("HOME", "home"), ("XDG_RUNTIME_DIR", "runtime"), ("XDG_CONFIG_HOME", "config"),
                          ("XDG_DATA_HOME", "data"), ("XDG_STATE_HOME", "state"), ("XDG_CACHE_HOME", "cache")):
            path = root / name
            path.mkdir(mode=0o700)
            environment[key] = str(path)
        front = root / "bin"
        front.mkdir()
        scope = front / "systemd-run"
        scope.write_text("#!/usr/bin/python3\nimport os,sys\na=sys.argv[sys.argv.index('--')+1:]\nos.execvp(a[0],a)\n")
        scope.chmod(0o700)
        environment.update(PATH=str(front) + ":" + environment["PATH"], XFCE_UPDATE_TEST_ROOT=str(root),
            QT_QPA_PLATFORM="offscreen", GDK_BACKEND="wayland", WAYLAND_DISPLAY="xfce-update-test",
            LIBGL_ALWAYS_SOFTWARE="1", KWIN_COMPOSE="Q", XDG_CURRENT_DESKTOP="KDE", NO_AT_BRIDGE="1",
            GIO_USE_VFS="local", GVFS_DISABLE_FUSE="1",
            __EGL_VENDOR_LIBRARY_FILENAMES="/usr/share/glvnd/egl_vendor.d/50_mesa.json")
        if "--shell" in sys.argv:
            module = sys.argv[sys.argv.index("--confirm-module") + 1]
            environment.update(XFCE_UPDATE_TEST_MODE="shell", GTK3_MODULES=str(Path(module).resolve()),
                               XFCE_UPDATE_TEST_CONFIRM_LOG=str(root / "confirmed"))
        result = subprocess.run(["dbus-run-session", "--", sys.executable, __file__, "--session", str(binary)],
                                env=environment, timeout=110)
        if result.returncode:
            for name in ("kwin.log", "original.log"):
                path = root / name
                if path.exists(): print(path.read_text(errors="replace")[-5000:], file=sys.stderr)
            for path in (root / "state/xfce4-terminal/updates").glob("*.log"):
                print(path.read_text(errors="replace")[-5000:], file=sys.stderr)
        return result.returncode


if __name__ == "__main__":
    sys.exit(main())
