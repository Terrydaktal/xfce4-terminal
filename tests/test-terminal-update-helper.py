"""Isolated transaction tests: no desktop session or user window is touched."""

import copy
import importlib.util
import json
import os
from pathlib import Path
import shutil
import select
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest.mock import Mock, patch

SOURCE = Path(__file__).resolve().parents[1] / "terminal/update"
sys.path.insert(0, str(SOURCE))
import core
import processes
SPEC = importlib.util.spec_from_file_location("terminal_mechanics", SOURCE / "terminal-update-helper.py")
mechanics = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(mechanics)


def window():
    geometry = dict(x=731, y=1551, width=719, height=614)
    return dict(id="original", pid=123, title="Terminal", geometry=geometry, normal=geometry.copy(),
                output=dict(x=0, y=0, width=5120, height=2160, name="HDMI-A-3", scale=1000),
                maximized=0, fullscreen=False, minimized=False, active=True, desktops=["desktop"],
                activities=[], keepAbove=False, keepBelow=False, noBorder=False, shaded=False,
                skipTaskbar=False, skipPager=False, skipSwitcher=False, above=[])


class LayoutTests(unittest.TestCase):
    def test_offscreen_edges_and_oversized_rectangles_remain_exact(self):
        for x, y, width, height in [(-5, 80, 400, 300), (100, -5, 400, 300),
                                     (5000, 2000, 719, 614), (-30, -40, 6000, 2400)]:
            saved = window()
            saved["geometry"] = dict(x=x, y=y, width=width, height=height)
            core.placement_matches(saved, copy.deepcopy(saved), False)

    def test_running_binary_identity_must_match_the_pinned_file(self):
        metadata = os.stat("/proc/self/exe")
        core.verify_binary(os.getpid(), (metadata.st_dev, metadata.st_ino))
        with self.assertRaises(processes.UpdateError):
            core.verify_binary(os.getpid(), (metadata.st_dev, metadata.st_ino + 1))

    def test_launch_entry_retargeting_is_observable(self):
        with tempfile.TemporaryDirectory() as directory:
            alias = Path(directory) / "terminal"
            alias.symlink_to("/bin/true")
            original = core.resolve_launch(alias, "executable")
            alias.unlink()
            alias.symlink_to("/bin/false")
            self.assertNotEqual(core.resolve_launch(alias, "executable"), original)

    def test_five_pixel_offscreen_position_is_preserved(self):
        original = window()
        core.placement_matches(original, copy.deepcopy(original), False)
        clamped = copy.deepcopy(original)
        clamped["geometry"]["y"] = 1546
        with self.assertRaisesRegex(processes.UpdateError, "geometry differs"):
            core.placement_matches(original, clamped, False)

    def test_changed_monitor_and_window_states_are_refused(self):
        original = window()
        for key in ("x", "y", "width", "height", "scale", "name"):
            changed = copy.deepcopy(original)
            changed["output"][key] = "other" if key == "name" else changed["output"][key] + 1
            with self.subTest(key=key), self.assertRaises(processes.UpdateError):
                core.placement_matches(original, changed, False)
        for key in ("maximized", "fullscreen", "minimized", "desktops", "activities", "keepAbove",
                    "keepBelow", "noBorder", "shaded", "skipTaskbar", "skipPager", "skipSwitcher"):
            changed = copy.deepcopy(original)
            changed[key] = ["other"] if isinstance(changed[key], list) else 1
            with self.subTest(key=key), self.assertRaises(processes.UpdateError):
                core.placement_matches(original, changed, False)

    def test_missing_or_invalid_output_is_not_assumed_unchanged(self):
        for output in (None, {}, dict(width=0, height=100, scale=1000), dict(width=100, height=100, scale=0)):
            with self.subTest(output=output), self.assertRaises(processes.UpdateError):
                core.exact_output(output, output)

    def test_tabs_preserve_order_active_tab_and_literal_arguments(self):
        tabs = [dict(cwd="/tmp/space ' dir", active=False), dict(cwd="/tmp", active=True)]
        commands = [["/bin/printf", "$(touch /never)", "a'b"], ["/bin/true"]]
        arguments = core.terminal_arguments(tabs, commands, 137, 41)
        self.assertEqual(arguments[:3], ["--disable-server", "--minimize", "--geometry=137x41"])
        self.assertNotIn("--window", arguments)
        self.assertEqual(arguments.count("--tab"), 1)
        self.assertEqual(arguments.count("--active-tab"), 1)
        import shlex
        self.assertEqual(shlex.split(arguments[arguments.index("--command") + 1]), commands[0])


class GateTests(unittest.TestCase):
    def test_pinned_execution_path_is_distinct_from_the_preserved_argv_zero(self):
        with tempfile.TemporaryDirectory() as directory:
            descriptor = os.open("/bin/sh", os.O_RDONLY)
            try:
                image = f"/proc/{os.getpid()}/fd/{descriptor}"
                tabs = [dict(argv=["original-program-name", "-c", 'printf "%s" "$0"'], executable=image)]
                gates = core.StartupGates(directory, tabs)
                child = subprocess.Popen(gates.commands(tabs, str(SOURCE / "terminal-update-helper.py"))[0],
                                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                try:
                    self.assertTrue(self.wait_ready(gates, child.pid))
                    gates.release()
                    output, error = child.communicate(timeout=3)
                    self.assertEqual(child.returncode, 0, error)
                    self.assertEqual(output, b"original-program-name")
                finally:
                    gates.close()
                    if child.poll() is None:
                        child.terminate()
                        child.communicate(timeout=3)
            finally:
                os.close(descriptor)

    def test_environment_values_and_absence_survive_the_generic_startup_gate(self):
        with tempfile.TemporaryDirectory() as directory:
            tabs = [dict(argv=[sys.executable, "-c", "import json,os; print(json.dumps(dict(os.environ)))"],
                         environment=dict(NO_COLOR=None, COLORTERM="truecolor", LC_CTYPE="C.UTF-8"))]
            gates = core.StartupGates(directory, tabs)
            environment = dict(os.environ, NO_COLOR="1", COLORTERM="wrong", XFCE4_TERMINAL_UPDATE_SERVICE="test")
            child = subprocess.Popen(gates.commands(tabs, str(SOURCE / "terminal-update-helper.py"))[0],
                                     env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                self.assertTrue(self.wait_ready(gates, child.pid))
                gates.release()
                output, error = child.communicate(timeout=3)
                self.assertEqual(child.returncode, 0, error)
                actual = json.loads(output)
                self.assertNotIn("NO_COLOR", actual)
                self.assertNotIn("XFCE4_TERMINAL_UPDATE_SERVICE", actual)
                self.assertEqual(actual["COLORTERM"], "truecolor")
                self.assertEqual(actual["LC_CTYPE"], "C.UTF-8")
            finally:
                gates.close()
                if child.poll() is None:
                    child.terminate()
                    child.communicate(timeout=3)

    def test_malformed_generic_environment_is_rejected_before_execution(self):
        for environment in ([], {"bad=name": "value"}, {"NAME": 1}, {"NAME": "bad\0value"}, {"NAME": "x" * 65537}):
            with self.subTest(environment=str(environment)[:40]), self.assertRaises(processes.UpdateError):
                core.validate_environment(environment)

    def test_restored_title_is_literal_output_before_the_application(self):
        with tempfile.TemporaryDirectory() as directory:
            title = "htop ~/100%/literal-%D 'quoted' \u03c0"
            tabs = [dict(argv=["/bin/printf", "APPLICATION"], title=title)]
            gates = core.StartupGates(directory, tabs)
            child = subprocess.Popen(gates.commands(tabs, str(SOURCE / "terminal-update-helper.py"))[0],
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                self.assertTrue(self.wait_ready(gates, child.pid))
                self.assertFalse(select.select([child.stdout], [], [], 0)[0])
                gates.release()
                output, error = child.communicate(timeout=3)
                self.assertEqual(child.returncode, 0, error)
                self.assertEqual(output, b"\033]2;" + title.encode() + b"\007APPLICATION")
            finally:
                gates.close()
                if child.poll() is None:
                    child.terminate()
                    child.communicate(timeout=3)

    def test_unsafe_title_is_rejected_before_starting_a_replacement(self):
        for title in ("bad\033]52;c;payload\007", "bad\nline", "bad\x9b31m", "x" * 16385):
            with self.subTest(title=title[:20]), tempfile.TemporaryDirectory() as directory:
                tabs = [dict(argv=["/bin/true"], title=title)]
                gates = core.StartupGates(directory, tabs)
                try:
                    with self.assertRaisesRegex(processes.UpdateError, "title"):
                        gates.commands(tabs, str(SOURCE / "terminal-update-helper.py"))
                finally:
                    gates.close()

    def test_real_child_cannot_execute_until_release(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "executed"
            tabs = [dict(argv=["/bin/sh", "-c", 'printf executed > "$1"', "gate-test", str(output)])]
            gates = core.StartupGates(directory, tabs)
            command = gates.commands(tabs, str(SOURCE / "terminal-update-helper.py"))[0]
            child = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            try:
                # The socket can be ready before VTE's async spawn callback sets child_pid.
                self.assertTrue(select.select(gates.sockets, [], [], 3)[0])
                self.assertFalse(gates.ready([dict(child_pid=0)]))
                self.assertIsNone(child.poll())
                self.assertTrue(self.wait_ready(gates, child.pid))
                self.assertFalse(output.exists())
                gates.release()
                self.assertEqual(child.wait(timeout=3), 0)
                self.assertEqual(output.read_text(), "executed")
            finally:
                gates.close()
                if child.poll() is None:
                    child.terminate()
                child.communicate(timeout=3)

    def wait_ready(self, gates, pid):
        import time
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            if gates.ready([dict(child_pid=pid)]):
                return True
        return False

    def test_cancelled_gate_does_not_execute(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "should-not-exist"
            tabs = [dict(argv=["/bin/touch", str(output)])]
            gates = core.StartupGates(directory, tabs)
            child = subprocess.Popen(gates.commands(tabs, str(SOURCE / "terminal-update-helper.py"))[0],
                                     stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            try:
                self.assertTrue(self.wait_ready(gates, child.pid))
                gates.close()
                self.assertEqual(child.wait(timeout=3), 125)
                self.assertFalse(output.exists())
            finally:
                if child.poll() is None:
                    child.terminate()
                child.communicate(timeout=3)

    def test_generic_staging_gates_every_command_without_interpreting_tmux(self):
        with tempfile.TemporaryDirectory() as directory:
            command = ["/usr/bin/tmux", "-N", "-S", "/tmp/private", "attach-session", "-t", "$6"]
            tabs = [dict(argv=command, tmux={"session": "$6"}, title="tmux owns this title")]
            gates = core.StartupGates(directory, tabs)
            prepared = gates.commands(tabs, "unused")[0]
            self.assertEqual(prepared[-len(command):], command)
            self.assertIn("--gate", prepared)
            self.assertFalse(gates.ready([dict(child_pid=0)]))
            gates.close()


class ObserverTests(unittest.TestCase):
    def test_worker_exit_after_final_publish_cannot_turn_success_into_failure(self):
        for phase in ("complete", "failed", "running"):
            with self.subTest(phase=phase):
                backend = mechanics.Backend.__new__(mechanics.Backend)
                backend.operations = threading.Lock()
                backend.status = dict(phase=phase, backup="/private/retained-plan")
                backend.replacement = Mock(closed=False)
                backend.publish = Mock()
                backend.coordinator_lost()
                if phase == "running":
                    backend.replacement.recover.assert_called_once()
                    self.assertEqual(backend.publish.call_args.args[0]["phase"], "failed")
                    self.assertEqual(backend.publish.call_args.args[0]["backup"], "/private/retained-plan")
                else:
                    backend.replacement.recover.assert_not_called()
                    backend.publish.assert_not_called()


class Backend:
    def __init__(self):
        self.events = []
        self.closed = False
        self.status = None
        self.record = dict(window_uuid="window", tab_uuid="tab", active=True, child_pid=111,
                           foreground_pgid=111, pty="/missing-test-pty", working_directory="/tmp")
        self.fault = None

    def publish(self, status):
        self.status = status
        self.events.append(status["phase"])

    def records(self, owner):
        record = dict(self.record)
        if owner == "new":
            record.update(window_uuid="new-window", tab_uuid="new-tab", child_pid=222)
        return [record]

    def wait_for_confirmation(self, seconds):
        self.events.append("confirmed")
        if self.fault == "cancel":
            raise processes.UpdateError("cancelled")

    def check_cancelled(self):
        pass

    def terminal_owner(self, pid):
        return "new"

    def kwin(self, action, **values):
        self.events.append(action)
        snapshot = window()
        if values["pid"] == 222:
            snapshot["id"] = "replacement"
            snapshot["pid"] = 222
            snapshot["minimized"] = action != "present"
        if self.fault == "monitor":
            snapshot["output"]["scale"] += 1 if self.events.count("capture") > 1 else 0
        if self.fault == "geometry" and values["pid"] == 222:
            snapshot["geometry"]["y"] = 1546
        return snapshot

    def close_original(self, uuid):
        self.events.append("close-original")
        self.closed = True

    def finish_replacement(self, owner):
        self.events.append("finish-staging")
        if self.fault == "finish":
            raise processes.UpdateError("Could not finish replacement startup")

    def call(self, *arguments):
        return ["<method name='HoldForUpdate'/>"]

    def hold_original(self, enabled):
        self.events.append("hold" if enabled else "unhold")


class ReplacementTests(unittest.TestCase):
    def run_staging(self, fault=None):
        backend = Backend()
        backend.fault = fault
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "xfce4-terminal"
            shutil.copyfile("/bin/true", binary)
            binary.chmod(0o700)
            request = dict(owner_pid=123, owner_bus="old", window_uuid="window", window_title="Terminal",
                           executable=str(binary), launch_path=str(binary), launch_kind="executable", job_id="job", service="job-service")
            tab = dict(argv=["/bin/true"], cwd="/tmp", active=True, title="literal %D")
            ticks = iter(range(1, 1000))
            class Gates:
                def __init__(self, *arguments): pass
                def commands(self, *arguments): return [["/bin/true"]]
                def ready(self, *arguments): backend.events.append("gates-ready"); return True
                def release(self): backend.events.append("release-programs")
                def close(self): backend.events.append("gates-closed")
            class Child:
                pid = 222
                returncode = 1
                def poll(self): return 1 if fault == "exit" else None
                def terminate(self): backend.events.append("discard-replacement")
            job = core.Replacement(request, backend, str(SOURCE / "terminal-update-helper.py"))
            with patch.object(core, "identity", return_value=dict(pid=123, start=1, exe="old")), \
                 patch.object(core, "digest", side_effect=lambda path: "old" if str(path).startswith("/proc/") else "new"), \
                 patch.object(core, "verify_binary"), \
                 patch.object(core, "alive", side_effect=lambda saved: not backend.closed), \
                 patch.object(core, "StartupGates", Gates), \
                 patch.object(core.subprocess, "Popen", return_value=Child()), \
                 patch.object(core.time, "sleep"), patch.object(core.time, "monotonic", side_effect=lambda: next(ticks)), \
                 patch.dict(os.environ, {"XDG_STATE_HOME": str(Path(directory) / "state"), "XDG_RUNTIME_DIR": directory}):
                try:
                    job.capture()
                    job.stage(dict(tabs=[tab]))
                    self.assertNotIn("close-original", backend.events)
                    job.release()
                    backend.events.append("application-verified-by-coordinator")
                    job.commit()
                    backend.status = dict(phase="complete")
                except processes.UpdateError as error:
                    backend.status = dict(phase="failed", message=str(error), original_closed=job.closed)
                    try:
                        job.recover()
                    except processes.UpdateError:
                        backend.events.append("recovery-error-retained")
                finally:
                    job.cleanup()
        return backend

    def test_staging_never_closes_original_until_coordinator_releases_and_commits(self):
        backend = self.run_staging()
        self.assertEqual(backend.status["phase"], "complete", backend.status)
        events = backend.events
        self.assertLess(events.index("gates-ready"), events.index("close-original"))
        self.assertLess(events.index("release-programs"), events.index("close-original"))
        self.assertLess(events.index("application-verified-by-coordinator"), events.index("close-original"))
        self.assertLess(events.index("release-programs"), events.index("present"))

    def test_preparation_and_placement_failures_never_close_original(self):
        for fault in ("monitor", "geometry", "exit"):
            with self.subTest(fault=fault):
                backend = self.run_staging(fault)
                self.assertEqual(backend.status["phase"], "failed", backend.status)
                self.assertNotIn("close-original", backend.events)
                self.assertNotIn("release-programs", backend.events)
                if fault == "geometry":
                    self.assertIn("geometry differs", backend.status["message"])
                if fault == "exit":
                    self.assertIn("Replacement exited", backend.status["message"])

    def test_tab_changes_invalidate_snapshot(self):
        record = Backend().record
        for key, value in dict(window_uuid="other", tab_uuid="other", active=False, child_pid=222,
                               foreground_pgid=222, pty="other", working_directory="/other").items():
            changed = {**record, key: value}
            self.assertFalse(processes.same_tabs([record], [changed]), key)

    def test_failed_native_finish_keeps_the_original_window(self):
        backend = self.run_staging("finish")
        self.assertEqual(backend.status["phase"], "failed")
        self.assertFalse(backend.status["original_closed"])
        self.assertNotIn("close-original", backend.events)


if __name__ == "__main__":
    unittest.main()
