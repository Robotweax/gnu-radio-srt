# gr-robotweax-srt

GNU Radio out-of-tree module that carries continuous sample streams over an
impaired IP network using the [Robotweax SRT](https://github.com/Robotweax/srt)
public C API, with bounded buffering, reported discontinuities and predictable
shutdown.

**Status: milestone M0 (integration spike) is complete on one platform. The
module does not transport samples yet.** It contains the build and packaging
skeleton, the shared SRT runtime owner, and two lifecycle probe blocks that
qualify scheduler and transport behaviour for the real blocks. No performance
or benchmark result is claimed.

- Specification: [docs/project-plan.md](docs/project-plan.md)
- What M0 established, and where it changes the plan: [docs/decisions.md](docs/decisions.md)
- Pinned dependencies: [dependencies.lock.json](dependencies.lock.json)
- M0 evidence summary: [docs/evidence/m0/](docs/evidence/m0/)

## Qualified platform

| Component | Version |
|---|---|
| OS | Ubuntu 24.04 LTS, Linux aarch64 (Docker) |
| GNU Radio | 3.10.9.2 (Ubuntu package) |
| Robotweax SRT | 0.3.2, commit `e45d0edd`, shared library, OpenSSL backend |
| Toolchain | GCC 13.3, CMake 3.28, Ninja 1.11, Python 3.12, pybind11 2.11 |

x86-64, which the plan names as the first target, has not been run yet.
Nothing else is implied to work.

## Build and test

Everything runs in a development image built from the pins in the lockfile.
The image build needs Docker and a git checkout of Robotweax SRT that contains
the pinned commit (default: `../srt`).

```sh
tools/dev/build-image.sh [path-to-srt-checkout]
tools/dev/run.sh tools/dev/m0-gate.sh
```

The gate builds the SRT API spike and the module, runs the lifecycle tests
(repeated), installs into a clean prefix, imports the module from outside the
source tree, checks GNU Radio Companion discovery, generates and runs a
Companion flowgraph, and builds a C++ consumer against the installed package.
It writes `artifacts/m0/` (`manifest.json`, `srt_api.jsonl`,
`lifecycle.jsonl`, `verdict.json`, logs) and exits nonzero if any gate fails.

Individual steps:

```sh
tools/dev/run.sh tools/dev/build-and-test.sh
tools/dev/run.sh sh -c 'tools/dev/build-and-test.sh && tools/dev/check-install.sh'
```

## Layout

| Path | Content |
|---|---|
| `include/gnuradio/robotweax_srt/` | Public block headers (no SRT types) |
| `lib/` | Block implementations and `runtime_owner` |
| `python/robotweax_srt/` | pybind11 bindings and QA tests |
| `grc/` | Companion block definitions |
| `spikes/m0_srt_api/` | Standalone SRT public-API spike, no GNU Radio |
| `tests/install_consumer/` | C++ consumer of the installed package |
| `tools/dev/` | Image, build, test and gate scripts |
| `docker/` | Development image definition |

The `m0_probe_sink` and `m0_probe_source` blocks are scaffolding. They are
replaced by `srt_sink` and `srt_source` in milestone M2.

## License

GPL-3.0-or-later, see [LICENSE](LICENSE) and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
