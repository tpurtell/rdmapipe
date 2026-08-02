#!/bin/sh
set -eu

cd "$(dirname "$0")/.."

./tests/protocol-test

version=$(./rdmapipe --version)
case "$version" in
	*"protocol 1"*"chunk=2097152 depth=4 channels=auto"*) ;;
	*) printf '%s\n' "unexpected --version output" >&2; exit 1 ;;
esac

./rdmapipe --help >/dev/null

expect_status()
{
	expected=$1
	shift
	set +e
	"$@" </dev/null >/dev/null 2>/dev/null
	actual=$?
	set -e
	if test "$actual" -ne "$expected"; then
		printf '%s\n' "expected exit $expected, got $actual: $*" >&2
		exit 1
	fi
}

expect_status 2 ./rdmapipe --channels=3 --send
expect_status 2 ./rdmapipe example.invalid true
expect_status 2 ./rdmapipe --send --ssh=ssh
expect_status 2 ./rdmapipe --recv --remote-path=rdmapipe
expect_status 2 ./rdmapipe --recv --port=123
expect_status 2 ./rdmapipe --send --discard
expect_status 2 ./rdmapipe --discard example.invalid -- true
expect_status 2 ./rdmapipe '--remote-path=bad path' example.invalid -- true

expect_status 2 ./rdmapipe --recv

set +e
printf '%s\n' '{}' | ./rdmapipe --recv >/dev/null 2>/dev/null
status=$?
set -e
test "$status" -eq 2

test_directory=$(mktemp -d)
trap 'rm -rf "$test_directory"' EXIT HUP INT TERM
marker=$test_directory/consumer-pids
fifo=$test_directory/descriptor
mkfifo "$fifo"
encoded=$(
	printf '%s\000' sh -c \
		'trap "exit 0" TERM; echo "$$" >"$1"; sh -c '\''trap "" TERM; sleep 30'\'' & echo "$!" >>"$1"; wait' \
		sh "$marker" | base64 | tr -d '\n'
)
exec 3<>"$fifo"
./rdmapipe --recv "--exec-argv=$encoded" <&3 >/dev/null 2>/dev/null &
receiver=$!
attempt=0
while test ! -s "$marker" && test "$attempt" -lt 100; do
	sleep 0.02
	attempt=$((attempt + 1))
done
test -s "$marker"
printf '%s\n' '{}' >&3
set +e
wait "$receiver"
status=$?
set -e
test "$status" -eq 2
while IFS= read -r pid; do
	attempt=0
	while kill -0 "$pid" 2>/dev/null && test "$attempt" -lt 100; do
		sleep 0.02
		attempt=$((attempt + 1))
	done
	if kill -0 "$pid" 2>/dev/null; then
		printf '%s\n' "remote consumer process $pid survived receiver failure" >&2
		exit 1
	fi
done <"$marker"
exec 3>&-
exec 3<&-

printf '%s\n' "command and protocol tests passed"
