#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
operation=${1:-history}
if [[ $# -gt 0 ]]; then shift; fi
case "$operation" in
history | rebase) ;;
*)
	printf 'Usage: bump-version.sh history | rebase --base COMMIT --upstream VERSION\n' >&2
	exit 2
	;;
esac
exec "${PYTHON:-python3}" "$root/scripts/fork_version.py" "$operation" --root "$root" "$@"
