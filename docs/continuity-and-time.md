# Continuity and time

How sample coordinates, gaps and capture times travel from the input of an
SRT Sink to the output of an SRT Source. The wire rules are in
[protocol-v1.md](protocol-v1.md).

## Coordinates

Every sample has a coordinate: run, segment, sample index.

- **Run**: one start of a Sink. A random 128-bit identifier, shown as 32 hex
  digits. Restarting a Sink always starts a new run.
- **Segment**: starts at 0. A new segment begins after a gap of unknown
  length. Sample indices restart at 0 in a new segment.
- **Sample index**: counts samples within a segment, including samples that
  were lost and whose number is known.

GNU Radio item offsets count delivered items. They say nothing about what a
physical source lost before delivering them. Only an acquisition adapter
knows that, and it tells the Sink with tags.

## Input tags of the Sink

Attach them at the exact item they apply to. The Sink splits its record at
that item.

| Key | Value | Effect |
|---|---|---|
| `rwsrt.capture_time` | dict `{time_ns, uncertainty_ns}`, both unsigned integers | That sample was captured at `time_ns` Unix nanoseconds, known to within `uncertainty_ns` |
| `rwsrt.input_discontinuity` | dict `{missing_samples: n}` | Exactly `n` samples were lost before that sample. The index advances by `n` |
| `rwsrt.input_discontinuity` | dict `{unknown: true}` | An unknown number was lost. A new segment starts and the time anchor is dropped |

Rules:

- A capture-time tag that cannot be read is counted in `malformed_tags` and
  sets nothing. No time is invented.
- A discontinuity tag that cannot be read is still a discontinuity. It is
  treated as unknown and also counted in `malformed_tags`.
- In strict mode any `rwsrt.input_discontinuity` ends the Sink with a
  `continuity` failure.
- An `rx_rate` tag whose value differs from the configured rate ends the
  Sink with a `configuration` failure. A rate change needs a new run.
- All other tags, including `rx_time`, are not transported. They are counted
  in `untransported_tags`. `rx_time` is not reinterpreted as Unix time: its
  epoch and uncertainty are not known to this block.

## Output tags of the Source

| Key | Placed on | Value |
|---|---|---|
| `rwsrt.record` | first sample of every record | dict: `run_id`, `segment_id`, `first_sample_index`, `sample_count`, `record_sequence`, `rate_num`, `rate_den` |
| `rwsrt.discontinuity` | first sample after a break | dict, see below |
| `rwsrt.capture_time` | first sample of a record that carries a time | dict `{time_ns, uncertainty_ns}` |

Each tag appears once, at the absolute output offset of that sample, also
when a record is emitted across several scheduler calls.

A `rwsrt.discontinuity` value contains:

| Field | Meaning |
|---|---|
| `reasons` | Comma-separated causes, several can apply at one boundary |
| `has_previous`, `previous` | The coordinate that was expected next. Absent when nothing was observed before |
| `next` | The coordinate of the tagged sample |
| `missing_known` | Whether the number of missing samples is known |
| `missing_samples` | Present only when known. An unknown count is never written as zero |
| `connection_generation` | Counts connections of this Source |

Reasons:

| Reason | Meaning |
|---|---|
| `initial_join` | The Source joined a run that had already started. It has not seen what came before |
| `sample_gap` | Samples are missing within a segment; the count is known |
| `upstream_unknown_gap` | The sender started a new segment |
| `reconnect` | The connection was re-established before this sample. Reported even when indices are adjacent |
| `new_run` | The sender was restarted. Coordinates before and after are unrelated |
| `receiver_queue_drop` | The Source's own queue discarded samples because the flowgraph did not take them in time |

**Samples on either side of a discontinuity tag are not continuous
acquisition.** A filter, FFT or demodulator that runs across the tag
produces wrong results. Existing GNU Radio blocks do not know this tag.
After a Source in segmented mode, use blocks that reset their state at the
tag, or use strict mode.

No sample is ever created to carry a tag. A break with no sample after it,
such as a missing tail revealed by END, is published as an `event` message
on the `status` port and counted in `untagged_discontinuities`.

What the Source cannot know: whether a network gap was caused by the path
or by the sender discarding records. Both look the same from the receiving
side. The sender's own status has the answer (`tx_queue` drop counters).

## Capture time

A capture-time tag is an anchor. From it the Sink derives the time of the
first sample of every later record:

```text
time = anchor_time + floor(samples_since_anchor * 1e9 * rate_den / rate_num)
```

computed from the anchor in integer arithmetic, never by adding up a rounded
sample period. When the division is inexact the uncertainty grows by one
nanosecond; it never shrinks and is never reported as smaller than supplied.

- A known gap keeps the anchor: the sample clock kept running.
- An unknown gap drops it. Records carry no time until the next anchor.
- Samples before the first anchor carry no time.

These times are only as good as the anchor. This module does not
synchronise clocks, measure offsets or correct drift between the capture
clock and any clock at the receiver. SRT's own timestamps are a different
clock domain and are not capture times.

## Latency

`latency_ms` is the SRT receive delay. It is not the capture-to-output
delay, which also includes assembly (up to `flush_interval_ms`), time in the
local queues (up to `queue_max_age_ms` each) and the scheduler. Capture-to-
output age can only be stated when the capture clock and the receiver's
clock are related with known uncertainty.

`latency_ms` is not an upper bound either: a record that arrives after its
scheduled time is handed over at once, however late. `max_lateness_ms` on
the Source discards such records; see "Bounding the age" in
[block-reference.md](block-reference.md).
