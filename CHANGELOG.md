# Changelog

Versions follow `MAJOR.MINOR.PATCH`. Before 1.0 any minor release may change
the block API. The wire protocol has its own version; see
[docs/compatibility.md](docs/compatibility.md).

## 0.1.0-rc1 (release candidate, tagged 2026-11-13)

First release candidate. Nothing was released before it.

### What it contains

- Two GNU Radio blocks, `srt_sink` and `srt_source`, that carry one stream of
  `f32` or `cf32` samples between two flowgraphs over Robotweax SRT in live
  mode, as Caller or Listener.
- RWSR version 1 wire protocol: 96-byte header, run, segment and sample
  coordinates on every record, an END record for finite streams. Golden byte
  vectors are in `tests/fixtures/rwsr_v1/`.
- Two continuity policies. `strict_stop` ends the run at the first loss.
  `segmented_live` continues and marks every break on the Source's output
  with a `rwsrt.discontinuity` tag that carries the number of missing
  samples when it is known.
- Capture-time tags carried from the Sink's input to the Source's output,
  with their uncertainty.
- Bounded queues at both ends, by bytes, records and age. Overflow and
  expiry are counted and reported; nothing is zero-filled or resampled.
- AES-CTR encryption with the passphrase read from an environment variable
  at start. No fallback to plaintext.
- Reconnect after an outage in segmented mode, with the gap reported.
- Status as a JSON document, a Python dictionary and messages on a `status`
  port, including the module's own version.
- GNU Radio Companion definitions for both blocks.
- Examples: two Companion flowgraphs, headless sender and receiver, and a
  spectrum display that never transforms across a gap.
- Headless reference sender and receiver without GNU Radio, also usable over
  UDP and TCP for comparison.
- A source package and an install guide that is executed in a fresh
  container as part of the release check.

### Qualification

- Ubuntu 24.04, GNU Radio 3.10.9.2, Robotweax SRT 0.3.2, on aarch64 locally
  and x86-64 on a hosted CI runner.
- Network behaviour was measured in emulation on one machine only. Results
  and the supported operating envelope: [docs/results-m4.md](docs/results-m4.md).

### Known limitations

See [docs/limitations.md](docs/limitations.md). The most important ones: not
run on a real network, between two hosts or with acquisition hardware;
encryption does not authenticate; end-to-end age is not bounded when a
network path queues for seconds instead of dropping.
