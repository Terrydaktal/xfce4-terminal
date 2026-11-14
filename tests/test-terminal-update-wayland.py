"""Real terminal/tmux handoff under an isolated, software-rendered KWin."""

import copy
import hashlib
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
import traceback
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
    daemon_log = open(root / "lifecycle-daemon.log", "wb")
    daemon_binary = os.environ.get("XFCE_LIFECYCLE_DAEMON") or shutil.which("applicationlauncherd")
    if not daemon_binary:
        raise RuntimeError("The shared lifecycle integration requires applicationlauncherd")
    daemon = subprocess.Popen([daemon_binary], stdout=daemon_log, stderr=daemon_log)
    original = None
    replacement = helper_pid = suspended_application = None
    socket_path = str(root / "tmux.socket")
    backend = None
    failures = []
    loop = GLib.MainLoop()

    def test():
        nonlocal original, replacement, helper_pid, suspended_application, backend, daemon
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
            def lifecycle_ready():
                result = backend.call("com.terrydaktal.ApplicationLauncher", "/Lifecycle",
                    "com.terrydaktal.ApplicationLauncher.Lifecycle1", "GetCapabilities")[0]
                assert json.loads(result)["protocol"] == 1
                return True
            wait_for(lifecycle_ready)
            external_mode = os.environ.get("XFCE_LIFECYCLE_TEST_EXTERNAL") == "1"
            if external_mode:
                lifecycle_source = Path(daemon_binary).resolve().parents[2] / "src/lifecycle"
                for module_name, filename in (("lifecycle_worker", "worker.py"), ("lifecycle_window", "window.py"), ("lifecycle_external", "external.py")):
                    spec = importlib.util.spec_from_file_location(module_name, lifecycle_source / filename)
                    module = importlib.util.module_from_spec(spec)
                    sys.modules[module_name] = module
                    spec.loader.exec_module(module)
                external = sys.modules["lifecycle_external"]
            subprocess.run(["tmux", "-S", socket_path, "-f", "/dev/null", "new-session", "-d", "-s", "fixture",
                            "/bin/sleep 180"], check=True, timeout=5)
            pane = subprocess.check_output(["tmux", "-S", socket_path, "display-message", "-p", "-t", "fixture",
                                             "#{pane_pid}"], timeout=3).decode().strip()
            # A private copy with a changed ELF note represents the old running build.
            old = root / "old/xfce4-terminal"
            old.parent.mkdir()
            shutil.copyfile(os.environ.get("XFCE_LIFECYCLE_TEST_LEGACY_BINARY", str(binary)), old)
            note = subprocess.check_output(["readelf", "-n", str(old)], timeout=3).decode()
            build_id = bytes.fromhex(re.search(r"Build ID: ([0-9a-f]+)", note).group(1))
            contents = bytearray(old.read_bytes())
            contents[contents.index(build_id)] ^= 1
            if external_mode and "XFCE_LIFECYCLE_TEST_LEGACY_BINARY" not in os.environ:
                contents = contents.replace(b"GetUpdateStatus", b"OldUpdateStatus")
            if os.environ.get("XFCE_LIFECYCLE_TEST_NO_TAB_API") == "1":
                contents = contents.replace(b"ListTerminals", b"LostTerminals")
            if os.environ.get("XFCE_LIFECYCLE_TEST_NO_HOLD_API") == "1":
                contents = contents.replace(b"HoldForUpdate", b"LostForUpdate")
            old.write_bytes(contents)
            old.chmod(0o700)
            environment = os.environ.copy()
            environment["XFCE4_TERMINAL_LAUNCH_PATH"] = str(binary)
            environment["XFCE4_TERMINAL_LAUNCH_KIND"] = "executable"
            shell_mode = os.environ.get("XFCE_UPDATE_TEST_MODE") == "shell"
            title_mode = os.environ.get("XFCE_UPDATE_TEST_MODE") == "titles"
            codex_mode = os.environ.get("XFCE_UPDATE_TEST_MODE") == "codex"
            multiple_tabs = shell_mode or title_mode
            titles = ["htop ~", "nvtop ~/literal-%D 'quoted'"]
            command = shlex.join([shutil.which("bash"), "--noprofile", "--norc"] if shell_mode else
                                 [shutil.which("tmux"), "-N", "-S", socket_path, "attach-session", "-t", "fixture"])
            tmux_shell = os.environ.get("XFCE_LIFECYCLE_TEST_TMUX_SHELL") == "1"
            if tmux_shell:
                command = shlex.join([shutil.which("bash"), "--noprofile", "--norc", "-i", "-c", command + "; exec bash --noprofile --norc"])
            title_commands = []
            codex_arguments = None
            if codex_mode:
                fixture = Path(os.environ.get("XFCE_LIFECYCLE_CODEX_FIXTURE", str(Path(daemon_binary).resolve().parents[2] / "tests/fixtures/application_update_codex.c")))
                codex_binary = root / "bin/codex"
                subprocess.run(["cc", "-O2", str(fixture), "-o", str(codex_binary)], check=True, timeout=15)
                with codex_binary.open("rb") as stream:
                    original_codex_hash = hashlib.file_digest(stream, "sha256").hexdigest()
                main_id = "12345678-1234-5678-9abc-123456789abc"
                logs = []
                for index in range(4):
                    identity = main_id if index == 0 else f"abcdef0{index}-1234-5678-9abc-123456789abc"
                    payload = dict(id=identity, source="cli")
                    if index:
                        payload.update(parent_thread_id=main_id, source=dict(subagent=dict(thread_spawn=dict(parent_thread_id=main_id, depth=1))))
                    path = root / f"rollout-fixture-{index}.jsonl"
                    path.write_text(json.dumps(dict(type="session_meta", payload=payload)) + "\n")
                    logs.append(str(path))
                environment["APPLICATION_UPDATE_FIXTURE_LOGS"] = ":".join(logs)
                if os.environ.get("XFCE_LIFECYCLE_TEST_RESTART") == "1":
                    environment["APPLICATION_UPDATE_FIXTURE_GRACEFUL_EXIT"] = "1"
                codex_arguments = [str(codex_binary), "resume", main_id, "--model", "fixture-model", "--profile", "fixture-profile",
                                   "--config", 'approval_policy="never"', "--dangerously-bypass-approvals-and-sandbox"]
                command = shlex.join(codex_arguments)
                if os.environ.get("XFCE_LIFECYCLE_TEST_CODEX_IN_FISH") == "1":
                    fish = os.environ.get("XFCE_LIFECYCLE_TEST_FISH") or shutil.which("fish")
                    assert fish, "A real Fish executable is required"
                    command = shlex.join([fish, "--no-config", "--interactive", "--init-command", command])
            if title_mode:
                for name, title in zip(("htop", "nvtop"), titles, strict=True):
                    program = root / name
                    shutil.copyfile(binary.parent.parent / "tests/application-monitor-child", program)
                    program.chmod(0o700)
                    title_commands.append(shlex.join([shutil.which("sh"), "-c",
                        'printf "\\033]2;%s\\007" "$1"; exec "$2"', "title-fixture", title, str(program)]))
                command = title_commands[0]
            with open(root / "original.log", "wb") as terminal_log:
                arguments = [str(old), "--disable-server", "--working-directory", str(root), "--command", command]
                if multiple_tabs:
                    (root / "second-tab").mkdir()
                    arguments.extend(["--tab", "--working-directory", str(root / "second-tab"),
                                      "--active-tab", "--command", title_commands[1] if title_mode else command])
                original = subprocess.Popen(arguments, env=environment, stdout=terminal_log, stderr=terminal_log)
            owner = wait_for(lambda: backend.terminal_owner(original.pid))
            def original_records():
                try: return backend.records(owner)
                except Exception:
                    if not external_mode: raise
                    return [external.proc_record(original.pid, "original")]
            def capture():
                records = original_records()
                if len(records) != (2 if multiple_tabs else 1) or any(row["foreground_pgid"] <= 1 for row in records):
                    raise RuntimeError("Original tmux tab is not ready: " + json.dumps(records))
                if title_mode and [row.get("terminal_title") for row in records] != titles:
                    raise RuntimeError("Original per-tab titles are not ready")
                if tmux_shell:
                    _, active = external.worker.foreground(records[0]["child_pid"], records[0]["foreground_pgid"])
                    assert external.worker.kind(active) == "tmux", "The shell has not started its tmux client"
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
            window_uuid = original_records()[0]["window_uuid"]
            if codex_mode:
                original_codex_pid = original_records()[0]["child_pid"]
                if os.environ.get("XFCE_LIFECYCLE_TEST_CODEX_IN_FISH") == "1":
                    def codex_child():
                        candidates = Path(f"/proc/{original_codex_pid}/task/{original_codex_pid}/children").read_text().split()
                        matches = [int(pid) for pid in candidates
                            if os.path.realpath(f"/proc/{pid}/exe") == str(codex_binary)]
                        assert len(matches) == 1, "Waiting for Codex under the original Fish"
                        return matches[0]
                    original_codex_pid = wait_for(codex_child)
                if os.environ.get("XFCE_LIFECYCLE_TEST_LATE_EXIT") == "1":
                    suspended_application = os.pidfd_open(original_codex_pid)
                    signal.pidfd_send_signal(suspended_application, signal.SIGSTOP)
                newer = root / "bin/newer-codex"
                shutil.copyfile(codex_binary, newer)
                note = subprocess.check_output(["readelf", "-n", str(newer)], timeout=3).decode()
                build_id = bytes.fromhex(re.search(r"Build ID: ([0-9a-f]+)", note).group(1))
                contents = bytearray(newer.read_bytes())
                contents[contents.index(build_id)] ^= 1
                newer.write_bytes(contents)
                newer.chmod(0o700)
                newer.replace(codex_binary)
            def available():
                status = json.loads(backend.call(owner, worker.TERMINAL_PATH, worker.TERMINAL_INTERFACE,
                    "GetUpdateStatus", "(s)", [window_uuid])[0])
                if not status["updatable"]:
                    raise RuntimeError("Old terminal has not detected its replacement yet")
                return status
            if not external_mode: wait_for(available)
            already_confirmed = external_mode or os.environ.get("XFCE_LIFECYCLE_TEST_CONFIRMED") == "1"
            if already_confirmed and not external_mode:
                for method, signature, values in (
                        ("UpdateInPlaceWithConsent", "(sb)", [window_uuid, False]),
                        ("HoldForUpdate", "(ssb)", [window_uuid, "untrusted-service", True])):
                    try:
                        backend.call(owner, worker.TERMINAL_PATH, worker.TERMINAL_INTERFACE, method, signature, values)
                    except GLib.Error as error:
                        assert "consent" in str(error) or "pending update helper" in str(error), error
                    else:
                        raise AssertionError("Unconfirmed/foreign native update request was accepted")
                assert not available()["pending"]
            if external_mode:
                tracker = ("com.terrydaktal.ApplicationLauncher", "/Tracker", "com.terrydaktal.ApplicationLauncher.Tracker1")
                def tracker_call(method, signature="()", arguments=()):
                    value = json.loads(backend.call(*tracker, method, signature, arguments)[0])
                    assert "Ok" in value, value
                    return value["Ok"]
                payload = dict(id=saved["id"], pid=original.pid, title=saved["title"], **{"class":"xfce4-terminal"})
                backend.call("com.terrydaktal.ApplicationLauncher", "/WindowFeed", "com.terrydaktal.ApplicationLauncher.WindowFeed",
                             "ReplaceSnapshot", "(s)", [json.dumps([payload])])
                operation = tracker_call("BeginTerminalUpgradePreview")
                def ready_preview():
                    report = tracker_call("GetTerminalUpgradeReport", "(s)", [operation])
                    if report["phase"] == "preparing": raise RuntimeError("Waiting for compatibility preview")
                    assert report["phase"] == "ready" and len(report["windows"]) == 1, report
                    assert report["windows"][0]["eligible"], report
                    return report
                wait_for(ready_preview, seconds=35)
                denied = json.loads(backend.call(*tracker, "StartTerminalUpgrade", "(sasb)", [operation, [saved["id"]], False])[0])
                assert "Err" in denied and original.poll() is None, denied
                tracker_call("StartTerminalUpgrade", "(sasb)", [operation, [saved["id"]], True])
                def status():
                    for path in (root / "state/applicationlauncher/lifecycle").glob("*/request.json"):
                        request = json.loads(path.read_text())
                        if request.get("transport") == "external" and request["external"]["window"]["id"] == saved["id"]:
                            value = json.loads(path.with_name("status.json").read_text())
                            return dict(value, operation_id=path.parent.name)
                    raise RuntimeError("Waiting for independently managed compatibility worker")
            else:
                service = backend.call(owner, worker.TERMINAL_PATH, worker.TERMINAL_INTERFACE,
                    "UpdateInPlaceWithConsent" if already_confirmed else "UpdateInPlace",
                    "(sb)" if already_confirmed else "(s)", [window_uuid, True] if already_confirmed else [window_uuid])[0]
                def status():
                    return json.loads(backend.call(service, worker.PATH, worker.INTERFACE, "GetStatus")[0])
            last = wait_for(status)
            if not external_mode: helper_pid = backend.dbus("GetConnectionUnixProcessID", "(s)", [service])[0]
            deadline = time.monotonic() + 75
            daemon_stopped = False
            while time.monotonic() < deadline:
                last = status()
                if (os.environ.get("XFCE_LIFECYCLE_TEST_RESTART") == "1" and not daemon_stopped
                        and last["phase"] == "running"):
                    daemon.terminate()
                    daemon.wait(timeout=5)
                    daemon_stopped = True
                if last["phase"] in ("failed", "complete"):
                    break
                time.sleep(0.1)
            if os.environ.get("XFCE_LIFECYCLE_TEST_LATE_EXIT") == "1":
                assert last["phase"] == "failed" and "two graceful interrupts" in last["message"], last
                assert not last["original_closed"], last
                def hold_released():
                    current = json.loads(backend.call(owner, worker.TERMINAL_PATH, worker.TERMINAL_INTERFACE,
                        "GetUpdateStatus", "(s)", [window_uuid])[0])
                    assert not current["pending"], "Waiting for native failure cleanup"
                    return True
                wait_for(hold_released)
                signal.pidfd_send_signal(suspended_application, signal.SIGCONT)
                os.close(suspended_application)
                suspended_application = None
                def retained_empty_tab():
                    assert original.poll() is None, "The retained original closed after a delayed child exit"
                    records = original_records()
                    assert len(records) == 1 and records[0]["child_pid"] <= 1, records
                    assert records[0]["input_enabled"], "The failed update left terminal input disabled"
                    actual = backend.kwin("capture", pid=original.pid, id=saved["id"])
                    placement_matches(saved, actual, saved["minimized"])
                    return actual
                wait_for(retained_empty_tab)
                print(json.dumps(dict(result="passed", mode="late-application-exit", old_pid=original.pid,
                    geometry=saved["geometry"], original_retained=True)), flush=True)
                return
            if last["phase"] != "complete":
                raise AssertionError(json.dumps(last))
            replacement = last["replacement_pid"]
            if os.environ.get("XFCE_LIFECYCLE_TEST_RESTART") == "1":
                assert daemon_stopped, "The coordinator did not expose its running phase before completion"
                daemon = subprocess.Popen([daemon_binary], stdout=daemon_log, stderr=daemon_log)
                def recovered_report():
                    report = json.loads(backend.call("com.terrydaktal.ApplicationLauncher", "/Lifecycle",
                        "com.terrydaktal.ApplicationLauncher.Lifecycle1", "GetWindowUpdateReport", "(s)", [last["operation_id"]])[0])
                    assert report["phase"] == "complete", report
                    return report
                wait_for(recovered_report)
            lifecycle_state = root / "state/applicationlauncher/lifecycle"
            plans = list(lifecycle_state.glob("*/restart-plan.json"))
            assert len(plans) == 1, "The update did not retain its metadata-only restart plan"
            plan = json.loads(plans[0].read_text())
            assert plan["image_retention"] == "Open descriptors during handover only; no executable backups"
            assert not (lifecycle_state / "builds").exists(), "The update archived executable contents"
            for tab in plan["tabs"]:
                assert "recovery_argv" not in tab and "recovery_build_sha256" not in tab
                assert "recovery_executable" not in tab.get("return_shell", {})
            original.wait(timeout=5)
            actual = backend.kwin("capture", pid=replacement, id=last["replacement_window_id"])
            placement_matches(saved, actual, saved["minimized"])
            assert subprocess.check_output(["tmux", "-N", "-S", socket_path, "display-message", "-p", "-t", "fixture",
                                            "#{pane_pid}"], timeout=3).decode().strip() == pane
            if not (shell_mode or title_mode or codex_mode):
                assert len(subprocess.check_output(["tmux", "-N", "-S", socket_path, "list-clients", "-F", "#{client_pid}"], timeout=3).splitlines()) == 1
            replacement_records = backend.records(backend.terminal_owner(replacement))
            assert len(replacement_records) == (2 if multiple_tabs else 1)
            if multiple_tabs:
                if external_mode: assert not (root / "confirmed").exists(), "A second terminal confirmation was shown"
                else: assert (root / "confirmed").read_text() == "confirmed\n"
                assert [row["active"] for row in replacement_records] == [False, True]
                assert [os.path.realpath(row["working_directory"]) for row in replacement_records] == [str(root), str(root / "second-tab")]
            if title_mode:
                expected_title = saved["title"].replace("\u0336", "")
                assert titles[1] in expected_title
                def titles_restored():
                    records = backend.records(backend.terminal_owner(replacement))
                    # exec and process verification can precede VTE consuming
                    # the gate's OSC output on the GTK main loop.
                    if ([row.get("terminal_title") for row in records] != titles
                            or records[1]["window_title"] != expected_title):
                        raise RuntimeError("Waiting for restored OSC titles: " + json.dumps(records))
                    return records
                replacement_records = wait_for(titles_restored)
                # A restored title is initial output, not a permanent custom-title override.
                descriptor = os.open(replacement_records[1]["pty"], os.O_WRONLY | os.O_NOCTTY)
                try:
                    os.write(descriptor, b"\033]2;application changed its title\007")
                finally:
                    os.close(descriptor)
                def title_changed():
                    records = backend.records(backend.terminal_owner(replacement))
                    if records[1].get("terminal_title") != "application changed its title":
                        raise RuntimeError("Waiting for the application's new title")
                    assert records[0]["terminal_title"] == titles[0]
                    assert records[1]["window_title"] == expected_title.replace(titles[1], "application changed its title")
                    return records
                wait_for(title_changed)
            if codex_mode:
                root_pid = replacement_records[0]["child_pid"]
                candidates = [root_pid, *map(int, Path(f"/proc/{root_pid}/task/{root_pid}/children").read_text().split())]
                matches = [candidate for candidate in candidates
                    if [os.fsdecode(arg) for arg in Path(f"/proc/{candidate}/cmdline").read_bytes().split(b"\0") if arg] == codex_arguments]
                assert len(matches) == 1, "The exact Codex argv was not preserved under the terminal shell"
                pid = matches[0]
                assert pid != original_codex_pid
                arguments = Path(f"/proc/{pid}/cmdline").read_bytes().split(b"\0")
                assert [os.fsdecode(arg) for arg in arguments[1:] if arg] == codex_arguments[1:]
                with open(f"/proc/{pid}/exe", "rb") as stream:
                    assert hashlib.file_digest(stream, "sha256").hexdigest() == original_codex_hash
                with codex_binary.open("rb") as stream:
                    assert hashlib.file_digest(stream, "sha256").hexdigest() != original_codex_hash
                assert os.path.realpath(f"/proc/{pid}/cwd") == str(root)
                if already_confirmed: assert not (root / "confirmed").exists(), "A second confirmation was shown"
                else: assert (root / "confirmed").read_text() == "confirmed\n"
                # This private fixture returns zero for SIGUSR1: model /exit,
                # then verify the *same* tab/window remains as an idle shell.
                os.kill(pid, signal.SIGUSR1)
                def returned_to_fish():
                    records = backend.records(backend.terminal_owner(replacement))
                    assert len(records) == 1 and records[0]["child_pid"] == root_pid
                    executable = Path(os.readlink(f"/proc/{root_pid}/exe").removesuffix(" (deleted)"))
                    assert executable.name == "fish", "Codex quit without leaving Fish in the updated terminal"
                    assert not Path(f"/proc/{root_pid}/task/{root_pid}/children").read_text().strip(), "Fish has not reached its idle prompt"
                    assert os.path.realpath(f"/proc/{root_pid}/cwd") == str(root)
                    return records
                wait_for(returned_to_fish)
                placement_matches(saved, backend.kwin("capture", pid=replacement, id=actual["id"]), saved["minimized"])
            print(json.dumps(dict(result="passed", transport="external" if external_mode else "native", mode="codex" if codex_mode else "titles" if title_mode else "shell" if shell_mode else "tmux", old_pid=original.pid, replacement_pid=replacement,
                                  geometry=actual["geometry"], tmux_pane_pid=pane)), flush=True)
        except Exception as error:
            failures.append(str(error))
            print("Wayland update test failed: " + str(error), file=sys.stderr, flush=True)
            traceback.print_exc()
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
    if suspended_application is not None:
        try: signal.pidfd_send_signal(suspended_application, signal.SIGCONT)
        except ProcessLookupError: pass
        finally: os.close(suspended_application)
    if original is not None and original.poll() is None:
        original.terminate()
        original.wait(timeout=5)
    subprocess.run(["tmux", "-N", "-S", socket_path, "kill-server"], timeout=3,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    compositor.terminate()
    compositor.wait(timeout=5)
    log.close()
    daemon.terminate()
    daemon.wait(timeout=5)
    daemon_log.close()
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
        (front / "xfce4-terminal").symlink_to(binary)
        scope = front / "systemd-run"
        scope.write_text("#!/usr/bin/python3\nimport os,sys,subprocess\na=sys.argv[sys.argv.index('--')+1:]\n"
            "if '--service-type=exec' in sys.argv:\n"
            " log=open(os.environ['XFCE_UPDATE_TEST_ROOT']+'/supervised-jobs.log','ab')\n"
            " if os.environ.get('XFCE_LIFECYCLE_TEST_TRACE') == '1':\n"
            "  a=['strace','-ff','-s','96','-e','trace=execve,execveat,exit_group,clone,clone3,wait4,openat,read,lseek,close','-o',os.environ['XFCE_UPDATE_TEST_ROOT']+'/worker-trace']+a\n"
            " subprocess.Popen(a,stdin=subprocess.DEVNULL,stdout=log,stderr=log,start_new_session=True,close_fds=True)\n"
            "else:\n os.execvp(a[0],a)\n")
        scope.chmod(0o700)
        environment.update(PATH=str(front) + ":" + environment["PATH"], XFCE_UPDATE_TEST_ROOT=str(root),
            QT_QPA_PLATFORM="offscreen", GDK_BACKEND="wayland", WAYLAND_DISPLAY="xfce-update-test",
            LIBGL_ALWAYS_SOFTWARE="1", KWIN_COMPOSE="Q", XDG_CURRENT_DESKTOP="KDE", NO_AT_BRIDGE="1",
            GIO_USE_VFS="local", GVFS_DISABLE_FUSE="1",
            __EGL_VENDOR_LIBRARY_FILENAMES="/usr/share/glvnd/egl_vendor.d/50_mesa.json")
        private_vte = binary.parent.parent / "subprojects/vte-0.84.1/src"
        if private_vte.is_dir():
            environment["LD_LIBRARY_PATH"] = str(private_vte) + ":" + environment.get("LD_LIBRARY_PATH", "")
        if "--shell" in sys.argv or "--titles" in sys.argv or "--codex" in sys.argv:
            module = sys.argv[sys.argv.index("--confirm-module") + 1]
            environment.update(XFCE_UPDATE_TEST_MODE="codex" if "--codex" in sys.argv else "titles" if "--titles" in sys.argv else "shell", GTK3_MODULES=str(Path(module).resolve()),
                               XFCE_UPDATE_TEST_CONFIRM_LOG=str(root / "confirmed"))
        if "--restart-daemon" in sys.argv:
            environment["XFCE_LIFECYCLE_TEST_RESTART"] = "1"
        if "--confirmed" in sys.argv:
            environment["XFCE_LIFECYCLE_TEST_CONFIRMED"] = "1"
        if "--external" in sys.argv:
            environment["XFCE_LIFECYCLE_TEST_EXTERNAL"] = "1"
        if "--legacy-binary" in sys.argv:
            environment["XFCE_LIFECYCLE_TEST_LEGACY_BINARY"] = sys.argv[sys.argv.index("--legacy-binary") + 1]
        if "--without-tab-api" in sys.argv:
            environment["XFCE_LIFECYCLE_TEST_NO_TAB_API"] = "1"
        if "--tmux-shell" in sys.argv:
            environment["XFCE_LIFECYCLE_TEST_TMUX_SHELL"] = "1"
        if "--without-hold-api" in sys.argv:
            environment["XFCE_LIFECYCLE_TEST_NO_HOLD_API"] = "1"
        if "--late-application-exit" in sys.argv:
            environment["XFCE_LIFECYCLE_TEST_LATE_EXIT"] = "1"
        if "--codex-in-fish" in sys.argv:
            environment["XFCE_LIFECYCLE_TEST_CODEX_IN_FISH"] = "1"
        if "--trace-worker" in sys.argv:
            environment["XFCE_LIFECYCLE_TEST_TRACE"] = "1"
        result = subprocess.run(["dbus-run-session", "--", sys.executable, __file__, "--session", str(binary)],
                                env=environment, timeout=110)
        if result.returncode:
            for name in ("kwin.log", "original.log", "lifecycle-daemon.log", "supervised-jobs.log"):
                path = root / name
                if path.exists(): print(path.read_text(errors="replace")[-5000:], file=sys.stderr)
            for path in (root / "state/xfce4-terminal/updates").glob("*.log"):
                print(path.read_text(errors="replace")[-5000:], file=sys.stderr)
            for path in (root / "state/applicationlauncher/lifecycle").glob("*/replacement-launch.json"):
                print(path.read_text(errors="replace")[-5000:], file=sys.stderr)
            if "--trace-worker" in sys.argv or "--keep-artifacts" in sys.argv:
                evidence = Path(tempfile.mkdtemp(prefix="lifecycle-window-failure-"))
                for path in root.glob("worker-trace*"):
                    shutil.copyfile(path, evidence / path.name)
                shutil.copytree(root / "state/applicationlauncher/lifecycle", evidence / "lifecycle")
                print("Retained private failure artifacts: " + str(evidence), file=sys.stderr)
        return result.returncode


if __name__ == "__main__":
    sys.exit(main())
