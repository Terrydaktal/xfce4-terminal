#!/usr/bin/python3
"""Mock systemd-run/update worker for a private-bus GTK test only."""
import json
import os
from pathlib import Path
import sys

import gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

mode = int(os.environ["TERMINAL_UPDATE_TEST_SCENARIO"])
if mode == 11:
    sys.exit(1)
request = json.loads(sys.argv[sys.argv.index("--serve") + 1])
state_file = Path(os.environ["TERMINAL_UPDATE_TEST_DIRECTORY"]) / "worker.json"
counts = {"begins": 1, "reports": 0, "starts": 0}
phase = "preparing" if mode == 9 else "failed" if mode == 7 else "ready"
loop = GLib.MainLoop()
bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
xml = """<node><interface name='org.xfce.Terminal.Update1'>
<method name='GetStatus'><arg type='s' direction='out'/></method>
<method name='Confirm'><arg type='b' direction='in'/><arg type='s' direction='out'/></method>
<method name='Cancel'><arg type='s' direction='out'/></method>
</interface></node>"""


def persist():
    temporary = state_file.with_suffix(".tmp")
    temporary.write_text(json.dumps(counts))
    temporary.replace(state_file)


def method(connection, sender, path, interface, name, parameters, invocation):
    global phase
    if name == "GetStatus":
        counts["reports"] += 1
    elif name == "Confirm":
        counts["starts"] += 1
        phase = "failed" if mode in (12, 13) else "complete"
    elif name == "Cancel":
        GLib.timeout_add(25, lambda: (loop.quit(), False)[1])
    persist()
    report = {"operation_id": "other" if mode == 5 else request["job_id"],
              "window_uuid": "other" if mode == 3 else request["window_uuid"],
              "owner_pid": request["owner_pid"] + (1 if mode == 4 else 0),
              "phase": phase, "message": "Isolated terminal-owned update fixture",
              "preserves_processes": mode not in (1, 2, 8, 10)}
    invocation.return_value(GLib.Variant("(s)", ("broken" if mode == 6 else json.dumps(report),)))


node = Gio.DBusNodeInfo.new_for_xml(xml)
bus.register_object("/org/xfce/Terminal/Update", node.interfaces[0], method, None, None)
bus.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
              "RequestName", GLib.Variant("(su)", (request["service"], 4)), None, 0, 1000, None)
persist()
GLib.timeout_add_seconds(20, lambda: (loop.quit(), False)[1])
loop.run()
