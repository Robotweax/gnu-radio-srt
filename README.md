# gr-robotweax-srt

GNU Radio out-of-tree module that carries continuous sample streams over an
impaired IP network using the [Robotweax SRT](https://github.com/Robotweax/srt)
public C API, with bounded buffering, reported discontinuities and predictable
shutdown.

**Status: milestones M0 to M3 are complete; M4 is complete as far as one
emulated machine allows.** The module has two blocks,
`srt_sink` and `srt_source`, that carry f32 or cf32 sample streams between
two GNU Radio flowgraphs, with record, discontinuity and capture-time tags,
strict and segmented continuity modes, AES-CTR, status reporting, Companion
definitions and examples. Outages, reconnects, lost records and a vanished peer are tested with a
scripted fault-injecting relay. The transport was measured against UDP and TCP on emulated paths (netem) on
one virtualised machine; see [docs/results-m4.md](docs/results-m4.md) for
what that showed and for the supported operating envelope. **It has not been
run on a real network, between two hosts, or with acquisition hardware. Field
readiness is not claimed.** No performance or
benchmark result is claimed.

- Specification: [docs/project-plan.md](docs/project-plan.md)
- Using the blocks: [docs/block-reference.md](docs/block-reference.md)
- Tags, gaps and time: [docs/continuity-and-time.md](docs/continuity-and-time.md)
- Examples: [examples/](examples/)
- Wire protocol: [docs/protocol-v1.md](docs/protocol-v1.md)
- Measurement protocol and results: [docs/benchmarking.md](docs/benchmarking.md), [docs/results-m4.md](docs/results-m4.md)
- Findings and decisions, and where they change the plan: [docs/decisions.md](docs/decisions.md)
- Pinned dependencies: [dependencies.lock.json](dependencies.lock.json)
- Evidence summaries: [docs/evidence/m0/](docs/evidence/m0/), [docs/evidence/m1/](docs/evidence/m1/), [docs/evidence/m2/](docs/evidence/m2/), [docs/evidence/m3/](docs/evidence/m3/), [docs/evidence/m4/](docs/evidence/m4/)

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

The gate covers every milestone so far: SRT API spike, module build, native
unit tests and golden vectors, the flowgraph test suites of the two blocks
(repeated, and once more under ASan/UBSan), fault-injection tests, a
clean-prefix install with Companion and the examples run end to end, a C++
consumer, the reference-peer end-to-end suite, sanitizer runs and two fuzz
targets. It writes `artifacts/gate/` and exits nonzero if any gate fails. It
takes about twenty-five minutes.

The 30-minute soak is separate:

```sh
tools/dev/run.sh tools/dev/soak.sh 1800
```

Individual steps:

```sh
tools/dev/run.sh tools/dev/build-and-test.sh
tools/dev/run.sh tools/dev/qa.sh qa_tags
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
| `lib/` | Blocks, wire codec, queues, continuity, session worker, SRT adapter |
| `python/robotweax_srt/` | pybind11 bindings, configuration helpers, flowgraph QA suites |
| `examples/` | Companion flowgraphs and headless sender/receiver |
| `grc/` | Companion block definitions |
| `spikes/m0_srt_api/` | Standalone SRT public-API spike, no GNU Radio |
| `tests/unit/`, `tests/fuzz/`, `tests/integration/` | Native tests; `tests/` is also a standalone CMake project |
| `tests/fixtures/rwsr_v1/` | Golden byte vectors |
| `tools/reference_peer/` | Headless sender and receiver |
| `tools/impair/` | Scripted UDP fault-injection relay for robustness tests |
| `tests/netem/`, `tools/run_matrix.py`, `tools/validate_results.py` | Network qualification: namespaces with netem, run matrix, evidence validator |
| `tests/install_consumer/` | C++ consumer of the installed package |
| `tools/dev/` | Image, build, test and gate scripts |
| `docker/` | Development image definition |

## License

GPL-3.0-or-later, see [LICENSE](LICENSE) and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
