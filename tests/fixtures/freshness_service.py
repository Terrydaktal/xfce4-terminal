#!/usr/bin/python3
"""Private-bus protocol oracle. Never used by the terminal's production code."""
import json
import os
from pathlib import Path
import re
import subprocess
import sys

import gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

SERVICE = "com.terrydaktal.ApplicationLauncher"
INTERFACE = SERVICE + ".Freshness1"
bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
entries = {}
generation = 0
images = {}
disks = {}


def build_id(path):
    text = subprocess.check_output(["readelf", "-n", str(path)], stderr=subprocess.DEVNULL, timeout=2).decode()
    return re.search(r"Build ID: ([0-9a-f]+)", text).group(1)


def inspect(target):
    result = dict(schema_version=1, generation=0, target=target, status="unknown",
                  start_time=target["start_time"], image_dev=target["image_dev"], image_ino=target["image_ino"])
    try:
        pid = target["pid"]
        root = Path(f"/proc/{pid}")
        fields = (root / "stat").read_text().rsplit(") ", 1)[1].split()
        start = int(fields[19])
        info = (root / "exe").stat()
        identity = (pid, start, info.st_dev, info.st_ino)
        for name, actual in zip(("start_time", "image_dev", "image_ino"), identity[1:]):
            if target[name] not in (0, actual):
                return result
        if identity not in images:
            images[identity] = build_id(root / "exe")
        path = Path(target["launch_path"] or os.readlink(root / "exe").removesuffix(" (deleted)"))
        if path.name == "run-patched-xfce4-terminal.sh":
            path = path.resolve().parent / "build/terminal/xfce4-terminal"
        disk = path.stat()
        if not path.is_file() or not disk.st_mode & 0o111:
            return result
        key = (disk.st_dev, disk.st_ino, disk.st_mtime_ns, disk.st_ctime_ns)
        if key not in disks:
            disks[key] = build_id(path)
        result.update(start_time=start, image_dev=info.st_dev, image_ino=info.st_ino,
                      status="current" if images[identity] == disks[key] else "outdated")
    except (OSError, ValueError, AttributeError, subprocess.SubprocessError):
        pass
    return result


def poll():
    global generation
    for key, previous in list(entries.items()):
        result = inspect(previous["target"])
        result["generation"] = previous["generation"]
        if result != previous:
            generation += 1
            result["generation"] = generation
            entries[key] = result
            bus.emit_signal(None, "/Freshness", INTERFACE, "ProcessStatusChanged",
                            GLib.Variant("(s)", (json.dumps(result),)))
    return GLib.SOURCE_CONTINUE


def method(connection, sender, path, interface, name, parameters, invocation):
    global generation
    target = dict(zip(("pid", "start_time", "image_dev", "image_ino", "launch_path"), parameters.unpack()))
    key = tuple(target.values())
    if key not in entries:
        generation += 1
        result = inspect(target)
        result["generation"] = generation
        entries[key] = result
    invocation.return_value(GLib.Variant("(s)", (json.dumps(entries[key]),)))


xml = """<node><interface name='com.terrydaktal.ApplicationLauncher.Freshness1'>
<method name='GetProcessStatus'><arg type='u' direction='in'/><arg type='t' direction='in'/>
<arg type='t' direction='in'/><arg type='t' direction='in'/><arg type='s' direction='in'/>
<arg type='s' direction='out'/></method><signal name='ProcessStatusChanged'><arg type='s'/></signal>
</interface></node>"""
node = Gio.DBusNodeInfo.new_for_xml(xml)
bus.register_object("/Freshness", node.interfaces[0], method, None, None)
bus.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
              GLib.Variant("(su)", (SERVICE, 4)), None, 0, 1000, None)
GLib.timeout_add(100, poll)
print("READY", flush=True)
GLib.MainLoop().run()
