/*
 * Prototype compositor-side scale for xfce4-terminal.
 *
 * This intentionally uses KWin's effect Scale transform rather than VTE/Pango
 * font scaling. It is a probe for whether compositor scaling has acceptable
 * input behavior when applied per window.
 */

var animations = new Map();

function configuredScale() {
    var scale = Number(effect.readConfig("Scale", 1.15));
    if (!isFinite(scale) || scale <= 0) {
        return 1.15;
    }
    return scale;
}

function windowId(window) {
    if (window.internalId !== undefined) {
        return String(window.internalId);
    }
    return String(window.pid) + ":" + String(window.caption);
}

function isTerminal(window) {
    var cls = String(window.windowClass || "").toLowerCase();
    var caption = String(window.caption || "").toLowerCase();

    return cls.indexOf("xfce4-terminal") !== -1 ||
           caption.indexOf("terminal") !== -1;
}

function applyScale(window) {
    if (!window || !isTerminal(window)) {
        return;
    }

    var id = windowId(window);
    if (animations.has(id)) {
        return;
    }

    var animation = effect.set({
        window: window,
        duration: 0,
        animations: [{
            type: Effect.Scale,
            to: {
                value1: configuredScale(),
                value2: configuredScale()
            }
        }]
    });

    animations.set(id, animation);
}

function clearScale(window) {
    if (!window) {
        return;
    }

    var id = windowId(window);
    var animation = animations.get(id);
    if (animation !== undefined) {
        effect.cancel(animation);
        animations.delete(id);
    }
}

function reloadAll() {
    for (var i = 0; i < effects.stackingOrder.length; ++i) {
        clearScale(effects.stackingOrder[i]);
        applyScale(effects.stackingOrder[i]);
    }
}

effects.windowAdded.connect(applyScale);
effects.windowClosed.connect(clearScale);

if (effect.configChanged !== undefined) {
    effect.configChanged.connect(reloadAll);
}

for (var i = 0; i < effects.stackingOrder.length; ++i) {
    applyScale(effects.stackingOrder[i]);
}
