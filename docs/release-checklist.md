# Release checklist for 0.1.0

State of release candidate 0.1.0-rc2 against the plan's definition of done
(section 15), and the steps that turn a candidate into a release.

## Definition of done

"Met" means there is evidence in this repository. "Open" items are not
hidden behind a pass: they are what stands between this candidate and a
release that could claim field readiness.

| # | Item | State | Evidence or reason |
|---|---|---|---|
| 1 | Dependency and platform matrix is pinned and reproducible | Met | `dependencies.lock.json`; base image by digest; SRT by commit; the CI gate builds from those pins on every push |
| 2 | Protocol document and golden fixtures match the implementation | Met | `tools/gen_golden_vectors.py --check` and the unit tests run in the gate |
| 3 | Both blocks install and appear in Companion outside the source tree | Met | Gate `clean_prefix_install_python_companion_cxx`; release check from the source package in a fresh container |
| 4 | f32/cf32 values, item counts and tag offsets pass deterministic tests | Met | `qa_transfer`, `qa_tags` |
| 5 | Loss, overload, run changes, reconnects and unknown termination are visible | Met | `qa_robustness`, `run_e2e.py`, [results-m5.md](results-m5.md), [results-rc2.md](results-rc2.md) |
| 6 | No silent zero-fill, resampling, timestamp invention or security downgrade | Met | Same tests; wrong or missing passphrase is a terminal failure, never plaintext |
| 7 | All mandatory lifecycle, sanitizer, memory and integration gates pass | Met | Gate, 11 of 11, on aarch64 and x86-64; 30-minute soak on aarch64 only |
| 8 | Network experiments retain failed and inconclusive runs | Met | [results-m4.md](results-m4.md), "Runs that failed or were repeated" |
| 9 | Headline performance statements are limited to measured conditions | Met | [results-m4.md](results-m4.md), "Operating envelope" |
| 10 | One independent developer reproduces the examples from the documentation | **Open** | No person outside the project has tried. Two substitutes exist and neither is that: the install guide is executed by a script in a fresh container, and a second AI agent with no knowledge of the project followed it (D48) |
| 11 | Hardware/clock limitations and security boundaries are documented | Met | [limitations.md](limitations.md), [continuity-and-time.md](continuity-and-time.md), [block-reference.md](block-reference.md) |
| 12 | Source package, notices, changelog and compatibility statement are complete | Met | `tools/release/check.sh`, `THIRD_PARTY_NOTICES.md`, `CHANGELOG.md`, [compatibility.md](compatibility.md) |

Also required by the plan before field readiness may be claimed, and open:

| Item | State |
|---|---|
| Two-host experiment with independent clocks | Open: needs a second machine |
| Trial with acquisition hardware and an adapter for capture times and overruns | Open: needs hardware |
| Timing results from a dedicated machine | Open: needs a machine |
| A first user with a real workload | Open: none; academic project for now (owner, 2026-11-12) |

## From candidate to release

A release candidate is first a commit. The owner decides each step below;
none of them happens automatically.

State on 2026-11-14: steps 1 to 3 are done for 0.1.0-rc1. Commit `e10c51d`
is tagged `v0.1.0-rc1` and published as a GitHub pre-release with the
package from CI run 43381133959. Commit `c25c678` is tagged `v0.1.0-rc2` and
published as a pre-release on 2026-11-14 with the package from CI run
43516353024: steps 1 to 3 are done for it too. Steps 4 and 5 are open.

1. Choose the commit. Its CI run must pass all jobs: `gate`, `netem-smoke`,
   `release-check`, `blocks-netem`.
2. Take the package from that run's `release-check-x86_64` artifact, or
   build it: `tools/release/check.sh`. The same commit gives the same
   content everywhere: compare `CONTENT_SHA256`. The compressed file can
   differ between machines with different gzip programs, so publish one
   file, the one from CI, with the `SHA256SUMS` built next to it.
3. To publish the candidate: tag the commit `v0.1.0-rc2` and attach the
   package and `SHA256SUMS`. Pushing a tag starts the ordinary CI run, like
   any push; nothing else reacts to tags, and nothing is signed or published
   automatically.
4. Give the package and [install.md](install.md) to someone who has not
   seen the project. Record what they could not do. This closes item 10.
5. For the final release: set `RWSRT_RELEASE_VERSION` in `CMakeLists.txt` to
   `0.1.0`; replace `0.1.0-rc2` in [install.md](install.md), `CHANGELOG.md`,
   `MANIFEST.md`, `README.md`, [compatibility.md](compatibility.md) and
   [limitations.md](limitations.md); run the gate and the release check; tag
   `v0.1.0`.

Whether 0.1.0 is released with the open items above, as a developer release
that says so, or waits for them is the owner's decision.
