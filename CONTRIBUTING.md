# Contributing

This is an academic project at release-candidate stage. Reports and patches
are welcome; there is no promise of a response time.

## Reporting a problem

Open an issue. Say what you ran, on what platform, and attach the final
status of **both** ends: the last line that `status_json()` or the headless
examples print. Some causes are visible on one end only. A status never
contains the passphrase; check anything else you paste.

Reports from the things this project could not try are especially useful:
a real network, two machines, acquisition hardware, other platforms. See
[docs/limitations.md](docs/limitations.md).

Security problems: see [SECURITY.md](SECURITY.md), not a public issue.

## Changing the code

- Read [docs/decisions.md](docs/decisions.md) first. It records why things
  are as they are and overrides the original plan where they differ.
- Everything builds and runs in the development image; the README shows how.
  `tools/dev/run.sh tools/dev/gate.sh` must pass. CI runs it on every push.
- A change in behaviour needs a test that fails without it.
- Do not weaken an assertion or a deadline to get a pass. If a gate is
  wrong, say why in the change.
- The wire protocol, version 1, is frozen
  ([docs/compatibility.md](docs/compatibility.md)). The golden vectors in
  `tests/fixtures/rwsr_v1/` do not change.
- [docs/benchmarking.md](docs/benchmarking.md) was fixed before the
  measurements were taken. Later changes go into its "Changes" section, and
  failed runs are kept.
- No secrets in code, logs, status or evidence. Passphrases reach the blocks
  through an environment variable only.

## Licence

Contributions are accepted under GPL-3.0-or-later, the licence of the
module.
