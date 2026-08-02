# Linux producer-pipe input tuning

## Purpose and hypothesis

The initial transport reached approximately 193 Gb/s from a regular file but
only a 58.04-Gb/s median when `dd` fed stdin through a Linux pipe.  The fabric
and registered send rings were therefore not the limiting stages in ordinary
Unix composition.

The hypothesis was that the sender was paying too many poll/read transitions:
it performed one blocking `read(2)` after each `poll(2)`, while the default
pipe held much less than one 2 MiB RDMA chunk.  This experiment makes pipe
growth best-effort, puts the read end temporarily into nonblocking mode, and
drains every immediately available byte into the current registered slot
before polling again.

The change is retained.  The alternating seven-sample pipe median increased
from **57.55 to 81.04 Gb/s**, a **40.8% improvement**, while the three-sample
regular-file median changed from 180.81 to 184.07 Gb/s.  The sender restores
the original file status flags after transfer, and failure to resize a pipe is
non-fatal.

## Test state

- Date: 2026-08-02 (Asia/Taipei)
- Baseline: commit `54fd041`, `harden rendezvous and command lifecycle`
- Tuned code: `54fd041` plus the input fast path committed with this report
- Raw samples: [`2026-08-02-pipe-input.csv`](2026-08-02-pipe-input.csv)
- Sender `raptor`: x86_64, Linux 7.0.0-28-generic, `mlx5_0`, firmware
  28.43.4100, 400 Gb/s, `10.55.0.12`
- Receiver `ostrich`: aarch64, Linux 6.17.0-1026-nvidia,
  `rocep1s0f0`/`roceP2p1s0f0`, firmware 28.45.4028, 200 Gb/s per selected rail
- Receiver mode: `--discard`; stdout and storage are excluded
- Transfer size: 4 GiB for pipe samples and an 8 GiB sparse regular file for
  regression samples
- Transport: two channels, 2 MiB chunks, queue depth 4
- Linux pipe maximum on the sender: 1 MiB; the tuned sender confirmed a 1 MiB
  input capacity with `-vv`
- Timing: first DATA post through authenticated final status, excluding setup

The earlier independent raptor-to-ostrich two-flow `ib_write_bw` baseline was
159.74 Gb/s median.  Direct file-fed rdmapipe measurements around 193 Gb/s have
since exceeded that old baseline, so it remains historical context rather
than a current physical ceiling.

## Command shape

```sh
dd if=/dev/zero bs=1M count=4096 status=none |
  rdmapipe --channels=2 --chunk-size=2M --queue-depth=4 --send |
  ssh ostrich 'rdmapipe --quiet --recv --discard'
```

Baseline and tuned binaries were alternated once per round.  A separate
regular-file run used the same command with stdin redirected from an 8 GiB
sparse file.

## Results

| Source | Variant | Samples | Median Gb/s | Range Gb/s | Messages |
| --- | --- | ---: | ---: | ---: | ---: |
| Linux pipe | blocking one-read-per-poll | 7 | 57.55 | 50.88–60.13 | 2048–2049 |
| Linux pipe | **grown, nonblocking drain** | **7** | **81.04** | **57.64–84.58** | **2048** |
| regular file | baseline | 3 | 180.81 | 178.65–194.71 | 4096 |
| regular file | grown, nonblocking drain | 3 | 184.07 | 162.10–194.63 | 4096 |

The tuned pipe path produced exactly 2048 full-size DATA frames in every
sample.  The baseline occasionally emitted 2049 frames because its 1 ms
coalescing boundary split a chunk.  The file samples are noisy but show no
systematic regression and preserve the exact expected frame count.

One setup-inclusive `/usr/bin/time` spot check recorded:

| Variant | Data Gb/s | Sender user/system | CPU | Max RSS |
| --- | ---: | ---: | ---: | ---: |
| baseline | 45.04 | 0.03/0.36 s | 27% | 19,056 KiB |
| tuned | 65.07 | 0.02/0.53 s | 45% | 20,628 KiB |

The tuned process moves more bytes per wall-clock second and consequently
uses more system CPU during the shorter interval.  RSS remains close to the
same 16 MiB registered-ring footprint; the kernel pipe accounts separately.

## Implementation decision

- On Linux, request `min(chunk_size, pipe-max-size)` for an input pipe.  Keep
  the existing capacity if the request is denied.
- Temporarily enable `O_NONBLOCK`, drain all currently readable bytes into the
  registered slot, and poll only after `EAGAIN`.
- Restore the caller's original file status flags on every completed transfer.
- Keep the 2 MiB chunk and depth-4 defaults unchanged.
- Add no buffering copy, checksum, compression, or TCP data fallback.

The optimization preserves the direct registered-memory path and bounded
memory model.  Correctness remains governed by framed lengths, global
sequence, explicit FIN, and matching final byte/message counters.
