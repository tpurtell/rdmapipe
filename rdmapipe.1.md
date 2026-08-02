# rdmapipe(1)

## NAME

rdmapipe — connect Unix byte streams across an RDMA fabric

## SYNOPSIS

```text
rdmapipe [OPTIONS] --send
rdmapipe [OPTIONS] --recv
rdmapipe [OPTIONS] HOST -- COMMAND [ARG ...]
```

## DESCRIPTION

`rdmapipe` transports local stdin to a remote stdout or command stdin over one
or two libibverbs RC channels.  SSH authenticates and launches the remote
process; RDMA carries the unencrypted, unchecksummed bulk byte stream.

The raw form is:

```sh
producer | rdmapipe --send | ssh host 'rdmapipe --recv | consumer'
```

The sender's stdout contains one JSON descriptor line, not the bulk stream.
The receiver reads exactly one line and connects immediately.  The shorthand:

```sh
producer | rdmapipe host -- consumer args
```

supervises the equivalent sender and SSH receiver and transmits command
arguments as an encoded argv vector.

## OPTIONS

- `--send`: run the descriptor-producing, stdin-consuming sender.
- `--recv`: read one descriptor and write the received stream.
- `--channels=auto|1|2`, `--rails=...`: select RC channel count; default auto.
- `--chunk-size=SIZE`: registered payload per slot; default 2M.
- `--queue-depth=N`: slots per channel; default 4.
- `--device=LIST`: filter local verbs/netdevice/address candidates.
- `--remote-device=LIST`: shorthand receiver filter.
- `--port=PORT`: sender bootstrap base port; default ephemeral.
- `--timeout=SECONDS`: setup/stall timeout; default 300.
- `--discard`: receiver validates but does not write payload.
- `--remote-discard`: shorthand receiver discard mode.
- `--remote-path=PROGRAM`: remote executable; default `rdmapipe`.
- `--ssh=PROGRAM`: SSH executable; default `ssh`.
- `--ssh-option=ARG`: repeatable SSH argument before HOST.
- `-q`, `--quiet`: suppress configuration and statistics.
- `-v`, `--verbose`: add stderr diagnostics; repeat for protocol detail.
- `--help`: print usage.
- `--version`: print version and protocol information.

Sizes accept binary K, M, and G suffixes.  Detailed ranges, conflict behavior,
and exit codes are documented in `COMMANDS.md` shipped with the source.

## EXAMPLES

```sh
docker image save --platform linux/arm64 app:latest |
  rdmapipe ostrich -- sudo docker image load

tar -C /models -cf - glmrt |
  rdmapipe kiwi -- tar -xf - -C /srv/models

set -o pipefail
producer | rdmapipe --send |
  ssh dodo 'set -o pipefail; rdmapipe --recv | consumer'
```

## SEMANTICS

The sender reads directly into fixed registered slots.  The receiver writes
directly from registered slots and does not repost them until consumed, giving
bounded backpressure.  EOF is an explicit FIN frame.  Disconnect before FIN,
write failure, early consumer exit, timeout, authentication failure, and QP
failure are errors.

Version 1 is unidirectional.  Remote stdout and stderr return through SSH in
shorthand mode.

## SECURITY

The descriptor carries a random one-shot token used to authenticate bootstrap
connections.  Neither the RDMA payload nor TCP control channel is encrypted or
cryptographically checksummed.  Use only on an appropriately trusted fabric or
compose application security explicitly.  See `SECURITY.md`.

## SEE ALSO

ssh(1), pipe(7), ibv_post_send(3), ibv_post_recv(3), docker-image-save(1),
docker-image-load(1)
