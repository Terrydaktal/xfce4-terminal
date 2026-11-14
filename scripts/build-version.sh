#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
[[ $# -le 1 ]] || exit 2
output=${1:-"$root"}
exec "${PYTHON:-python3}" "$root/scripts/fork_version.py" reserve --root "$root" --output "$output" --format build --allow-source-output
