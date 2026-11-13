# RWSR version 1 wire protocol

Normative description of the application records that `gr-robotweax-srt`
carries over SRT. It restates section 6 of the [project plan](project-plan.md)
and fixes the points that section left open; where they differ, this document
is authoritative. Implementation: `lib/wire_codec.*`. Golden vectors:
`tests/fixtures/rwsr_v1/`.

## Transport mapping

- One record is one SRT message in Live/Message mode.
- A record never exceeds the configured record limit (default 1200 bytes),
  which must not exceed the qualified single-packet payload ceiling of the
  connection (1456 bytes for IPv4 with MSS 1500, see decisions D9).
- There is no fragmentation and no multi-message reassembly.
- Arbitrary SRT media players do not understand this payload.

## Encoding

- Header integers are big-endian.
- Sample payload is little-endian IEEE-754 binary32.
- `cf32` is the ordered pair I, Q per sample, with no padding.
- Float bit patterns are transported unchanged, including signed zero,
  infinities, subnormals and every NaN payload.

## Fixed 96-byte header

| Offset | Bytes | Field | Contract |
|---:|---:|---|---|
| 0 | 4 | `magic` | ASCII `RWSR` |
| 4 | 1 | `version` | 1 |
| 5 | 1 | `kind` | 1 DATA, 2 END |
| 6 | 2 | `flags` | Bit 0: capture timestamp valid. All other bits zero |
| 8 | 2 | `header_bytes` | 96 |
| 10 | 1 | `sample_format` | 1 f32 (4 bytes per sample), 2 cf32 (8 bytes) |
| 11 | 1 | `clock_domain` | 0 unavailable, 1 Unix/POSIX nanoseconds |
| 12 | 4 | `payload_bytes` | Exact number of bytes following the header |
| 16 | 16 | `run_id` | Random 128-bit identifier generated at Sink start |
| 32 | 8 | `segment_id` | Starts at 0; increments on unknown upstream continuity loss |
| 40 | 8 | `first_sample_index` | Coordinate of the first payload sample within the segment |
| 48 | 4 | `sample_count` | DATA: positive. END: zero |
| 52 | 4 | `sample_rate_num` | Positive, normalized with the denominator |
| 56 | 4 | `sample_rate_den` | Positive, normalized with the numerator |
| 60 | 4 | `reserved_a` | Zero |
| 64 | 8 | `capture_time_ns` | Signed time of the first payload sample, when valid |
| 72 | 8 | `capture_uncertainty_ns` | Unsigned upper bound of the time error, when valid |
| 80 | 8 | `record_sequence` | Increases by one per assembled record of the run, END included |
| 88 | 8 | `reserved_b` | Zero |

## Validation rules

A receiver rejects a message that breaks any rule. A sender never emits one.
The encoder and decoder share one rule set.

1. Length is at least 96 bytes and at most the configured record limit. The
   limit is checked before any field is read.
2. `magic`, `version` and `header_bytes` have the values above.
3. `kind` is 1 or 2. `sample_format` is 1 or 2. `clock_domain` is 0 or 1.
4. Only flag bit 0 may be set. `reserved_a` and `reserved_b` are zero.
5. Message length equals `96 + payload_bytes`.
6. DATA: `sample_count > 0` and
   `payload_bytes == sample_count * sample bytes`, computed without overflow.
7. END: `sample_count == 0`, `payload_bytes == 0`, no capture timestamp.
   `first_sample_index` is the exclusive end coordinate of its segment.
8. Rate numerator and denominator are nonzero and their GCD is 1.
9. Capture time:
   - flag clear: `clock_domain`, `capture_time_ns` and
     `capture_uncertainty_ns` are all zero;
   - flag set: `clock_domain` is 1, `capture_time_ns >= 0`, and
     `capture_time_ns + capture_uncertainty_ns` fits a signed 64-bit integer.
10. `first_sample_index + sample_count` does not exceed 2^64 - 1.
11. `record_sequence` is below 2^64 - 1. A sender starts a new run before
    either coordinate would wrap.

Points fixed here that the plan left open: a non-normalized rate is rejected
rather than normalized by the receiver; a capture time before the Unix epoch
is "impossible" in version 1; flag and clock domain must agree in both
directions.

Every DATA record carries format and rate, so the loss of any earlier record
never makes a later one undecodable.

## Identity and ordering

- A Sink start creates a new random `run_id`, segment 0, sample index 0 and
  record sequence 0.
- An automatic reconnect keeps run, segment and coordinate accounting.
- A block restart creates a new run. It never continues an old one.
- Format and rate are invariant within a run.
- `record_sequence` is assigned when a record is assembled. A record that is
  later discarded from a local queue keeps its number, so sequence gaps are
  diagnostic. Missing samples are quantified by sample indices.
- Within one segment, sample intervals are strictly increasing and never
  overlap. A known upstream gap of N samples advances the index by N.
- An unknown upstream gap starts the next segment at sample index 0. The
  number of samples missing across a segment boundary is unknown.

## Receiver behaviour

Implemented by `lib/continuity.*`. The receiver compares each accepted record
with its expectation and either accepts it, accepts it with a discontinuity
report, or fails.

| Observation | Segmented mode | Strict mode |
|---|---|---|
| Format or rate differs from configuration | terminal configuration error | same |
| First record is segment 0, index 0, sequence 0 | clean start | clean start |
| First record is anything else | `initial_join` | fail |
| Index above expectation, same segment | `sample_gap`, missing count known | fail |
| Higher segment | `upstream_unknown_gap`, missing count unknown | fail |
| First record after a reconnect | `reconnect`, even if indices are adjacent | fail (connection loss) |
| Different run on a new connection | `new_run`, reference reset | fail |
| Different run on the same connection | protocol failure | same |
| Sequence repeated or lower | protocol failure | same |
| Segment lower, or sample interval overlapping | protocol failure | same |
| Any record after END of the same run | protocol failure | same |
| END index above expectation | missing tail reported as a status event | fail |
| END index below expectation | protocol failure | same |

Several causes at one boundary are reported together. Duplicates are never
filtered silently. No sample is ever manufactured to carry a report.

A receiver that joined mid-run never claims to have observed what preceded
its first record.

## END and finite streams

- Natural upstream completion makes the Sink flush its partial DATA record
  and send END after all DATA. There is exactly one END record per run.
- A user stop is cancellation and sends no END.
- END is an ordinary Live message and there is no acknowledgement. In SRT
  live mode a lost packet is only noticed, and asked for again, when a
  later packet arrives; END is the last one. **A sender may therefore
  transmit the same END message up to three times** (since 0.1.0-rc2: three
  times, 10 ms apart). The repeats are byte-identical. Each exposes the
  loss of the one before it, and each is a complete END by itself.
- A receiver acts on the first END it receives and reads nothing after it.
  It therefore never sees a repeat, and the rule "any record after END is a
  protocol failure" is unchanged. A receiver of 0.1.0-rc1 behaves this way
  already.
- END can still be lost if every transmission is lost.
- The Source emits all accepted queued DATA before it reports END.
- A connection that closes without END is `termination_unknown`.
- "Receiver-observed complete" means: clean start, no discontinuity, and END
  whose index equals the receiver's expectation. It is a statement about the
  receiver only; the Sink never learns it.
- An empty run is a single END with segment 0, index 0, sequence 0.

## Receiver notice

Added in 0.1.0-rc2. Optional for both ends, and compatible with peers that
do not know it.

When a receiver ends the session because of what it received, it may send
one message back to the sender on the same SRT connection, so that the
sender learns why instead of only seeing the connection close. A notice is
not an RWSR record.

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 4 | magic | ASCII `RWSN` |
| 4 | 1 | version | 1 |
| 5 | 1 | reason | 1 configuration (format or rate differs), 2 protocol, 3 continuity (a strict receiver saw a loss), 4 queue overflow at the receiver |
| 6 | 2 | detail length | big-endian, 0 to 200 |
| 8 | n | detail | printable ASCII, for people; not to be parsed |

- The message length is exactly 8 plus the detail length. Anything else is
  not a notice.
- A sender that understands notices treats one as a terminal failure of the
  session with the given reason. It does not reconnect.
- A sender that does not read from its connection (0.1.0-rc1) never sees a
  notice. A receiver of 0.1.0-rc1 never sends one.
- After sending a notice the receiver keeps the connection open for one
  latency plus 150 ms, at most its stop timeout, because SRT releases the
  message to the sender only after the latency. It reads nothing in that
  time.
- The notice is one live message. It can be lost, and a run that ends
  within about two latencies of its start can complete before it arrives.
  A sender that received no notice knows nothing about the receiver.
- The sender never sends anything but records. A receiver that gets a
  notice treats it like any other malformed record.

Implementation: `lib/peer_notice.*`.

## Capture time

A record's `capture_time_ns` applies to its first sample. Times of later
samples follow from the rational rate:

```text
t(k) = capture_time_ns + floor(k * 1e9 * rate_den / rate_num)
```

computed in 128-bit integer arithmetic from the anchor sample, never by
accumulating a rounded per-sample period. When the division is inexact the
derived uncertainty is one nanosecond larger than the anchor's. Uncertainty
is never reported as smaller than what the source supplied
(`lib/sample_time.*`).

## Worked example

<!-- BEGIN GENERATED EXAMPLE -->
The record below is the fixture `tests/fixtures/rwsr_v1/timed_data.bin`,
a timed cf32 DATA record with two samples. It is produced by
`tools/gen_golden_vectors.py`, which does not use the C++ codec.

| Offset | Bytes | Field | Hex | Value |
|---:|---:|---|---|---|
| 0 | 4 | `magic` | `52 57 53 52` | `RWSR` |
| 4 | 1 | `version` | `01` | 1 |
| 5 | 1 | `kind` | `01` | 1 (DATA) |
| 6 | 2 | `flags` | `00 01` | bit 0 set: capture timestamp valid |
| 8 | 2 | `header_bytes` | `00 60` | 96 |
| 10 | 1 | `sample_format` | `02` | 2 (cf32) |
| 11 | 1 | `clock_domain` | `01` | 1 (Unix nanoseconds) |
| 12 | 4 | `payload_bytes` | `00 00 00 10` | 16 |
| 16 | 16 | `run_id` | `ff ee dd cc bb aa 99 88 77 66 55 44 33 22 11 00` | opaque |
| 32 | 8 | `segment_id` | `00 00 00 00 00 00 00 01` | 1 |
| 40 | 8 | `first_sample_index` | `00 00 01 00 00 00 00 05` | 1099511627781 (2^40 + 5) |
| 48 | 4 | `sample_count` | `00 00 00 02` | 2 |
| 52 | 4 | `sample_rate_num` | `00 0f 42 40` | 1000000 |
| 56 | 4 | `sample_rate_den` | `00 00 00 03` | 3 (333333.3 samples/s) |
| 60 | 4 | `reserved_a` | `00 00 00 00` | 0 |
| 64 | 8 | `capture_time_ns` | `18 e7 4a 13 fa b4 9e 18` | 1794484426201407000 |
| 72 | 8 | `capture_uncertainty_ns` | `00 00 00 00 00 00 00 fa` | 250 |
| 80 | 8 | `record_sequence` | `00 00 00 02 00 00 00 07` | 8589934599 (2^33 + 7) |
| 88 | 8 | `reserved_b` | `00 00 00 00 00 00 00 00` | 0 |
| 96 | 4 | payload | `00 00 80 3e` | sample 0 I = 0.25, little-endian |
| 100 | 4 | payload | `00 00 80 be` | sample 0 Q = -0.25, little-endian |
| 104 | 4 | payload | `db 0f 49 40` | sample 1 I = 3.1415927, little-endian |
| 108 | 4 | payload | `db 0f 49 c0` | sample 1 Q = -3.1415927, little-endian |

Total message length: 112 bytes. SHA-256: `c2a74ff6527c4ee9dba933be807314902f0862a484d1bbe0b54853d19dae1b31`.
<!-- END GENERATED EXAMPLE -->

## Golden vectors

| File | Content |
|---|---|
| `f32_data.bin` | f32 DATA, 4 samples: +0.0, -0.0, 1.5, a quiet NaN with payload |
| `cf32_data.bin` | cf32 DATA, 3 samples with -0.0, a signalling NaN, +inf, a subnormal |
| `timed_data.bin` | cf32 DATA with capture time, rational rate 1000000/3, 64-bit coordinates |
| `end.bin` | END closing the run of `cf32_data.bin` |
| `empty_run_end.bin` | END of an empty run |

`manifest.json` lists every field value and a SHA-256 per file.
`tools/gen_golden_vectors.py --check` fails if any committed file differs
from the generator. The files are immutable: changing one is a protocol
change and needs a new version.
