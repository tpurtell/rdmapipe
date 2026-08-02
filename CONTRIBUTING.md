# Contributing

## Principles

Keep rdmapipe small enough to audit and unsurprising in a Unix pipeline.

- stdout is data/protocol, never logging;
- no silent TCP bulk fallback;
- no implicit checksum, encryption, or compression;
- one configured rail must remain fully supported;
- memory must be bounded independently of stream size;
- disconnect is not EOF;
- errors and signals must release listeners, QPs, MRs, children, and sockets;
- command arguments must not be reassembled through unsafe shell quoting.

## Build and checks

```sh
make clean all
make check
```

Changes to verbs or wire behavior also require native live tests on amd64 and
arm64:

```sh
tests/live.sh ostrich /path/to/remote/rdmapipe
```

At minimum exercise:

- empty, short, odd-sized, and multi-chunk streams;
- forced one channel and automatic/two channels;
- raptor↔Spark and Spark↔Spark directions;
- stdout consumer backpressure and early close;
- missing/invalid descriptor and bad token;
- sender/receiver SIGINT and SSH death;
- raw mode and shorthand argv preservation;
- exact content using an independent digest;
- no leaked processes, listeners, or registered MRs.

## Parameter tuning

Every chunk, depth, channel-selection, polling, or buffering tuning project
must add a Markdown report under `benchmarks/` and retain raw CSV data when
practical.  Record:

- purpose and hypothesis;
- exact commit and commands;
- host architecture, kernel, devices, firmware, addresses, and topology;
- independent fabric ceiling;
- source and destination choice;
- sample count, median, range, CPU, and registered memory;
- selected default and rejected alternatives;
- correctness and cleanup checks.

Prefer the smallest memory configuration within 2% of the throughput plateau.
Do not claim link-rate performance when the measured fabric, producer,
consumer, CPU, or storage ceiling is lower.

## Style

- C11, tabs for C indentation, warning-clean project code;
- checked sizes and explicit byte order at protocol boundaries;
- small functions with one lifecycle responsibility;
- comments explain invariants and tradeoffs, not syntax;
- errors identify the failing channel/stage and go to stderr;
- user-visible behavior changes update README, COMMANDS, DESIGN, and the man
  page in the same commit.
