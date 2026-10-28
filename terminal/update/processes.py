"""Bounded, same-user restart plans. Never infer a session from a window title."""

import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess


class UpdateError(Exception):
    pass


def read_limited(path, limit=65536):
    with open(path, "rb") as stream:
        data = stream.read(limit + 1)
    if len(data) > limit:
        raise UpdateError(f"Metadata exceeds its safety limit: {path}")
    return data


def identity(pid):
    if not isinstance(pid, int) or pid <= 1:
        raise UpdateError("A live process identity is required")
    directory = Path(f"/proc/{pid}")
    if directory.stat().st_uid != os.getuid():
        raise UpdateError("The process belongs to another user")
    raw = read_limited(directory / "stat", 16384).decode()
    fields = raw.rsplit(")", 1)[1].split()
    if fields[0] in ("Z", "X", "x"):
        raise ProcessLookupError(pid)
    arguments = [os.fsdecode(arg) for arg in read_limited(directory / "cmdline").split(b"\0") if arg]
    return {
        "pid": pid, "start": int(fields[19]), "parent": int(fields[1]),
        "group": int(fields[2]), "tty": int(fields[4]),
        "exe": os.readlink(directory / "exe"), "argv": arguments,
    }


def alive(saved):
    try:
        current = identity(saved["pid"])
        return all(current[key] == saved[key] for key in ("pid", "start", "exe"))
    except (OSError, ValueError, UpdateError):
        return False


def children(pid):
    values = read_limited(f"/proc/{pid}/task/{pid}/children", 16384).split()
    if len(values) > 512:
        raise UpdateError("Too many child processes to identify safely")
    return [int(value) for value in values]


def kind(process):
    names = [Path(process["exe"].removesuffix(" (deleted)")).name]
    names.extend(Path(arg).name for arg in process["argv"][:3] if not arg.startswith("-"))
    for name in ("tmux", "codex", "agy", "antigravity", "htop", "nvtop", "fish", "bash", "zsh"):
        if name in names:
            return name
    return "unsupported"


def foreground(record):
    root = identity(record["child_pid"])
    pending = [(root["pid"], 0)]
    visited = set()
    candidates = []
    while pending:
        pid, depth = pending.pop()
        if pid in visited:
            continue
        visited.add(pid)
        if len(visited) > 512:
            raise UpdateError("Too many terminal processes to identify safely")
        try:
            process = identity(pid)
            if process["tty"] != root["tty"]:
                continue
            # Do not traverse into a tmux server or guess at its pane processes.
            if process["group"] == record["foreground_pgid"] and kind(process) != "unsupported":
                candidates.append((depth, process))
            if kind(process) != "tmux":
                pending.extend((child, depth + 1) for child in children(pid))
        except (OSError, ValueError):
            continue
    applications = [(depth, process) for depth, process in candidates if kind(process) not in ("fish", "bash", "zsh")]
    if applications and len({kind(process) for _, process in applications}) == 1:
        deepest = max(depth for depth, _ in applications)
        leaves = [process for depth, process in applications if depth == deepest]
        if len(leaves) == 1:
            return leaves[0]
    if not applications and root["group"] == record["foreground_pgid"] and kind(root) in ("fish", "bash", "zsh"):
        if children(root["pid"]):
            raise UpdateError("The shell has background jobs; original left untouched")
        return root
    raise UpdateError("The foreground application is unsupported or ambiguous; original left untouched")


def codex_session(process):
    ids = set()
    arguments = process["argv"]
    for first, second in zip(arguments, arguments[1:]):
        if first == "resume" and re.fullmatch(r"[0-9a-fA-F-]{36}", second):
            ids.add(second)
    directory = Path(f"/proc/{process['pid']}/fd")
    for index, entry in enumerate(directory.iterdir()):
        if index >= 512:
            raise UpdateError("Too many descriptors to identify the exact Codex session")
        try:
            target = Path(os.readlink(entry))
            if not target.name.startswith("rollout-") or target.suffix != ".jsonl":
                continue
            descriptor = os.open(entry, os.O_RDONLY | os.O_NONBLOCK | os.O_NOCTTY)
            with os.fdopen(descriptor, "rb") as stream:
                if not stat.S_ISREG(os.fstat(stream.fileno()).st_mode):
                    continue
                first = stream.readline(1024 * 1024 + 1)
            if len(first) > 1024 * 1024:
                continue
            row = json.loads(first)
            session = row.get("payload", {}).get("id", "")
            if row.get("type") == "session_meta" and re.fullmatch(r"[0-9a-fA-F-]{36}", session):
                ids.add(session)
        except (OSError, ValueError, TypeError):
            continue
    if len(ids) != 1:
        raise UpdateError("The exact Codex session is unavailable or ambiguous; refusing resume --last")
    return ids.pop()


def tmux_query(executable, socket, *arguments):
    environment = os.environ.copy()
    environment.pop("TMUX", None)
    environment.pop("TMUX_PANE", None)
    result = subprocess.run(
        [executable, "-N", "-S", socket, *arguments], env=environment,
        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        timeout=2, check=True,
    )
    if len(result.stdout) > 65536:
        raise UpdateError("tmux returned oversized metadata")
    return result.stdout.decode()


def tmux_plan(process):
    arguments = process["argv"][1:]
    explicit = None
    label = "default"
    index = 0
    while index < len(arguments) and arguments[index].startswith("-"):
        argument = arguments[index]
        index += 1
        if argument in ("-S", "-L", "-f", "-c", "-T"):
            if index == len(arguments):
                raise UpdateError("Incomplete tmux client arguments")
            value = arguments[index]
            index += 1
            if argument == "-S":
                explicit = value
            elif argument == "-L":
                label = value
        elif argument.startswith("-S"):
            explicit = argument[2:]
        elif argument.startswith("-L"):
            label = argument[2:]
    if "/" in label or not label:
        raise UpdateError("Unsupported tmux socket label")
    socket = explicit or str(Path(os.environ.get("TMUX_TMPDIR", "/tmp")) / f"tmux-{os.getuid()}" / label)
    metadata = os.stat(socket)
    if not Path(socket).is_absolute() or metadata.st_uid != os.getuid() or not stat.S_ISSOCK(metadata.st_mode):
        raise UpdateError("A verified, same-user tmux socket is required")
    executable = process["exe"].removesuffix(" (deleted)")
    rows = tmux_query(executable, socket, "list-clients", "-F", "#{client_pid}\t#{session_id}\t#{pid}\t#{session_created}")
    matches = [row.split("\t") for row in rows.splitlines() if row.split("\t")[0] == str(process["pid"])]
    if len(matches) != 1 or len(matches[0]) != 4 or not re.fullmatch(r"\$\d+", matches[0][1]):
        raise UpdateError("The exact live tmux client/session could not be identified")
    _, session, server, created = matches[0]
    return {
        "argv": [executable, "-N", "-S", socket, "attach-session", "-t", session],
        "tmux": {"socket": socket, "session": session, "server": identity(int(server)), "created": created},
    }


def capture_tab(record):
    process = foreground(record)
    program = kind(process)
    cwd = os.path.realpath(record["working_directory"])
    if not os.path.isabs(record["working_directory"]) or not os.path.isdir(cwd):
        raise UpdateError("The terminal working directory is unavailable")
    executable = process["exe"].removesuffix(" (deleted)")
    if program == "tmux":
        plan = tmux_plan(process)
    elif program == "codex":
        command = executable if Path(executable).name == "codex" else shutil.which("codex")
        if command is None:
            raise UpdateError("The Codex command is unavailable")
        flags = [arg for arg in process["argv"] if arg in ("--dangerously-bypass-approvals-and-sandbox", "--yolo")]
        plan = {"argv": [command, "resume", codex_session(process), *flags]}
    elif program in ("agy", "antigravity"):
        command = shutil.which("agy")
        if command is None:
            raise UpdateError("The Antigravity command is unavailable")
        plan = {"argv": [command, "-c"]}
    elif program in ("fish", "bash", "zsh"):
        plan = {"argv": [executable, "-l"]}
    elif program in ("htop", "nvtop"):
        plan = {"argv": [executable]}
    else:
        raise UpdateError(f"Cannot safely resume {program}")
    if not os.access(plan["argv"][0], os.X_OK):
        raise UpdateError("The saved application executable is unavailable")
    return {**plan, "cwd": cwd, "active": bool(record["active"]), "record": record,
            "process": process, "kind": program}


def same_tabs(first, second):
    keys = ("window_uuid", "tab_uuid", "active", "child_pid", "foreground_pgid", "pty", "working_directory")
    return len(first) == len(second) and all(
        all(a.get(key) == b.get(key) for key in keys) for a, b in zip(first, second)
    )
