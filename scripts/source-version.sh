#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
if [[ $# -gt 1 || ($# == 1 && $1 != --commit) ]]; then
	printf 'Usage: source-version.sh [--commit]\n' >&2
	exit 2
fi
format=provenance
[[ ${1:-} != --commit ]] || format=commit
exec "${PYTHON:-python3}" "$root/scripts/fork_version.py" source --root "$root" --format "$format"
