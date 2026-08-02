# Roadmap

## Version 1

Version 1 is intentionally limited to one bulk direction:

```text
local stdin -> remote stdout or remote command stdin
```

It includes:

- raw `--send`/`--recv` filters;
- safe SSH shorthand;
- automatic one/two-channel RoCE-v2 transport;
- fixed registered rings and consumer backpressure;
- explicit FIN, cancellation, final status, and exit propagation;
- queue, chunk, channel, device, port, timeout, and diagnostic controls.

## Candidate version-2 work

### Bidirectional mode

`rdmapipe --bidi HOST -- COMMAND` could map local stdin/stdout to remote
stdin/stdout using a second framed stream.  It needs independent flow control,
half-close semantics, and unambiguous exit ordering.  It will not be smuggled
into protocol 1.

### One-to-many fanout

A fanout sender could read each producer chunk once and post it to one QP per
target.  Docker-image distribution to emu, kiwi, ostrich, and dodo is a strong
use case.  It needs per-target credits, failure policy, and bounded retention
when one consumer is slower.

### Unix-socket orchestration API

A local daemon could amortize device discovery and registration for repeated
short transfers.  The simple one-process tool remains the reference behavior.

### IPv6 RoCE and non-RoCE fabrics

Protocol fields can represent a future address family, but version 1 discovery
and descriptors accept IPv4 RoCE v2 only.  InfiniBand LID routing and IPv6
require explicit design and tests.

### Completion events and adaptive polling

Busy polling is appropriate on the throughput path.  A future adaptive mode
could use CQ events during long producer/consumer stalls without compromising
the measured fast path.

### Optional end-to-end digest

Applications can already compose `sha256sum`, BLAKE3, or another verifier.
An integrated digest would be opt-in, negotiated, benchmarked, and visibly
separate from the default no-checksum transport.
