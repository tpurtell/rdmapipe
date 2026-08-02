# Benchmark reports

This directory holds parameter-tuning reports and raw measurements.

The initial implementation started from rdmasync's 2 MiB/depth-8 settings and
then measured the direct stdin/stdout path independently.  The first sweep
selected depth 4; dated reports below are authoritative for current defaults.

Reports and planned reports:

1. `FABRIC-BASELINE.md` — independent one/two-flow verbs ceilings.
2. [`2026-08-02-queue-depth.md`](2026-08-02-queue-depth.md) — initial registered
   memory, depth, and chunk sweep.
3. [`2026-08-02-pipe-input.md`](2026-08-02-pipe-input.md) — Linux producer-pipe
   capacity and nonblocking-drain tuning.
4. `PIPE-END-TO-END.md` — tar, Docker-stage, and storage ceilings where
   available.
5. [`2026-08-02-failure-cleanup.md`](2026-08-02-failure-cleanup.md) — early
   consumer, signals, timeout, exit propagation, and process cleanup.

Each report follows the evidence requirements in
[`CONTRIBUTING.md`](../CONTRIBUTING.md).
