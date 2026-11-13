# Security

## Reporting a vulnerability

Use GitHub's private reporting for this repository: the **Security** tab,
then **Report a vulnerability**. Please do not open a public issue for a
vulnerability. This is an academic project without a response-time promise.

A problem in the transport library itself belongs to
[Robotweax SRT](https://github.com/Robotweax/srt).

## What the module does and does not protect

Stated in full in [docs/limitations.md](docs/limitations.md), "Security
boundary". In short:

- AES-CTR protects confidentiality only. It authenticates neither the
  payload nor the peer.
- The passphrase is read from an environment variable. That is a
  development convenience, not secret management.
- The default bind address is loopback. Any other address exposes a UDP
  port to whoever can reach it.

Behaviour that follows from these stated boundaries is not a vulnerability.
A way to make a block crash, hang, exhaust memory, leak the passphrase or
accept malformed records is.

## Supported versions

Only the latest release or release candidate.
