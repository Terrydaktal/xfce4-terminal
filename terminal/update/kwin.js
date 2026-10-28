/* One short-lived placement operation. No shortcuts, timers or global tracking. */
(function () {
    "use strict";
    function rect(value) {
        return value ? {x: Math.round(value.x), y: Math.round(value.y),
            width: Math.round(value.width), height: Math.round(value.height)} : null;
    }
    function output(value) {
        if (!value || !value.name || !value.geometry) { return null; }
        var geometry = rect(value.geometry);
        geometry.name = String(value.name);
        geometry.scale = Math.round((value.devicePixelRatio || 1) * 1000);
        return geometry;
    }
    function ids(values) {
        return Array.prototype.map.call(values || [], function (value) { return String(value.id); });
    }
    function snapshot(window) {
        var mode = typeof window.maximizeMode === "number" ? window.maximizeMode :
            ((window.maximizedVertically ? 1 : 0) | (window.maximizedHorizontally ? 2 : 0));
        return {id: String(window.internalId), pid: window.pid, title: String(window.caption),
            geometry: rect(window.frameGeometry), output: output(window.output),
            normal: rect(window.fullScreen ? window.fullscreenGeometryRestore : window.maximizeGeometryRestore),
            maximized: mode, fullscreen: !!window.fullScreen, minimized: !!window.minimized,
            active: !!window.active, desktops: ids(window.desktops), activities: window.activities || [],
            keepAbove: !!window.keepAbove, keepBelow: !!window.keepBelow, noBorder: !!window.noBorder,
            shaded: !!window.shade, skipTaskbar: !!window.skipTaskbar, skipPager: !!window.skipPager,
            skipSwitcher: !!window.skipSwitcher,
            above: workspace.stackingOrder.filter(function (other) {
                return other !== window && other.stackingOrder > window.stackingOrder;
            }).map(function (other) { return String(other.internalId); })};
    }
    function terminal(window) {
        var name = String(window.resourceClass).toLowerCase();
        return !window.deleted && !window.dialog &&
            (name.indexOf("xfce4-terminal") !== -1 || name === "org.xfce.terminal");
    }
    function equal(first, second) {
        return first && second && ["name", "x", "y", "width", "height", "scale"].every(function (key) {
            return first[key] === second[key];
        });
    }
    function verifyOutput(saved) {
        if (!saved || saved.width <= 0 || saved.height <= 0 || saved.scale <= 0) {
            throw new Error("Saved monitor geometry is unavailable");
        }
        var found = workspace.screens.filter(function (screen) { return String(screen.name) === saved.name; });
        if (found.length !== 1 || !equal(output(found[0]), saved)) {
            throw new Error("Monitor geometry or scale changed; original left untouched");
        }
    }
    function select() {
        var windows = workspace.windowList().filter(function (window) {
            if (request.id) { return String(window.internalId) === request.id; }
            return terminal(window) && window.pid === request.pid;
        });
        if (!request.id && windows.length > 1 && request.title !== undefined) {
            function stable(title) { return String(title).replace(/[\u2800-\u28ff]/g, "").replace(/\s+/g, " ").trim(); }
            windows = windows.filter(function (window) { return stable(window.caption) === stable(request.title); });
        }
        if (windows.length !== 1) {
            throw new Error("Expected exactly one terminal window, found " + windows.length);
        }
        if (windows[0].pid !== request.pid) { throw new Error("The terminal window process changed"); }
        return windows[0];
    }
    try {
        var window = select();
        if (request.action !== "capture") {
            var saved = request.saved;
            verifyOutput(saved.output);
            if (request.action === "place") {
                window.minimized = true;
                window.fullScreen = false;
                window.setMaximize(false, false);
                window.noBorder = saved.noBorder;
                window.keepAbove = saved.keepAbove;
                window.keepBelow = saved.keepBelow;
                window.skipTaskbar = saved.skipTaskbar;
                window.skipPager = saved.skipPager;
                window.skipSwitcher = saved.skipSwitcher;
                var desktops = workspace.desktops.filter(function (desktop) {
                    return saved.desktops.indexOf(String(desktop.id)) !== -1;
                });
                if (desktops.length !== saved.desktops.length) { throw new Error("The saved virtual desktop is unavailable"); }
                window.desktops = desktops;
                window.activities = saved.activities;
                var geometry = (saved.maximized || saved.fullscreen) ? saved.normal : saved.geometry;
                if (!geometry || geometry.width <= 0 || geometry.height <= 0) {
                    throw new Error("Saved normal geometry is unavailable");
                }
                window.frameGeometry = geometry;
                window.setMaximize(!!(saved.maximized & 1), !!(saved.maximized & 2));
                window.fullScreen = saved.fullscreen;
                window.shade = saved.shaded;
                window.minimized = true;
            } else if (request.action === "present") {
                window.minimized = saved.minimized;
                if (saved.active && !saved.minimized) { workspace.activeWindow = window; }
                else {
                    var above = workspace.windowList().filter(function (other) {
                        return saved.above.indexOf(String(other.internalId)) !== -1;
                    });
                    if (above.length && workspace.restackWindowUnder) { workspace.restackWindowUnder(window, above[0]); }
                }
            }
        }
        callDBus(request.service, request.path, "org.xfce.Terminal.Update1", "KWinResult",
            request.nonce, JSON.stringify({ok: snapshot(window)}));
    } catch (error) {
        callDBus(request.service, request.path, "org.xfce.Terminal.Update1", "KWinResult",
            request.nonce, JSON.stringify({error: String(error)}));
    }
}());
