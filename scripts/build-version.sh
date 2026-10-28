#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
[[ $# -le 1 ]] || exit 2
output=${1:-"$root"}
version=$(bash "$root/scripts/source-version.sh")
commit=$(bash "$root/scripts/source-version.sh" --commit)
[[ $version == *"+g${commit:0:12}"* ]] || {
	printf 'Source revision changed while preparing the build; retry.\n' >&2
	exit 1
}
mkdir -p -- "$output"
counter=${BUILD_COUNTER_FILE:-"$root/build/build-number"}
mkdir -p -- "$(dirname -- "$counter")"
# Reserve each number once, even across concurrent isolated build snapshots.
exec {lock_fd}>"$counter.lock"
flock -x "$lock_fd"
build_id=0
if [[ -e $counter || -L $counter ]]; then
	build_id=$(cat -- "$counter")
fi
if [[ ! $build_id =~ ^(0|[1-9][0-9]{0,11})$ || $build_id -ge 999999999999 ]]; then
	printf 'Invalid or exhausted build counter: %s\n' "$counter" >&2
	exit 1
fi
next=$(mktemp "$counter.XXXXXX")
printf '%s\n' "$((build_id + 1))" >"$next"
mv -f -- "$next" "$counter"
printf '%s\n' "$version" >"$output/SOURCE-VERSION"
printf '%s\n' "$commit" >"$output/SOURCE-COMMIT"
printf '%s\n' "$build_id" >"$output/BUILD-ID"
printf '%s-%s\n' "${version%%+g*}" "$build_id" >"$output/BUILD-VERSION"
cat "$output/BUILD-VERSION"
