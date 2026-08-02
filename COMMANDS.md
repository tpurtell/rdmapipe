# Command reference

## Synopsis

```text
rdmapipe [OPTIONS] --send
rdmapipe [OPTIONS] --recv
rdmapipe [OPTIONS] HOST -- COMMAND [ARG ...]
rdmapipe --help
rdmapipe --version
```

The three data forms are mutually exclusive.  `--send` and `--recv` are pure
filters.  The host form is convenience orchestration around those filters and
SSH.

## Raw sender

```sh
producer | rdmapipe [OPTIONS] --send
```

The sender performs these steps:

1. Enumerate usable local IPv4 RoCE-v2 endpoints.
2. Open one TCP bootstrap listener per usable endpoint.
3. Generate a cryptographically random one-shot session token.
4. Write and flush one JSON descriptor line to stdout.
5. Accept the receiver's authenticated bootstrap connection(s).
6. Establish and probe one or two RC QPs.
7. Read stdin directly into registered send slots and post RDMA SENDs.
8. Send an explicit FIN and wait for the receiver's final status.

No other bytes are written to stdout.  The process deliberately keeps stdout
open until it exits; a receiver must read one line rather than wait for EOF.

## Raw receiver

```sh
rdmapipe [OPTIONS] --recv | consumer
```

The receiver reads one descriptor line from stdin, chooses compatible local
paths, connects to the sender, validates the session token, establishes the
QPs, and writes DATA payloads to stdout in stream order.  It reports success
only after receiving FIN and completing every output write.

For a reliable multi-stage raw pipeline, enable pipeline failure propagation:

```sh
set -o pipefail
producer |
  rdmapipe --send |
  ssh host 'set -o pipefail; rdmapipe --recv | consumer'
```

The local `pipefail` exposes sender failure.  The remote `pipefail` exposes
receiver failure; because the consumer is last, its status is returned by the
remote shell even without pipefail, but enabling it is still recommended.

## SSH shorthand

```sh
producer | rdmapipe [OPTIONS] HOST -- COMMAND [ARG ...]
```

The literal `--` is required, and every rdmapipe option must precede `HOST`.
Everything after `--` is the remote argv, including arguments that begin with
`-`.

Shorthand mode starts two children:

- a local raw sender whose descriptor stdout is connected to SSH stdin;
- `ssh HOST rdmapipe --recv --exec-argv ENCODED_ARGV`.

The remote receiver decodes the argv vector and calls `execvp(3)` in a child
whose stdin is the RDMA stream.  Command arguments are preserved exactly and
are not interpolated into a shell command.  Remote stdout and stderr remain on
SSH and are inherited by the local invocation.

Use `sh -c` explicitly when shell syntax is actually wanted:

```sh
producer |
  rdmapipe host -- sh -c 'filter | sudo consumer --flag'
```

## Transport options

### `--channels=auto|1|2`

Select the number of RC data channels.  `--rails` is an alias.

- `auto` is the default.  One usable path is sufficient.
- `1` forces one QP.
- `2` requires two QPs.  The same high-rate device may back both QPs when the
  opposite endpoint has two devices; forcing two on one low-rate path is
  permitted for diagnosis but may not improve throughput.

The sender's requested value is included in the descriptor.  A raw receiver
may use its own `--channels` value to lower an automatic request or to require
a specific count.  Conflicting explicit requirements fail before data moves.

### `--chunk-size=SIZE`

Set the registered payload capacity of each slot.  The default is `2M`.
Accepted values are 4 KiB through 8 MiB and must be multiples of 64 bytes.
Binary suffixes `K`, `M`, and `G` are supported.

The sender proposes this value in the descriptor and the receiver uses it for
the session.  A receiver-side explicit value must match.

### `--queue-depth=N`

Set registered slots per channel.  The default is 4; the accepted range is 2
through 4096.  A receiver-side explicit value must match the descriptor.

Approximate registered payload memory on each endpoint is:

```text
channels × queue-depth × chunk-size
```

Thus the defaults use 8 MiB for one channel and 16 MiB for two.

### `--device=LIST`

Restrict local discovery to a comma-separated set of libibverbs device names,
network-device names, or RoCE IPv4 addresses.  Examples:

```sh
rdmapipe --send --device=mlx5_0
rdmapipe --recv --device=rocep1s0f0
rdmapipe --send --device=10.55.0.12
```

In shorthand mode this applies to the local sender.  Use `--remote-device` for
the receiver.

### `--remote-device=LIST`

Shorthand-only receiver filter.  It is passed as one argv item to the remote
`rdmapipe` and is not interpreted by the remote shell.

### `--port=PORT`

Set the sender's first TCP bootstrap port.  The default `0` requests ephemeral
ports.  When multiple candidate interfaces are advertised, fixed ports advance
from the base port.  The TCP connection carries QP setup, cancellation, and
final status only; bulk bytes use RDMA.

### `--timeout=SECONDS`

Set the setup/stall timeout.  The default is 300 seconds.  Accepted values are
1 through 86400.  A continuously backpressured but progressing consumer does
not time out; a descriptor that never arrives, a setup exchange that stalls,
or a channel with no completion or control progress does.

### `--discard`

Receiver-only benchmark mode.  Validate and consume the entire framed stream
without writing payload bytes to stdout.  The final status and byte counters
are unchanged.  Shorthand accepts `--remote-discard` for controlled transport
benchmarks.

### `-q`, `--quiet`

Suppress topology and final statistics.  Errors are never suppressed.

### `-v`, `--verbose`

Add endpoint selection and lifecycle details to stderr.  Repeat for protocol
diagnostics.  Descriptor and stream stdout remain clean at every level.

## SSH options

### `--remote-path=PROGRAM`

Set the remote rdmapipe program for shorthand mode.  The default is
`rdmapipe`.  A common per-user value is:

```sh
--remote-path='~/.local/bin/rdmapipe'
```

The quotes preserve `~` for expansion by the remote login shell.  For safety,
shorthand accepts only a simple executable word/path here.  Use an SSH
configuration or wrapper executable for more elaborate startup behavior.

### `--ssh=PROGRAM`

Set the SSH executable.  The default is `ssh`.

### `--ssh-option=ARG`

Append one argument to the SSH command before `HOST`.  Repeat as needed:

```sh
rdmapipe --ssh-option=-p --ssh-option=2222 host -- consumer
```

SSH configuration is generally cleaner for stable host-specific settings.

## Information options

### `--help`

Print concise usage to stdout and exit successfully.

### `--version`

Print the rdmapipe version, descriptor protocol version, compiled defaults,
and libibverbs capability.

## Exit status

| Code | Meaning |
| ---: | --- |
| 0 | stream and remote command completed successfully |
| 2 | command-line or descriptor syntax error |
| 69 | no usable RDMA fabric/device or connection unavailable |
| 70 | protocol, authentication, ordering, or explicit-FIN error |
| 74 | stdin/stdout, verbs, or control-channel I/O error |
| 75 | setup or progress timeout |
| 126/127 | remote command could not be executed/found |
| 128+N | process terminated by signal N |

In shorthand mode a nonzero remote consumer status is returned when it is the
decisive failure.  If sender and remote failures happen together, rdmapipe
reports both on stderr and returns a nonzero status.

## Environment

- `RDMAPIPE_SSH` supplies the shorthand SSH executable when `--ssh` is absent.
- `RDMAPIPE_REMOTE_PATH` supplies the remote program when `--remote-path` is
  absent.

No environment variable can silently enable encryption, compression, TCP
bulk fallback, or checksum behavior.
