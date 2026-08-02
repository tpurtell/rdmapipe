# rdmapipe

`rdmapipe` is a small Unix-stream tool for moving a producer's standard output
directly into a consumer on another machine over RDMA.

SSH remains responsible for host selection, authentication, remote process
lifecycle, diagnostics, and command output.  The bulk byte stream does not
traverse SSH: it moves over one or two bounded libibverbs RC channels with no
additional encryption, compression, or checksum.

The fundamental interface is deliberately composable:

```sh
producer |
  rdmapipe --send |
  ssh host 'rdmapipe --recv | consumer'
```

`rdmapipe --send` writes exactly one newline-terminated rendezvous descriptor
to stdout, flushes it, and then remains alive while it reads the producer from
stdin.  The descriptor—not the bulk data—passes through SSH.  The remote
`rdmapipe --recv` reads exactly that one line, connects back to the advertised
RDMA endpoint, and writes received bytes to stdout.

The convenient form is an RDMA-backed equivalent of feeding stdin to a remote
command:

```sh
producer | rdmapipe host -- consumer arg ...
```

It performs the same operation internally, using SSH to start a remote
receiver.  Command arguments are encoded as an argv vector and decoded before
`execvp(3)`; they are not concatenated into an unsafe shell command.

## Why it exists

Fast fabrics make SSH encryption, copies, and generic socket paths visible
bottlenecks.  Existing point tools move files well, but Unix already has an
excellent interface for connecting arbitrary producers and consumers: the
pipe.  `rdmapipe` keeps that interface and replaces only its network-sized
middle segment.

Typical uses include:

- `docker image save` into `docker image load` on a Spark;
- tar streams into a remote extractor;
- database dumps into a remote restore process;
- model, checkpoint, and artifact streams produced on demand;
- direct file or block streams when rsync semantics are unnecessary.

## Quick examples

Move a Docker image while preserving layers, tags, history, and configuration:

```sh
docker image save --platform linux/arm64 glmrt:latest |
  rdmapipe ostrich -- sudo docker image load
```

Archive a directory directly into a remote extractor:

```sh
tar -C /models -cf - glmrt |
  rdmapipe kiwi -- tar -xf - -C /srv/models
```

Compress only when CPU and compressibility make it worthwhile:

```sh
zstd -T0 -1 < checkpoint.bin |
  rdmapipe dodo -- sh -c 'zstd -d > /data/checkpoint.bin'
```

Use the transparent raw form:

```sh
set -o pipefail
docker image save glmrt:latest |
  rdmapipe --send |
  ssh emu 'set -o pipefail; rdmapipe --recv | sudo docker image load'
```

Copy one file without involving a remote shell command parser in the filename:

```sh
rdmapipe ostrich -- sh -c 'cat > "$1"' sh /data/checkpoint.bin \
  < checkpoint.bin
```

More examples and the Docker image/container distinction are in
[EXAMPLES.md](EXAMPLES.md).  The complete command contract is in
[COMMANDS.md](COMMANDS.md).

## Operating model

Version 1 is intentionally unidirectional:

```text
local stdin -> registered send rings == RDMA ==> registered receive rings
                                                    -> remote stdout/child stdin

SSH: authentication, descriptor delivery, remote stdout/stderr, exit status
TCP bootstrap: one-shot token, QP metadata, cancellation, final stream status
RDMA RC: ordered bulk DATA frames and an explicit FIN frame
```

The sender advertises active IPv4 RoCE-v2 endpoints.  The receiver enumerates
its own active endpoints and makes the final channel mapping.  A successful
RC QP transition and probe—not a hostname or subnet guess—is the connectivity
test.

Automatic mode accepts a single configured channel.  It selects two when both
peers have useful independent paths, or when a 400-Gb/s adapter faces two
200-Gb/s peer adapters.  The fixed receive rings are reposted only after their
payload has been written, so a slow consumer naturally applies bounded
backpressure.

Defaults are based on direct rdmapipe measurements on raptor and ostrich:

| Setting | Default | Endpoint payload memory |
| --- | ---: | ---: |
| channels | auto, maximum 2 | — |
| chunk size | 2 MiB | — |
| queue depth | 4 per channel | 8 MiB per channel |
| stall timeout | 300 seconds | — |

The direct-path sweep reached a 193.09-Gb/s median with two channels while
halving the registered ring from the inherited depth-8 setting.  A subsequent
Linux producer-pipe tuning pass raised its alternating median from 57.55 to
81.04 Gb/s.  Raw data and decisions are recorded under
[`benchmarks/`](benchmarks/).

## Build and install

Requirements:

- Linux with IPv4 RoCE v2;
- a C11 compiler and `make`;
- libibverbs headers and library;
- SSH for shorthand mode (raw mode can use any descriptor transport).

```sh
make
make check
sudo make install
```

For a per-user install on every endpoint:

```sh
make install PREFIX="$HOME/.local"
ssh ostrich 'command -v rdmapipe'
```

The final check is important: SSH uses the remote user's non-interactive PATH.
If necessary, use `--remote-path='~/.local/bin/rdmapipe'` or place that
directory in the remote non-interactive PATH.  Quoting preserves `~` for the
remote login shell instead of allowing the local shell to interpret it.

## Guarantees and non-goals

- Standard output is protocol-clean: descriptor only for `--send`, received
  bytes only for `--recv`, and remote-command stdout only for shorthand mode.
- All logs, topology summaries, statistics, and errors go to stderr.
- EOF is an explicit FIN frame.  Disconnect is never treated as successful
  EOF.
- Receiver write failure, early consumer exit, transport failure, timeout,
  and cancellation are reported as failures.
- Shorthand consumers run in a separate process group; failed transfers
  terminate the whole command tree and escalate if it ignores SIGTERM.
- Registered memory is bounded by channels × depth × aligned chunk size.
- The RDMA stream is not encrypted or checksummed.  Applications that require
  end-to-end integrity should provide it explicitly.
- Version 1 is not bidirectional and does not fan out one stream to many hosts.
  Both are possible future protocol additions, not hidden behavior.
- There is no TCP bulk-data fallback.  A tool named `rdmapipe` fails clearly
  when RDMA cannot be established.

## Documentation

- [COMMANDS.md](COMMANDS.md) — complete CLI and exit behavior
- [EXAMPLES.md](EXAMPLES.md) — practical compositions
- [DESIGN.md](DESIGN.md) — descriptor, bootstrap, frames, and lifecycle
- [SECURITY.md](SECURITY.md) — trust model and deployment guidance
- [CONTRIBUTING.md](CONTRIBUTING.md) — build, test, and tuning expectations
- [ROADMAP.md](ROADMAP.md) — explicit version-1 boundary and future work

## License

MIT.  See [LICENSE](LICENSE).
