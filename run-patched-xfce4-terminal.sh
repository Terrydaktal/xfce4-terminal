#!/usr/bin/env bash
set -euo pipefail

SCRIPT_PATH="$(readlink -f -- "${BASH_SOURCE[0]}")"
SCRIPT_DIR="$(cd "$(dirname "${SCRIPT_PATH}")" && pwd)"

# Retain the entry symlink, not only its current destination, for stale-build checks.
export XFCE4_TERMINAL_LAUNCH_PATH="${BASH_SOURCE[0]}"
export XFCE4_TERMINAL_LAUNCH_KIND=repo-wrapper

exec "${SCRIPT_DIR}/build/terminal/xfce4-terminal" "$@"
