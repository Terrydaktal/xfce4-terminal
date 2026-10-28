"""Native VTE selection regression, using only owned text and a private X server.

The installed VTE 0.84.1 can re-enable PTY reads from an already scheduled
processing frame while a mouse selection is still being extended. Its output
handler then clears the unfinished selection. Plain append-only output can
trigger this; no Pi backend or conversation is needed.

LD_LIBRARY_PATH is deliberately inherited so this test can qualify a candidate
VTE library without replacing a library used by running terminals.
"""

import hashlib
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import threading
import time
import unittest

CASES = (
    "test_idle_drag_and_autoscroll_control",
    "test_streaming_drag_stays_selected",
    "test_streaming_wheel_stays_selected",
    "test_streaming_drag_above_viewport_stays_selected",
    "test_active_selection_is_invalidated_when_its_text_changes",
    "test_released_selection_is_invalidated_only_when_its_text_changes",
)


def run_private_display():
    """Never inject mouse input into the desktop running the test command."""
    if not shutil.which("xvfb-run") or not shutil.which("xdotool"):
        raise SystemExit("This regression requires xvfb-run and xdotool")
    environment = os.environ.copy()
    environment.update(GDK_BACKEND="x11", NO_AT_BRIDGE="1",
                       XFCE_STREAM_SELECTION_PRIVATE="1")
    environment.pop("WAYLAND_DISPLAY", None)
    # Native grabs and PRIMARY ownership belong to a display. Give each case
    # its own display/process as well as text, rather than carrying GDK grab
    # state across destroyed windows.
    cases = sys.argv[1:] or [f"StreamSelection.{name}" for name in CASES]
    failed = False
    for case in cases:
        process = subprocess.Popen(
            ["xvfb-run", "-a", "-s", "-screen 0 1200x900x24 -nolisten tcp",
             sys.executable, str(Path(__file__).resolve()), "--private-display", case],
            env=environment, start_new_session=True,
        )
        try:
            failed |= process.wait(timeout=5) != 0
        except subprocess.TimeoutExpired:
            print(f"{case}: owned GUI case exceeded five seconds", file=sys.stderr)
            # Include the owned X server and GTK child, not only xvfb-run.
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=1)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait(timeout=1)
            failed = True
    raise SystemExit(int(failed))


if "--private-display" not in sys.argv:
    run_private_display()

if os.environ.get("XFCE_STREAM_SELECTION_PRIVATE") != "1":
    raise SystemExit("The GTK worker must be started by the private-display runner")

sys.argv.remove("--private-display")
import gi  # noqa: E402

gi.require_version("Gtk", "3.0")
gi.require_version("Gdk", "3.0")
gi.require_version("Vte", "2.91")
from gi.repository import Gdk, Gtk, Vte  # noqa: E402


class StreamSelection(unittest.TestCase):
    def create_terminal(self):
        self.window = Gtk.Window()
        self.window.set_default_size(800, 480)
        self.terminal = Vte.Terminal()
        self.terminal.set_scrollback_lines(5000)
        self.terminal.set_scroll_on_output(False)
        self.terminal.set_scroll_on_keystroke(False)
        scroller = Gtk.ScrolledWindow()
        scroller.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.NEVER)
        scroller.add(self.terminal)
        self.window.add(scroller)
        self.window.show_all()
        self.terminal.grab_focus()
        self.pump(.08)
        padding = self.terminal.get_style_context().get_padding(Gtk.StateFlags.NORMAL)
        self.padding = padding
        self.cw, self.ch = self.terminal.get_char_width(), self.terminal.get_char_height()
        self.window.resize(100 * self.cw + padding.left + padding.right,
                           30 * self.ch + padding.top + padding.bottom)
        self.pump(.1)
        self.xid = self.window.get_window().get_xid()
        self.adjustment = self.terminal.get_vadjustment()
        self.content_changes = 0
        self.terminal.connect("contents-changed", self.contents_changed)

    def contents_changed(self, _terminal):
        self.content_changes += 1

    @classmethod
    def pump(cls, seconds=.05):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if Gtk.events_pending():
                Gtk.main_iteration_do(False)
            time.sleep(.0005)

    @classmethod
    def pointer(cls, *arguments):
        subprocess.run(["xdotool", *map(str, arguments)], check=True, timeout=3)

    def xy(self, column, row):
        return (round(self.padding.left + (column + .5) * self.cw),
                round(self.padding.top + (row + .5) * self.ch))

    def setUp(self):
        self.create_terminal()
        self.master, self.slave = os.openpty()
        os.set_blocking(self.slave, False)
        self.pty = Vte.Pty.new_foreign_sync(self.master, None)
        self.terminal.set_pty(self.pty)
        self.terminal.reset(True, True)
        self.write("".join(f"SYNTHETIC ROW {row:04d} abcdefghijklmnopqrstuvwxyz\r\n"
                           for row in range(600)))
        # VTE's absolute ring coordinates can begin at zero or one after its
        # initial allocation; all 600 newline-terminated rows must be present.
        deadline = time.monotonic() + 1
        while self.terminal.get_cursor_position()[1] < 600 and time.monotonic() < deadline:
            self.pump(.005)
        self.assertIn(self.terminal.get_cursor_position()[1], (600, 601),
                      "owned transcript had not finished rendering")
        self.adjustment.set_value(self.adjustment.get_upper() - self.adjustment.get_page_size())
        self.pump(.06)
        self.assertEqual((self.terminal.get_column_count(), self.terminal.get_row_count()), (100, 30))
        self.stop = threading.Event()
        self.writer = None
        self.writes = 0

    def tearDown(self):
        self.stop.set()
        if self.writer:
            self.writer.join(timeout=2)
            self.assertFalse(self.writer.is_alive(), "owned PTY writer did not stop")
        self.pointer("mouseup", 1, "keyup", "Shift_L")
        self.pump(.05)
        self.window.destroy()
        self.pump(.03)
        os.close(self.slave)
        self.pty = None  # Vte.Pty owns and closes the master descriptor.

    def write(self, text):
        remaining = text.encode()
        deadline = time.monotonic() + 2
        while remaining and time.monotonic() < deadline:
            try:
                remaining = remaining[os.write(self.slave, remaining):]
            except BlockingIOError:
                pass
            self.pump(.001)
        self.assertFalse(remaining, "owned PTY output did not drain")
        self.pump(.04)

    def selection(self):
        text = self.terminal.get_text_selected(Vte.Format.TEXT) or ""
        return len(text), hashlib.sha256(text.encode()).hexdigest()

    def start_stream(self):
        def produce():
            while not self.stop.is_set():
                try:
                    # Only the last row changes; the selected rows are untouched.
                    os.write(self.slave, f"\x1b[30;1H\x1b[2KSYNTHETIC LIVE {self.writes}".encode())
                    self.writes += 1
                except BlockingIOError:
                    pass
                self.stop.wait(.002)

        self.writer = threading.Thread(target=produce, daemon=True)
        self.writer.start()
        self.pump(.025)

    def drag(self, shift=False):
        if shift:
            self.pointer("keydown", "Shift_L")
        self.pointer("mousemove", "--window", self.xid, *self.xy(20, 20),
                     "mousedown", 1, "mousemove", "--window", self.xid, *self.xy(0, 10))
        self.pump(.12)
        self.assertTrue(self.terminal.get_has_selection(), "stream output cancelled an active mouse selection")
        self.assertGreater(self.selection()[0], 0)

    def finish_stream(self):
        self.stop.set()
        if self.writer:
            self.writer.join(timeout=2)
        self.pointer("mouseup", 1, "keyup", "Shift_L")
        changes = self.content_changes
        self.write("\x1b[30;1H\x1b[2KSYNTHETIC FINAL UPDATE")
        self.assertGreater(self.content_changes, changes, "terminal output stopped after releasing selection")

    def exercise(self, action, shift=False, streaming=True):
        if streaming:
            self.start_stream()
        self.drag(shift)
        initial = self.selection()
        writes_before = self.writes
        changes_before = self.content_changes
        if action == "wheel":
            self.pointer("click", "--repeat", 3, "--delay", 25, 4)
            self.pump(.12)
            self.assertEqual(self.selection(), initial, "wheel scrolling altered the selected text")
        elif action == "autoscroll":
            before = self.adjustment.get_value()
            self.pointer("mousemove", "--window", self.xid, *self.xy(0, -1))
            self.pump(.25)
            self.assertLess(self.adjustment.get_value(), before, "dragging above the viewport did not autoscroll")
            self.assertGreater(self.selection()[0], initial[0], "autoscroll did not extend selection")
        else:
            self.pump(.12)
            self.assertEqual(self.selection(), initial)
        self.assertTrue(self.terminal.get_has_selection(), "scrolling cancelled an active selection")
        selected = self.selection()
        if streaming:
            self.assertGreater(self.writes, writes_before, "selection stopped the owned output producer")
            self.assertGreater(self.content_changes, changes_before,
                               "selection paused terminal output instead of preserving it")
        self.finish_stream()
        self.assertEqual(self.selection(), selected, "unrelated output invalidated the released selection")
        copied = Gtk.Clipboard.get(Gdk.SELECTION_PRIMARY).wait_for_text() or ""
        self.assertEqual((len(copied), hashlib.sha256(copied.encode()).hexdigest()), selected,
                         "released selection did not reach the native primary clipboard")

    def test_idle_drag_and_autoscroll_control(self):
        self.exercise("autoscroll", streaming=False)

    def test_streaming_drag_stays_selected(self):
        self.exercise("hold")

    def test_streaming_wheel_stays_selected(self):
        self.exercise("wheel", shift=True)

    def test_streaming_drag_above_viewport_stays_selected(self):
        self.exercise("autoscroll", shift=True)

    def test_released_selection_is_invalidated_only_when_its_text_changes(self):
        self.drag()
        self.pointer("mouseup", 1)
        self.pump(.05)
        original = self.selection()
        self.write("\x1b[30;1H\x1b[2KUNRELATED OUTPUT")
        self.assertEqual(self.selection(), original)
        self.write("\x1b[15;1H\x1b[2KSELECTED TEXT WAS REPLACED")
        self.assertFalse(self.terminal.get_has_selection(), "changed selected content remained highlighted")

    def test_active_selection_is_invalidated_when_its_text_changes(self):
        self.start_stream()
        self.drag()
        self.write("\x1b[15;1H\x1b[2KSELECTED TEXT WAS REPLACED")
        self.assertFalse(self.terminal.get_has_selection(), "changed active selection remained highlighted")


if __name__ == "__main__":
    unittest.main(verbosity=2)
