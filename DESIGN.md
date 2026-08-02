# Design and wire protocol

## Design goals

1. Preserve the Unix byte-stream abstraction.
2. Keep SSH as the authenticated orchestration and lifecycle mechanism.
3. Move bulk bytes over RDMA with no extra encryption, compression, hash, or
   heap copy.
4. Accept one usable RoCE path and aggregate two when the topology benefits.
5. Keep memory bounded and let the actual consumer provide backpressure.
6. Treat EOF, cancellation, and failure as explicit protocol states.
7. Keep the primitive independent of SSH; shorthand is a convenience layer.
8. Make stdout safe for composition and stderr useful for humans.

## Process topology

Raw mode:

```text
producer             local sender                ssh            remote receiver       consumer
   | stdin                 | stdout descriptor     | stdin             | stdout              |
   +---------------------->|---------------------->|------------------->|--------------------->|
                            \\================ RDMA DATA/FIN ===========//
```

Only the descriptor crosses the SSH stdin channel.  The sender keeps its
stdout open while transferring, so the receiver must read one line and begin
connecting immediately.

Shorthand mode creates the same topology without requiring the user to spell
out the descriptor pipe.  The local parent supervises sender and SSH children.
The remote receiver starts the decoded consumer argv with a pipe as its stdin;
consumer stdout and stderr stay attached to SSH.

## Discovery

Each endpoint enumerates `/sys/class/infiniband` and accepts candidates that:

- have port 1 in `ACTIVE` state;
- report Ethernet link layer;
- expose an `UP` associated network interface with IPv4;
- expose a RoCE-v2 GID whose IPv4-mapped address matches that interface.

Candidates record verbs device, network device, IPv4 address, GID index, GID,
and advertised link rate.  They are ordered by descending link rate, with
device names providing a deterministic tie-break.  `--device` filters by
verbs name, network name, or address.

Discovery does not assert peer connectivity.  Connectivity is proven only by
successful TCP bootstrap, RC state transitions, and a verbs probe.

## Rendezvous descriptor

Protocol 1 uses a single bounded newline-terminated JSON object.  Field order
is stable for simple inspection, but receivers parse by key.

```json
{"rdmapipe":1,"token":"0123456789abcdef0123456789abcdef","chunk":2097152,"depth":4,"channels":0,"endpoints":[{"address":"10.55.0.12","port":45123,"rate":400}]}
```

Fields:

| Field | Meaning |
| --- | --- |
| `rdmapipe` | descriptor protocol version, currently 1 |
| `token` | 128-bit random, one-shot session capability |
| `chunk` | proposed payload bytes per registered slot |
| `depth` | proposed slots per channel |
| `channels` | 0 for auto, otherwise explicit 1 or 2 |
| `endpoints` | sender RoCE addresses, TCP ports, and advertised rates |

The descriptor is limited to 4096 bytes, at most eight endpoints, numeric
ranges are checked before allocation, and only IPv4 literals are accepted.
Unknown optional JSON values are skipped with bounded nesting so compatible
extensions remain possible.  Unknown mandatory protocol versions fail;
disconnect is not a descriptor.  Reading the descriptor is covered by one
overall setup deadline rather than a fresh timeout for each byte.

## Channel selection

The receiver owns final mapping because it knows both the advertised sender
endpoints and its local candidates.

Automatic selection is:

1. two channels when both endpoints expose at least two candidates;
2. two channels when a single candidate on one endpoint is at least 1.5 times
   the rate of the other endpoint's first candidate and the other endpoint has
   at least two candidates;
3. one channel otherwise.

Mappings use distinct candidates when present and reuse the single high-rate
candidate for the asymmetric case.  `--channels=1` always selects one.
`--channels=2` creates two QPs or fails; it does not require two device names.

This specifically supports:

- Spark↔Spark with two 200-Gb/s devices;
- raptor's one 400-Gb/s device↔both Spark devices;
- a Spark configured with only one rail.

## TCP bootstrap and QP setup

The sender binds a TCP listener to each advertised RoCE IPv4 address.  The
receiver connects from its selected local RoCE address.  Each path exchanges a
fixed network-byte-order record containing:

- magic and protocol version;
- channel index and total channel count;
- selected sender endpoint index;
- chunk size and queue depth;
- QP number, random PSN, active MTU, and GID;
- the 128-bit descriptor token.

The token must match before the sender allocates substantial verbs resources.
Both sides create an RC QP, transition INIT→RTR→RTS using the exchanged
metadata, and complete a small probe.  Ready bytes on TCP finish setup.

The first bootstrap socket remains open as the control channel.  Other
bootstrap sockets close after their QPs are ready.

The control channel carries only:

- early receiver/write failure;
- sender cancellation;
- final byte/message counts and success/error status.

It never carries bulk payload and is not a silent fallback path.

## Registered rings

Every channel owns one page-aligned memory registration split into fixed
slots:

```text
[ frame header | payload capacity ][ frame header | payload capacity ] ...
```

The sender reads stdin directly into the next free registered payload slot.
The receiver writes directly from the completed registered payload to stdout
or the remote child's stdin.  No intermediate bulk ring or per-message heap
allocation is used.  On Linux the sender best-effort grows an input pipe to
the smaller of the chunk size and the system pipe limit, temporarily enables
nonblocking reads, and drains available input before polling again.  It
restores the original file status flags after transfer.

All SEND work requests are signaled.  A slot is reused only after its send
completion.  The receiver preposts its slots and reposts a slot only after its
payload has been fully written.  If the consumer slows, receive credits stop
returning and RC RNR/backpressure bounds the sender naturally.

## RDMA frame

Each SEND begins with a fixed 24-byte network-byte-order header:

| Field | Size | Meaning |
| --- | ---: | --- |
| magic | 32 bits | `RDPD` stream-frame identity |
| type | 32 bits | DATA=1, FIN=2, PROBE=3 |
| length | 32 bits | payload length, zero for FIN/PROBE |
| reserved | 32 bits | must be zero in protocol 1 |
| sequence | 64 bits | monotonically increasing stream frame number |

DATA frames are striped by `sequence % channel_count`.  The receiver polls the
expected channel and validates magic, type, length, byte count, reserved bits,
and exact sequence before exposing bytes.  RC preserves order within a QP; the
global sequence preserves order across QPs.

FIN is a real frame at the next sequence number.  QP disconnect, TCP close, or
process death before FIN is an error.

## Completion status

After FIN and all output delivery:

- a raw receiver reports stream delivery status;
- an exec receiver closes the consumer's stdin, waits for it, and incorporates
  its exit status;
- the receiver sends a fixed final status record over the authenticated TCP
  control socket;
- the sender compares byte/message counts and exits successfully only on a
  matching success record.

If stdout/child stdin closes early, the receiver sends an error status and
destroys its QPs.  The sender monitors control progress between stdin reads and
verbs operations, so it does not mistake an RNR stall for success.

## Signals and supervision

Signal handlers only record the signal.  Normal code tears down QPs,
registrations, sockets, and listeners.  An exec receiver places the remote
consumer in its own process group, so cancellation reaches the command's
descendants as well as its direct child.  Failed transfers send SIGTERM,
allow two seconds for cleanup, then use SIGKILL and reap the direct child.

In shorthand mode the parent supervises sender and SSH.  Failure of one side
terminates the other, both statuses are reaped, and a nonzero result is
returned.  SSH disconnect normally causes the remote shell and receiver to be
terminated; the sender additionally observes control/QP failure.

## Resource bounds

For `C` channels, queue depth `D`, payload `P`, and 24-byte header aligned to a
64-byte stride:

```text
registered bytes = C × D × align64(24 + P)
```

Protocol metadata, candidate arrays, CQ/QP objects, and SSH supervision are
small fixed overheads.  No buffer grows with stream length.

## Compatibility

Descriptor/bootstrap/frame version 1 is exact.  Future optional fields may be
added to the JSON descriptor, but wire changes require a new protocol version
or an explicit negotiated capability.  Version 1 never guesses around an
unknown peer.
