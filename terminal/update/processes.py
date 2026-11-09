"""Generic native process identities; application restart policy belongs to Lifecycle1."""
import os
from pathlib import Path


class UpdateError(Exception):
    pass


def read_limited(path, limit=65536):
    with open(path, "rb") as stream:
        data = stream.read(limit + 1)
    if len(data) > limit:
        raise UpdateError("Oversized native process metadata")
    return data


def identity(pid):
    if type(pid) is not int or pid <= 1:
        raise UpdateError("A live process identity is required")
    directory = Path(f"/proc/{pid}")
    if directory.stat().st_uid != os.getuid():
        raise UpdateError("The terminal process belongs to another user")
    fields = read_limited(directory / "stat").decode().rsplit(")", 1)[1].split()
    if fields[0] in ("Z", "X", "x"):
        raise ProcessLookupError(pid)
    return dict(pid=pid, start=int(fields[19]), exe=os.readlink(directory / "exe"))


def alive(saved):
    try:
        return identity(saved["pid"]) == saved
    except (OSError, ValueError, UpdateError):
        return False


def same_tabs(first, second, stopped=False):
    keys = ("window_uuid", "tab_uuid", "active", "working_directory")
    if not stopped:
        keys += ("child_pid", "foreground_pgid", "pty")
    return len(first) == len(second) and all(
        all(a.get(key) == b.get(key) for key in keys) for a, b in zip(first, second, strict=True))
