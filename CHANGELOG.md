# Changelog

Versions follow `MAJOR.MINOR.PATCH`. Before 1.0 any minor release may change
the block API. The wire protocol has its own version; see
[docs/compatibility.md](docs/compatibility.md).

## 0.1.0-rc2 (release candidate, not tagged)

All three answer findings recorded for 0.1.0-rc1. The record format is
unchanged and both ends interoperate with 0.1.0-rc1.

- **Lateness bound.** New Source parameter `max_lateness_ms` (default 0,
  off). A record that SRT hands over later than this after its scheduled
  time is discarded and reported as a gap of exact size. New counters
  `late_records_dropped`, `late_samples_dropped`, `max_lateness_seen_ms`.
  In an emulated path that queued for 10 s, the oldest delivered sample was
  6.3 s old without the bound and under 0.5 s with a bound of 150 ms.
- **END is sent three times**, 10 ms apart. A lost END could not be
  recovered by SRT and cost the Source its 60 s reconnect budget; this was
  seen in 1 of 6 emulated runs at 1 % loss. New counter `end_repeats_sent`.
- **Receiver notice.** A Source that ends a run because of what it
  received (rate or format mismatch, a loss in strict mode) tells the Sink,
  which now fails with the same reason and `reported by the peer: ...`
  instead of `connection_lost` or silent retries. New counters
  `peer_notices_sent`, `peer_notices_received`.
- The fault-injection relay can drop only the first END packets
  (`--drop-end-first`) and stall a path without losing packets (`--stall`).
- `SHA256SUMS` of the source package lists only the shipped file; the
  content hash is in `CONTENT_SHA256`.
- Added `CONTRIBUTING.md` and `SECURITY.md`.

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
