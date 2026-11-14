"""Application-agnostic terminal staging, PTY gates and window handover mechanics."""
import fcntl
import json
import os
from pathlib import Path
import shlex
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import termios
import time

from processes import UpdateError, alive, identity, same_tabs


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


def title_sequence(title):
    if (not isinstance(title, str) or len(title.encode("utf-8")) > 16384
            or any(ord(char) < 32 or 127 <= ord(char) < 160 for char in title)):
        raise UpdateError("The restored terminal title is unsafe or oversized")
    return b"\033]2;" + title.encode("utf-8") + b"\007"


class StartupGates:
    """Every prepared command waits behind a same-user, child-PID-bound gate."""
    def __init__(self, directory, tabs):
        self.sockets, self.connections = [], {}
        for index in range(len(tabs)):
            listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            listener.settimeout(0.05)
            listener.bind(str(Path(directory) / f"gate-{index}"))
            os.chmod(listener.getsockname(), 0o600)
            listener.listen(1)
            self.sockets.append(listener)

    def commands(self, tabs, helper):
        commands = []
        for listener, tab in zip(self.sockets, tabs, strict=True):
            command = [sys.executable, helper, "--gate", listener.getsockname()]
            if "executable" in tab:
                if not isinstance(tab["executable"], str) or not os.path.isabs(tab["executable"]) or "\0" in tab["executable"]:
                    raise UpdateError("Invalid prepared executable path")
                command.extend(["--executable", tab["executable"]])
            if "environment" in tab:
                validate_environment(tab["environment"])
                command.extend(["--environment", json.dumps(tab["environment"])])
            if "title" in tab:
                title_sequence(tab["title"])
                command.extend(["--title", tab["title"]])
            commands.append([*command, "--", *tab["argv"]])
        return commands

    def ready(self, records):
        if len(records) != len(self.sockets):
            return False
        for index, (listener, record) in enumerate(zip(self.sockets, records, strict=True)):
            if record.get("child_pid", 0) <= 1:
                return False
            if index in self.connections:
                continue
            try:
                connection, _ = listener.accept()
            except socket.timeout:
                return False
            pid, uid, _ = struct.unpack("3i", connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
            if uid != os.getuid() or pid != record["child_pid"]:
                connection.close()
                raise UpdateError("The replacement startup gate belongs to another child")
            self.connections[index] = connection
        return True

    def release(self):
        if len(self.connections) != len(self.sockets):
            raise UpdateError("Replacement startup gates are not ready")
        for connection in self.connections.values():
            connection.sendall(b"S")

    def close(self):
        for connection in self.connections.values():
            connection.close()
        for listener in self.sockets:
            name = listener.getsockname()
            listener.close()
            Path(name).unlink(missing_ok=True)


def validate_environment(environment):
    if (not isinstance(environment, dict) or len(environment) > 128
            or len(json.dumps(environment).encode()) > 65536
            or any(not isinstance(key, str) or not key or "=" in key or "\0" in key
                   or not isinstance(value, (str, type(None))) or (value is not None and "\0" in value)
                   for key, value in environment.items())):
        raise UpdateError("Invalid generic application environment")


def gate(path, arguments, title=None, environment=None, executable=None):
    executable = executable if executable is not None else arguments[0] if arguments else ""
    if not arguments or not isinstance(executable, str) or not os.path.isabs(executable) or "\0" in executable:
        raise UpdateError("A fixed executable is required for terminal startup")
    sequence = title_sequence(title) if title is not None else b""
    overrides = {} if environment is None else environment
    validate_environment(overrides)
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.connect(path)
        if connection.recv(1) != b"S":
            return 125
    if sequence:
        os.write(1, sequence)
    child_environment = os.environ.copy()
    child_environment.pop("XFCE4_TERMINAL_UPDATE_SERVICE", None)
    for key, value in overrides.items():
        if value is None: child_environment.pop(key, None)
        else: child_environment[key] = value
    os.execve(executable, arguments, child_environment)


class Replacement:
    def __init__(self, request, backend, helper):
        self.request, self.backend, self.helper = request, backend, str(Path(helper).resolve())
        self.snapshot = self.replacement = self.directory = self.gates = None
        self.descriptor = None
        self.released = self.closed = False

    def capture(self):
        if self.snapshot is not None:
            self.preflight()
            return self.snapshot
        request = self.request
        owner = identity(request["owner_pid"])
        records = self.backend.records(request["owner_bus"])
        records = [row for row in records if row["window_uuid"] == request["window_uuid"]]
        if not 0 < len(records) <= 32 or sum(bool(row["active"]) for row in records) != 1:
            raise UpdateError("The original terminal tab topology is ambiguous")
        if any(not row.get("input_enabled", True) for row in records):
            raise UpdateError("The original terminal already has an input hold")
        binary = resolve_launch(request["launch_path"], request["launch_kind"])
        if binary != str(Path(request["executable"]).resolve(strict=True)):
            raise UpdateError("The terminal launch target changed")
        self.descriptor = os.open(binary, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
        metadata = os.fstat(self.descriptor)
        if not stat.S_ISREG(metadata.st_mode) or os.pread(self.descriptor, 4, 0) != b"\x7fELF":
            raise UpdateError("The replacement is not a native terminal executable")
        if os.path.samestat(metadata, os.stat(f"/proc/{owner['pid']}/exe")):
            raise UpdateError("This terminal already runs the current build")
        saved = self.backend.kwin("capture", pid=owner["pid"], title=request["window_title"])
        exact_output(saved.get("output"), saved.get("output"))
        if not saved.get("geometry") or ((saved["maximized"] or saved["fullscreen"]) and not saved.get("normal")):
            raise UpdateError("The original window geometry is unavailable")
        xml = self.backend.call(request["owner_bus"], "/org/xfce/Terminal", "org.freedesktop.DBus.Introspectable", "Introspect")[0]
        capabilities = dict(hold='name="HoldForUpdate"' in xml or "name='HoldForUpdate'" in xml)
        self.snapshot = dict(owner=owner, records=records, window=saved, binary=binary, capabilities=capabilities,
                             pinned=[metadata.st_dev, metadata.st_ino, metadata.st_size, metadata.st_mtime_ns])
        return self.snapshot

    def preflight(self, stopped=False):
        saved = self.snapshot
        if saved is None or not alive(saved["owner"]):
            raise UpdateError("The original terminal process changed")
        records = [row for row in self.backend.records(self.request["owner_bus"])
                   if row["window_uuid"] == self.request["window_uuid"]]
        if not same_tabs(saved["records"], records, stopped=stopped):
            raise UpdateError("The original tabs changed after preview")
        metadata = os.stat(saved["binary"])
        if ([metadata.st_dev, metadata.st_ino, metadata.st_size, metadata.st_mtime_ns] != saved["pinned"]
                or resolve_launch(self.request["launch_path"], self.request["launch_kind"]) != saved["binary"]):
            raise UpdateError("The terminal build or launch target changed after preview")
        actual = self.backend.kwin("capture", pid=saved["owner"]["pid"], id=saved["window"]["id"])
        placement_matches(saved["window"], actual, saved["window"]["minimized"])
        return {}

    def stage(self, value):
        if self.replacement is not None:
            raise UpdateError("A replacement is already staged; it cannot be started twice")
        self.preflight()
        tabs = value.get("tabs", [])
        if len(tabs) != len(self.snapshot["records"]):
            raise UpdateError("The prepared tab count does not match the original")
        for tab, record in zip(tabs, self.snapshot["records"], strict=True):
            argv = tab.get("argv")
            if (not isinstance(argv, list) or not 0 < len(argv) <= 128
                    or any(not isinstance(arg, str) or "\0" in arg for arg in argv)
                    or sum(len(arg.encode()) for arg in argv) > 65536
                    or not argv[0] or not isinstance(tab.get("executable", argv[0]), str)
                    or not os.path.isabs(tab.get("executable", argv[0])) or not os.access(tab.get("executable", argv[0]), os.X_OK)
                    or not os.path.isabs(tab.get("cwd", "")) or not os.path.isdir(tab["cwd"])
                    or tab.get("active") != record["active"]):
                raise UpdateError("Invalid generic terminal restoration plan")
        self.directory = tempfile.TemporaryDirectory(prefix="xfce-stage-", dir=os.environ.get("XDG_RUNTIME_DIR"))
        self.gates = StartupGates(self.directory.name, tabs)
        commands = self.gates.commands(tabs, self.helper)
        columns = rows = 0
        try:
            active = next(row for row in self.snapshot["records"] if row["active"])
            pty = os.open(active["pty"], os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
            try:
                rows, columns, _, _ = struct.unpack("HHHH", fcntl.ioctl(pty, termios.TIOCGWINSZ, b"\0" * 8))
            finally:
                os.close(pty)
        except OSError:
            pass
        environment = os.environ.copy()
        for key in ("XDG_ACTIVATION_TOKEN", "DESKTOP_STARTUP_ID"):
            environment.pop(key, None)
        environment.update(XFCE4_TERMINAL_LAUNCH_PATH=self.request["launch_path"],
            XFCE4_TERMINAL_LAUNCH_KIND=self.request["launch_kind"], XFCE4_TERMINAL_UPDATE_SERVICE=self.request["service"])
        with open(Path(self.directory.name) / "replacement.log", "wb") as log:
            self.replacement = subprocess.Popen([self.snapshot["binary"], *terminal_arguments(tabs, commands, columns, rows)],
                executable=f"/proc/self/fd/{self.descriptor}", pass_fds=(self.descriptor,), env=environment,
                stdin=subprocess.DEVNULL, stdout=log, stderr=log, start_new_session=True)
        deadline, last = time.monotonic() + 30, "The replacement has not registered"
        while time.monotonic() < deadline:
            self.backend.check_cancelled()
            if self.replacement.poll() is not None:
                raise UpdateError("Replacement exited: " + (Path(self.directory.name) / "replacement.log").read_text(errors="replace")[-8192:])
            try:
                self.owner = self.backend.terminal_owner(self.replacement.pid)
                verify_binary(self.replacement.pid, tuple(self.snapshot["pinned"][:2]))
                records = self.backend.records(self.owner)
                if len(records) != len(tabs) or any(row["active"] != tab["active"] or os.path.realpath(row["working_directory"]) != tab["cwd"]
                                                 for row, tab in zip(records, tabs, strict=True)):
                    raise UpdateError("The replacement tab order or working directory differs")
                if not self.gates.ready(records):
                    raise UpdateError("The replacement startup gates are not ready")
                self.window = self.backend.kwin("place", pid=self.replacement.pid, saved=self.snapshot["window"])
                actual = self.backend.kwin("capture", pid=self.replacement.pid, id=self.window["id"])
                placement_matches(self.snapshot["window"], actual, True)
                return self.records()
            except (UpdateError, OSError) as error:
                last = str(error)
                time.sleep(0.05)
        raise UpdateError("Replacement not ready; original retained: " + last)

    def records(self):
        if self.replacement is None or self.replacement.poll() is not None:
            raise UpdateError("The replacement terminal is unavailable")
        verify_binary(self.replacement.pid, tuple(self.snapshot["pinned"][:2]))
        return dict(owner=self.owner, records=self.backend.records(self.owner), pid=self.replacement.pid)

    def release(self):
        if self.released:
            raise UpdateError("Prepared applications have already been released")
        self.gates.release()
        self.released = True
        return {}

    def commit(self):
        if not self.released or self.closed:
            raise UpdateError("The terminal handover is not eligible or was already committed")
        self.preflight(stopped=True)
        self.records()
        self.backend.finish_replacement(self.owner)
        self.backend.close_original(self.request["window_uuid"])
        self.closed = True
        actual = self.backend.kwin("present", pid=self.replacement.pid, id=self.window["id"], saved=self.snapshot["window"])
        placement_matches(self.snapshot["window"], actual, self.snapshot["window"]["minimized"])
        return dict(original_closed=True, replacement_pid=self.replacement.pid, replacement_window_id=actual["id"])

    def recover(self):
        try:
            if self.replacement is not None and self.replacement.poll() is None:
                if self.released:
                    self.backend.finish_replacement(self.owner)
                    self.backend.kwin("present", pid=self.replacement.pid, id=self.window["id"], saved=self.snapshot["window"])
                else:
                    self.replacement.terminate()
        finally:
            self.backend.hold_original(False)
        return dict(original_closed=self.closed)

    def cleanup(self):
        if self.gates is not None:
            self.gates.close()
        if self.descriptor is not None:
            os.close(self.descriptor)
            self.descriptor = None
        if self.directory is not None:
            self.directory.cleanup()
