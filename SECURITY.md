# Security model

## Trust boundary

`rdmapipe` is intended for trusted, high-speed RDMA fabrics such as the
isolated RoCE network between raptor and the Sparks.

SSH authenticates the remote account and carries the rendezvous descriptor.
The bulk RDMA payload does **not** inherit SSH encryption, integrity, traffic
padding, or confidentiality.  The TCP bootstrap/control channel is likewise
not encrypted.

Do not use rdmapipe for plaintext-sensitive data on a fabric where untrusted
hosts or network observers are in scope.  Keep using SSH, TLS, an encrypted
overlay, or application-level protection in that environment.

## Session token

The sender creates a fresh 128-bit token from the kernel random source for
every invocation.  It appears in the descriptor carried through SSH and must
be presented on each TCP bootstrap connection before the sender accepts QP
metadata or allocates substantial verbs state.

The token prevents an unrelated fabric process that did not observe the SSH
descriptor from claiming the listener.  It is not:

- payload encryption;
- payload authentication;
- a user identity;
- a substitute for SSH host-key and account authentication.

Descriptors should be treated as short-lived capabilities.  Avoid storing or
logging them.  Listeners accept only the selected session paths and close as
soon as setup succeeds or times out.

## Remote command safety

Shorthand does not join `COMMAND [ARG ...]` into shell text.  It encodes the
argv vector, transmits the base64 string as a shell-safe SSH argument, validates
and decodes it remotely, then calls `execvp(3)`.

Shell behavior occurs only when the user explicitly requests it, for example:

```sh
rdmapipe host -- sh -c 'consumer > /chosen/path'
```

`HOST`, `--remote-path`, and SSH-option handling are validated or passed as
individual local `execvp` arguments.  Users remain responsible for SSH config,
the remote executable, `sudo` policy, and any explicitly invoked shell code.

## Parser and allocation limits

- descriptor length: 4096 bytes;
- endpoints: 8;
- channels: 2;
- chunk: 4 KiB–8 MiB, 64-byte aligned;
- queue depth: 2–4096;
- checked multiplication before allocation;
- fixed-size bootstrap and status records;
- exact sequence/length validation for every frame.

Malformed input fails before data transfer.  No descriptor field is treated as
a pathname or shell fragment.

## Integrity expectations

RC detects transport-level delivery failures but rdmapipe deliberately adds no
application checksum.  A successful FIN/status proves that the receiver wrote
the byte count presented by the sender through the established session; it is
not a cryptographic end-to-end digest.

Add explicit integrity when needed:

```sh
sha256sum artifact
rdmapipe host -- sh -c 'tee artifact | sha256sum' < artifact
```

## Reporting vulnerabilities

Do not publish a suspected command-execution, descriptor-authentication,
memory-safety, or cross-session issue before coordinating a fix with the
project owner.  Include the commit, architecture, command, descriptor version,
and whether an untrusted fabric participant was required.
