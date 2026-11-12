# gr-robotweax-srt

GNU Radio out-of-tree module that carries continuous sample streams over an
impaired IP network using the [Robotweax SRT](https://github.com/Robotweax/srt)
public C API, with bounded buffering, reported discontinuities and predictable
shutdown.

**Status: milestones M0 (integration spike) and M1 (protocol and core) are
complete. The GNU Radio blocks that carry samples are milestone M2 and do not
exist yet.** What exists: the RWSR v1 wire codec with golden vectors, bounded
queues, continuity accounting, the session worker over SRT, and headless
reference peers that transfer a verified test signal end to end on loopback.
Nothing has been tested under network impairment. No performance or benchmark
result is claimed.

- Specification: [docs/project-plan.md](docs/project-plan.md)
- Wire protocol: [docs/protocol-v1.md](docs/protocol-v1.md)
- Findings and decisions, and where they change the plan: [docs/decisions.md](docs/decisions.md)
- Pinned dependencies: [dependencies.lock.json](dependencies.lock.json)
- Evidence summaries: [docs/evidence/m0/](docs/evidence/m0/), [docs/evidence/m1/](docs/evidence/m1/)

## Qualified platform

| Component | Version |
|---|---|
| OS | Ubuntu 24.04 LTS in Docker: aarch64 locally, x86-64 on a hosted CI runner |
| GNU Radio | 3.10.9.2 (Ubuntu package) |
| Robotweax SRT | 0.3.2, commit `e45d0edd`, shared library, OpenSSL backend |
| Toolchain | GCC 13.3, CMake 3.28, Ninja 1.11, Python 3.12, pybind11 2.11 |

Nothing else is implied to work.

## Build and test

Everything runs in a development image built from the pins in the lockfile.
The image build needs Docker and a git checkout of Robotweax SRT that contains
the pinned commit (default: `../srt`).

```sh
tools/dev/build-image.sh [path-to-srt-checkout]
tools/dev/run.sh tools/dev/gate.sh
```

The gate covers both milestones. M0: SRT API spike, module build, lifecycle
tests, clean-prefix install, Python import, GNU Radio Companion discovery and
a C++ consumer. M1: native unit tests, golden vectors, the end-to-end suite of
the reference peers, all of it again under ASan/UBSan and TSan, and two fuzz
targets. It writes `artifacts/gate/` and exits nonzero if any gate fails. It
takes about ten minutes.

Individual steps:

```sh
tools/dev/run.sh tools/dev/build-and-test.sh
tools/dev/run.sh tools/dev/native-checks.sh 60
python3 tools/gen_golden_vectors.py --check
```

## Reference peers

Two headless programs use the same codec, queue and worker as the blocks
will, without GNU Radio. The sender produces a test signal whose every sample
value follows from its coordinate; the receiver verifies each sample. Inside
the image, after `tools/dev/native-checks.sh`:

```sh
export RWSRT_KEY='a passphrase of 10 to 79 bytes'
/tmp/rwsrt-native/rwsrt_ref_receiver --role=listener --port=9000 --passphrase-env=RWSRT_KEY &
/tmp/rwsrt-native/rwsrt_ref_sender --role=caller --port=9000 --passphrase-env=RWSRT_KEY --samples=100000
```

Each prints one JSON object when it ends. `--plaintext` replaces the
passphrase for local tests; there is no automatic fallback to it.

## Layout

| Path | Content |
|---|---|
| `include/gnuradio/robotweax_srt/` | Public block headers (no SRT types) |
| `lib/` | Wire codec, queues, continuity, session worker, SRT adapter, probe blocks |
| `python/robotweax_srt/` | pybind11 bindings and QA tests |
| `grc/` | Companion block definitions |
| `spikes/m0_srt_api/` | Standalone SRT public-API spike, no GNU Radio |
| `tests/unit/`, `tests/fuzz/`, `tests/integration/` | Native tests; `tests/` is also a standalone CMake project |
| `tests/fixtures/rwsr_v1/` | Golden byte vectors |
| `tools/reference_peer/` | Headless sender and receiver |
| `tests/install_consumer/` | C++ consumer of the installed package |
| `tools/dev/` | Image, build, test and gate scripts |
| `docker/` | Development image definition |

The `m0_probe_sink` and `m0_probe_source` blocks are scaffolding. They are
replaced by `srt_sink` and `srt_source` in milestone M2.

## License

GPL-3.0-or-later, see [LICENSE](LICENSE) and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
