#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
[[ $# == 1 ]] || exit 2
header=$1
output=$(dirname -- "$header")
version=$(bash "$root/scripts/build-version.sh" "$output")
commit=$(cat -- "$output/SOURCE-COMMIT")
temporary=$(mktemp "$header.XXXXXX")
sed -e "s/@REVISION@/$commit/g" -e "s/@VERSION_FULL@/$version/g" \
	"$root/xfce-revision.h.in" >"$temporary"
mv -f -- "$temporary" "$header"
printf '%s\n' "$version"
