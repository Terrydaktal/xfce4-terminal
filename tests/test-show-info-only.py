"""Check the real Show Info GUI under a disposable KWin and session bus."""

import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time


def wait_for(callback, seconds=20):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        result = callback()
        if result:
            return result
        time.sleep(0.05)
    raise AssertionError("Timed out waiting for " + callback.__name__)


def run_session(launcher, terminal):
    import gi

    gi.require_version("Gio", "2.0")
    from gi.repository import Gio, GLib

    root = Path(os.environ["SHOW_INFO_TEST_ROOT"])
    children = []
    logs = []

    def spawn(name, argv):
        log = open(root / (name + ".log"), "wb")
        logs.append(log)
        child = subprocess.Popen(argv, stdout=log, stderr=log)
        children.append(child)
        return child

    def command(*argv):
        return subprocess.run(argv, capture_output=True, text=True, timeout=8)

    def windows(child):
        assert child.poll() is None, "GUI exited before its information window opened"
        result = command("kdotool", "search", "--all", "--pid", str(child.pid), "--title", "")
        return {
            wid: command("kdotool", "getwindowname", wid).stdout.strip()
            for wid in result.stdout.splitlines()
            if wid.strip()
        }

    def info_windows(child):
        titles = windows(child)
        assert "Applicationlauncher" not in titles.values(), titles
        return titles

    def ready_info(child):
        titles = info_windows(child)
        return titles if titles and all("Window Info:" in title for title in titles.values()) else None

    def close_info(child, ids):
        for wid in ids:
            result = command("kdotool", "windowclose", wid)
            assert result.returncode == 0, result.stderr
        child.wait(timeout=12)
        assert child.returncode == 0

    try:
        compositor = spawn("kwin", ["kwin_wayland", "--virtual", "--width", "1280", "--height", "800",
                                   "--no-lockscreen", "--no-global-shortcuts", "--no-kactivities",
                                   "--socket", "show-info-test"])
        connection = Gio.bus_get_sync(Gio.BusType.SESSION, None)

        def compositor_ready():
            assert compositor.poll() is None, "Private KWin exited"
            try:
                return connection.call_sync("org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting",
                                            "isScriptLoaded", GLib.Variant("(s)", ("info-test",)),
                                            None, Gio.DBusCallFlags.NO_AUTO_START, 500, None)
            except GLib.Error:
                return None

        wait_for(compositor_ready)
        daemon = launcher.with_name("applicationlauncherd")
        if daemon.is_file():
            spawn("daemon", [str(daemon)])
        term = spawn("terminal", [str(terminal), "--disable-server", "--title=Info Fixture",
                                  "--execute", "/bin/sleep", "120"])

        def terminal_record():
            assert term.poll() is None, "Fixture terminal exited"
            names = connection.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus",
                                         "org.freedesktop.DBus", "ListNames", None, None, 0, 1000, None).unpack()[0]
            for name in names:
                if not name.startswith("org.xfce.Terminal5.Instance."):
                    continue
                records = connection.call_sync(name, "/org/xfce/Terminal", "org.xfce.Terminal5",
                                               "ListTerminals", None, None, 0, 1000, None).unpack()[0]
                if records:
                    return records[0]
            return None

        record = wait_for(terminal_record)
        wait_for(lambda: windows(term))
        request = [str(launcher), "--show-terminal-info", str(term.pid), record["window_uuid"]]

        app = spawn("success", request)
        ids = wait_for(lambda: ready_info(app))
        assert all("Window Info:" in title for title in ids.values()), ids
        forwarded = command(*request)
        assert forwarded.returncode == 0, forwarded.stderr
        assert set(wait_for(lambda: ready_info(app))) == set(ids), "Duplicate request opened another window"
        close_info(app, ids)
        print("PASS: popup-only startup, duplicate forwarding and host cleanup")

        app = spawn("error", [str(launcher), "--show-terminal-info", "2147483647", record["window_uuid"]])
        ids = wait_for(lambda: info_windows(app))
        assert list(ids.values()) == ["Show Info"], ids
        close_info(app, ids)
        print("PASS: failed requests show only an information window")

        app = spawn("reveal", request)
        ids = wait_for(lambda: ready_info(app))
        result = command(str(launcher))
        assert result.returncode == 0, result.stderr
        promoted = wait_for(lambda: windows(app) if len(windows(app)) >= 2 else None)
        assert "Applicationlauncher" in promoted.values(), promoted
        ids = [wid for wid, title in promoted.items() if title.startswith("Window Info:")]
        assert ids, promoted
        for wid in ids:
            assert command("kdotool", "windowclose", wid).returncode == 0
        time.sleep(0.2)
        assert app.poll() is None, "Opening the normal launcher must keep it alive"
        assert command(str(launcher), "--shutdown").returncode == 0
        app.wait(timeout=12)
        print("PASS: the normal launcher shortcut reveals and retains the host")

        app = spawn("existing", [str(launcher)])
        ids = wait_for(lambda: windows(app))
        assert "Applicationlauncher" in ids.values(), ids
        assert command(*request).returncode == 0
        wait_for(lambda: len(windows(app)) > len(ids))
        assert set(ids).issubset(windows(app)), "Existing launcher window was closed or hidden"
        assert command(str(launcher), "--shutdown").returncode == 0
        app.wait(timeout=12)
        print("PASS: existing launcher windows are preserved")
        return 0
    except Exception:
        for log in logs:
            log.flush()
        for path in root.glob("*.log"):
            print(f"--- {path.name} ---\n{path.read_text(errors='replace')[-3000:]}", file=sys.stderr)
        raise
    finally:
        for child in reversed(children):
            if child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait()
        for log in logs:
            log.close()


def main():
    if len(sys.argv) != 3:
        raise SystemExit("Usage: test-show-info-only.py LAUNCHER_BINARY TERMINAL_BINARY")
    launcher, terminal = [Path(value).resolve() for value in sys.argv[1:]]
    if os.environ.get("SHOW_INFO_TEST_SESSION") == "1":
        return run_session(launcher, terminal)
    if not all(shutil.which(tool) for tool in ("kwin_wayland", "kdotool", "dbus-run-session")):
        return 77
    with tempfile.TemporaryDirectory(prefix="show-info-") as directory:
        root = Path(directory)
        environment = os.environ.copy()
        for name in ("DISPLAY", "WAYLAND_DISPLAY", "XDG_ACTIVATION_TOKEN", "DESKTOP_STARTUP_ID", "TMUX", "TMUX_PANE"):
            environment.pop(name, None)
        for name, suffix in (("HOME", "home"), ("XDG_RUNTIME_DIR", "runtime"), ("XDG_CONFIG_HOME", "config"),
                             ("XDG_DATA_HOME", "data"), ("XDG_STATE_HOME", "state"), ("XDG_CACHE_HOME", "cache")):
            path = root / suffix
            path.mkdir(mode=0o700)
            environment[name] = str(path)
        environment.update(SHOW_INFO_TEST_ROOT=str(root), SHOW_INFO_TEST_SESSION="1",
                           QT_QPA_PLATFORM="offscreen", GDK_BACKEND="wayland", WAYLAND_DISPLAY="show-info-test",
                           KWIN_COMPOSE="O2", XDG_CURRENT_DESKTOP="KDE", NO_AT_BRIDGE="1",
                           GIO_USE_VFS="local", GVFS_DISABLE_FUSE="1")
        environment.pop("LIBGL_ALWAYS_SOFTWARE", None)
        environment.pop("__EGL_VENDOR_LIBRARY_FILENAMES", None)
        child = subprocess.Popen(["dbus-run-session", "--", sys.executable, __file__, str(launcher), str(terminal)],
                                 env=environment, start_new_session=True)
        try:
            return child.wait(timeout=120)
        except subprocess.TimeoutExpired:
            os.killpg(child.pid, signal.SIGTERM)
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(child.pid, signal.SIGKILL)
                child.wait()
            raise


if __name__ == "__main__":
    sys.exit(main())
