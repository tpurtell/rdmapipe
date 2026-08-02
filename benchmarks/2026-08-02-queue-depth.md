# Initial queue-depth and chunk tuning

## Purpose and hypothesis

This project revalidates the inherited rdmasync settings for rdmapipe's much
simpler direct stream.  The hypothesis was that 2 MiB × depth 8 would remain
near the plateau, but that a shallower ring might preserve throughput because
rdmapipe has no rsync control messages between bulk sends.

The outcome changes the default queue depth from 8 to **4**.  The 2 MiB chunk
stays unchanged.  Two-channel file-fed transfers had a 193.09-Gb/s median over
nine depth-4 samples, compared with 172.72 Gb/s at depth 8, while registered
memory fell from approximately 32 MiB to 16 MiB per endpoint.

## Test state

- Date: 2026-08-02 (Asia/Taipei)
- Code under test: `4d62df989eaa94c10359d4885dd18d1f0b22b6cf`
- Raw samples: [`2026-08-02-queue-depth.csv`](2026-08-02-queue-depth.csv)
- Sender `raptor`: x86_64, Linux 7.0.0-28-generic, `mlx5_0`, firmware
  28.43.4100, 400 Gb/s, `10.55.0.12`
- Receiver `ostrich`: aarch64, Linux 6.17.0-1026-nvidia,
  `rocep1s0f0`/`10.55.0.1` and `roceP2p1s0f0`/`10.55.0.5`, firmware
  28.45.4028, 200 Gb/s each
- Receiver mode: `--discard`, so storage and stdout copies are excluded
- Source: an 8 GiB sparse regular file, read through normal stdin directly
  into registered slots
- Sample timing: sender's first DATA post through authenticated final receiver
  status; QP/SSH setup is excluded

The earlier independent `ib_write_bw` baseline in the rdmasync project was
159.74 Gb/s median for two raptor→ostrich processes and 196.06 Gb/s for two
Spark→Spark paths.  The best rdmapipe samples approach the latter and exceed
the older raptor result.  Because receiver validation and matching 8 GiB byte
counts completed on every run, the stream results are retained, but the
raptor perftest baseline should be rerun before treating 159.74 Gb/s as the
current physical ceiling.

## Command shape

```sh
truncate -s 8G /tmp/rdmapipe-tune

rdmapipe --channels=2 --chunk-size=2M --queue-depth=4 --send \
  < /tmp/rdmapipe-tune |
  ssh ostrich 'rdmapipe --recv --discard --quiet'
```

The confirmation pass alternated configurations each round to avoid giving a
single candidate all warm or cold runs.  Exact content was tested separately
by `tests/live.sh` with SHA-256; this sweep used protocol sequence, length,
FIN, and final byte/message-count validation.

## Direct file-fed results

Medians combine the initial and alternating confirmation samples for each
configuration.  Some exploratory candidates have four or five samples; the
two deciding 2 MiB candidates have nine or ten.

| Channels | Chunk | Depth | Samples | Median Gb/s | Range Gb/s | Registered memory |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 2 | 1 MiB | 4 | 4 | 190.17 | 182.65–192.80 | 8 MiB |
| 2 | 1 MiB | 8 | 9 | 150.80 | 101.41–176.45 | 16 MiB |
| **2** | **2 MiB** | **4** | **9** | **193.09** | **188.01–193.95** | **16 MiB** |
| 2 | 2 MiB | 8 | 10 | 172.72 | 145.30–177.89 | 32 MiB |
| 2 | 2 MiB | 16 | 5 | 174.96 | 166.02–185.42 | 64 MiB |
| 2 | 4 MiB | 4 | 4 | 165.42 | 156.35–170.55 | 32 MiB |
| 2 | 4 MiB | 8 | 4 | 156.68 | 109.95–169.02 | 64 MiB |

The selected configuration is the fastest, the most stable of the high-rate
candidates, and uses the least memory within 2% of the measured plateau.

## One-channel and real-pipe check

The final check compares only depth while holding the 2 MiB chunk fixed.

| Source | Channels | Depth | Samples | Median Gb/s | Range Gb/s |
| --- | ---: | ---: | ---: | ---: | ---: |
| regular file | 1 | **4** | 3 | **118.73** | 118.71–118.73 |
| regular file | 1 | 8 | 3 | 94.29 | 94.19–94.32 |
| `dd` through a Linux pipe | 2 | **4** | 3 | **58.04** | 54.06–61.64 |
| `dd` through a Linux pipe | 2 | 8 | 3 | 53.59 | 53.18–54.59 |

The `dd | rdmapipe` case is limited by production and Linux pipe copies, not
the fabric; it must not be presented as RDMA capacity.  Depth 4 nevertheless
improves that real composition too.

One `/usr/bin/time` spot check reported:

| Depth | Sender data Gb/s | Sender user/system | Sender max RSS | Receiver user/system | Receiver max RSS |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 4 | 179.44 | 0.27/0.13 s | 20,112 KiB | 0.12/0.29 s | 19,928 KiB |
| 8 | 157.97 | 0.17/0.27 s | 35,908 KiB | 0.15/0.30 s | 36,564 KiB |

CPU percentages from short, setup-inclusive runs are noisy, so absolute
user/system seconds are recorded instead of used as the selection criterion.

## Decision

- default chunk remains 2 MiB;
- default queue depth becomes 4;
- one configured rail remains supported and benefits from the same default;
- automatic two-channel selection remains appropriate when topology allows;
- checksum, compression, and TCP bulk fallback remain off.

Future tuning should investigate completion batching and the Linux
producer-pipe ceiling separately.  Neither requires increasing the bounded
default ring.
