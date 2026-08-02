# Initial failure and cleanup verification

## Purpose

A fast pipe is only composable if it fails like a pipe.  This verification
checks that protocol EOF is explicit, consumer and signal failures propagate,
and neither endpoint is left holding listeners, QPs, children, or processes.

## Test state

- Date: 2026-08-02 (Asia/Taipei)
- Code under test: `1c4caf3` plus the lifecycle/packaging changes committed
  with this report
- Local: raptor, x86_64, Linux 7.0.0-28-generic
- Remote: ostrich, aarch64, Linux 6.17.0-1026-nvidia
- Remote binary: native ARM64 build under `/tmp/rdmapipe-build.700n0N`
- Main harness: `tests/live.sh ostrich REMOTE_RDMAPIPE`

## Outcomes

| Case | Expected | Observed |
| --- | ---: | ---: |
| descriptor stdin closes before newline | syntax failure | exit 2 |
| `{}` descriptor | syntax failure | exit 2 |
| sender gets no receiver for one second | timeout | exit 75 |
| remote consumer explicitly exits 7 | preserve consumer status | exit 7 |
| consumer reads one byte and closes | stream failure, not EOF | exit 74 |
| SIGINT to shorthand supervisor | signal status | exit 130 |
| exact argv with space, empty arg, and quotes | byte-identical remote argv | pass |
| empty, odd binary, and 9 MiB streams | independent SHA-256 match | pass |
| receiver restricted to one candidate in auto mode | one channel succeeds | pass |
| forced one channel and automatic two channels | both content-exact | pass |
| post-test local `pgrep -x rdmapipe` | no process | 0 found |
| post-test remote `pgrep -x rdmapipe` | no process | 0 found |

The SIGINT case used an endless `/dev/zero` source so cancellation occurred
during active bulk traffic rather than during startup.  The supervisor
signalled both children, the remote SSH lifecycle removed the receiver and its
consumer, and the local result was corrected to prefer `128 + SIGINT` over
SSH's generic 255 disconnect code.

## Additional checks

The warning-clean unit suite runs the descriptor/argv tests on native amd64
and ARM64 builds.  The local parser suite also passed under AddressSanitizer
and UndefinedBehaviorSanitizer.  The live receiver verifies frame magic,
type, reserved field, exact global sequence, payload length, explicit FIN,
and final sender/receiver byte and message counters on every transfer.

## Outcome

The tested version-1 lifecycle satisfies the documented Unix-pipe behavior.
QP fault injection and listener/MR accounting through external tracing remain
useful future hardening, but no tested failure was converted into successful
EOF and no process survived a completed test.
