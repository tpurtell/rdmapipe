#!/bin/bash
set -euo pipefail

if (($# < 1 || $# > 2)); then
	echo "usage: $0 HOST [REMOTE_RDMAPIPE]" >&2
	exit 2
fi

host=$1
remote=${2:-rdmapipe}
project=$(cd "$(dirname "$0")/.." && pwd)
local_tmp=$(mktemp -d /tmp/rdmapipe-live.XXXXXX)
remote_tmp=$(ssh "$host" 'mktemp -d /tmp/rdmapipe-live.XXXXXX')

cleanup() {
	case "$local_tmp" in
		/tmp/rdmapipe-live.*) rm -rf -- "$local_tmp" ;;
	esac
	case "$remote_tmp" in
		/tmp/rdmapipe-live.*)
			ssh "$host" "rm -rf -- '$remote_tmp'" >/dev/null 2>&1 || true ;;
	esac
}
trap cleanup EXIT

run_raw() {
	local source=$1 destination=$2 channels=$3
	"$project/rdmapipe" --quiet --channels="$channels" --send < "$source" |
		ssh "$host" "'$remote' --quiet --recv > '$destination'"
}

: > "$local_tmp/empty"
printf 'odd-sized stream\000with bytes\377' > "$local_tmp/odd"
dd if=/dev/urandom of="$local_tmp/multi" bs=1M count=9 status=none

run_raw "$local_tmp/empty" "$remote_tmp/empty" 1
run_raw "$local_tmp/odd" "$remote_tmp/odd" 1
run_raw "$local_tmp/multi" "$remote_tmp/multi-one" 1
run_raw "$local_tmp/multi" "$remote_tmp/multi-auto" auto
"$project/rdmapipe" --quiet --send < "$local_tmp/multi" |
	ssh "$host" "'$remote' --quiet --device=rocep1s0f0 --recv > '$remote_tmp/multi-one-candidate'"

local_hashes=$(sha256sum "$local_tmp/empty" "$local_tmp/odd" "$local_tmp/multi" | awk '{print $1}')
remote_hashes=$(ssh "$host" "sha256sum '$remote_tmp/empty' '$remote_tmp/odd' '$remote_tmp/multi-one' '$remote_tmp/multi-auto' '$remote_tmp/multi-one-candidate'" | awk '{print $1}')
test "$(printf '%s\n' "$remote_hashes" | sed -n '1p')" = "$(printf '%s\n' "$local_hashes" | sed -n '1p')"
test "$(printf '%s\n' "$remote_hashes" | sed -n '2p')" = "$(printf '%s\n' "$local_hashes" | sed -n '2p')"
test "$(printf '%s\n' "$remote_hashes" | sed -n '3p')" = "$(printf '%s\n' "$local_hashes" | sed -n '3p')"
test "$(printf '%s\n' "$remote_hashes" | sed -n '4p')" = "$(printf '%s\n' "$local_hashes" | sed -n '3p')"
test "$(printf '%s\n' "$remote_hashes" | sed -n '5p')" = "$(printf '%s\n' "$local_hashes" | sed -n '3p')"

"$project/rdmapipe" --quiet --remote-path="$remote" "$host" -- \
	sh -c 'cat > "$1"' sh "$remote_tmp/shorthand" < "$local_tmp/multi"
test "$(sha256sum "$local_tmp/multi" | awk '{print $1}')" = \
	"$(ssh "$host" "sha256sum '$remote_tmp/shorthand'" | awk '{print $1}')"

printf '%s\0%s\0%s' 'a b' '' 'quote'\''"' > "$local_tmp/argv-expected"
"$project/rdmapipe" --quiet --remote-path="$remote" "$host" -- \
	sh -c 'printf "%s\0%s\0%s" "$1" "$2" "$3" > "$4"' sh \
	'a b' '' 'quote'\''"' "$remote_tmp/argv" < /dev/null
test "$(sha256sum "$local_tmp/argv-expected" | awk '{print $1}')" = \
	"$(ssh "$host" "sha256sum '$remote_tmp/argv'" | awk '{print $1}')"

set +e
"$project/rdmapipe" --quiet --remote-path="$remote" "$host" -- \
	sh -c 'cat >/dev/null; exit 7' < "$local_tmp/odd"
remote_status=$?
set -e
test "$remote_status" -eq 7

set +e
dd if=/dev/zero bs=1M count=16 status=none |
	"$project/rdmapipe" --quiet --remote-path="$remote" "$host" -- \
	sh -c 'head -c 1 >/dev/null'
close_status=$?
set -e
test "$close_status" -ne 0

if pgrep -x rdmapipe >/dev/null || ssh "$host" 'pgrep -x rdmapipe >/dev/null'; then
	echo "rdmapipe process leaked after live tests" >&2
	exit 1
fi

echo "live rdmapipe tests passed against $host"
