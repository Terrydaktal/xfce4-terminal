#!/usr/bin/env bash
set -euo pipefail

cat >&2 <<'EOF'
Disabled: this KWin scripted Effect.Scale prototype breaks input/window geometry.

It can visually scale xfce4-terminal, but KWin still hit-tests and manages the
window as if it were unscaled. That causes symptoms like clicks landing on the
wrong window and windows failing to minimise normally.

The installed effect has been disabled/removed. Do not use this as the terminal
scale path.
EOF

kwriteconfig6 --file kwinrc --group Plugins --key xfceterminalscaleEnabled false
qdbus6 org.kde.KWin /Effects org.kde.kwin.Effects.unloadEffect xfceterminalscale >/dev/null 2>&1 || true
qdbus6 org.kde.KWin /KWin reconfigure >/dev/null 2>&1 || true
exit 1
