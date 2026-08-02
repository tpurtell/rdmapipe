# Examples

## Docker images

Use `docker image save` and `docker image load` for images:

```sh
docker image save glmrt:latest |
  rdmapipe emu -- sudo docker image load
```

This preserves layers, tags, history, configuration, entrypoint, environment,
and other image metadata.  Multiple images can share one archive:

```sh
docker image save api:latest worker:latest base:2026-08 |
  rdmapipe kiwi -- sudo docker image load
```

Moving an ARM64 image from an amd64 development machine:

```sh
docker image save --platform linux/arm64 glmrt:latest |
  rdmapipe ostrich -- sudo docker image load
```

Docker itself may be the bottleneck while serializing and ingesting layers.
Compression is optional and can reduce rather than improve throughput on a
200/400-Gb/s fabric:

```sh
docker image save glmrt:latest |
  zstd -T0 -1 |
  rdmapipe dodo -- sh -c 'zstd -d | sudo docker image load'
```

Use `docker container export | docker image import` only when a flattened
container filesystem is actually desired:

```sh
docker container export experiment |
  rdmapipe emu -- docker image import - experiment-flat:latest
```

Export/import discards image layers and most configuration, and mounted volume
contents are not part of the exported container filesystem.

## Files and trees

One ordinary file:

```sh
rdmapipe dodo -- sh -c 'cat > "$1"' sh /data/model.safetensors \
  < model.safetensors
```

A directory without an intermediate archive file:

```sh
tar -C /srv/checkpoints -cf - run-42 |
  rdmapipe kiwi -- tar -xf - -C /srv/checkpoints
```

Preserve sparse files with tar implementations that support the option:

```sh
tar --sparse -C /vm -cf - disk-images |
  rdmapipe ostrich -- tar --sparse -xf - -C /vm
```

An independently verified stream:

```sh
sha256sum checkpoint.bin
rdmapipe emu -- sh -c 'tee "$1" | sha256sum' sh /data/checkpoint.bin \
  < checkpoint.bin
```

The hash printed remotely covers the bytes the consumer received.  rdmapipe
itself intentionally adds no checksum.

## Databases

PostgreSQL dump into a remote database:

```sh
pg_dump --format=custom app |
  rdmapipe emu -- pg_restore --dbname=app-copy --clean --if-exists
```

Logical SQL stream:

```sh
pg_dump app |
  rdmapipe kiwi -- psql app-copy
```

Database tools often become the limiting stages.  That is desirable: rdmapipe
should disappear as the network bottleneck.

## Raw composition

The raw mode works with arbitrary descriptor transport.  SSH is the normal
choice because it authenticates the receiver launch:

```sh
set -o pipefail
tar -C /models -cf - glmrt |
  rdmapipe --send |
  ssh kiwi 'set -o pipefail; rdmapipe --recv | tar -xf - -C /models'
```

Inspect the descriptor while preserving the pipeline:

```sh
producer |
  rdmapipe --send |
  tee /dev/stderr |
  ssh ostrich 'rdmapipe --recv > /data/output'
```

The descriptor contains a one-shot connection token.  Printing it is useful
for debugging on a trusted system but should not become routine logging.

Use a fixed bootstrap range when fabric firewalls require it:

```sh
producer |
  rdmapipe --send --port=7471 |
  ssh dodo 'rdmapipe --recv | consumer'
```

## Channel and queue diagnosis

Force a Spark with only one configured rail:

```sh
producer |
  rdmapipe --channels=1 --device=rocep1s0f0 ostrich -- consumer
```

Force two channels and show detailed selection:

```sh
producer |
  rdmapipe -v --channels=2 dodo -- consumer
```

Benchmark transport without destination writes:

```sh
dd if=/dev/zero bs=2M count=8192 status=none |
  rdmapipe --send --channels=2 |
  ssh ostrich 'rdmapipe --recv --discard'
```

Queue/chunk sweeps must record topology, sample count, median/range, CPU, and
registered memory.  See [`benchmarks/README.md`](benchmarks/README.md).

## Per-user installations

Install on every endpoint:

```sh
make install PREFIX="$HOME/.local"
```

Confirm the non-interactive SSH environment, not just an interactive shell:

```sh
ssh ostrich 'command -v rdmapipe; printf "%s\n" "$PATH"'
```

If it is not on that PATH:

```sh
producer |
  rdmapipe --remote-path='~/.local/bin/rdmapipe' ostrich -- consumer
```

The quotes deliberately leave `~` for the remote login shell to expand.
