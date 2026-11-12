#!/usr/bin/env python
#
# Copyright 2026 Robotweax.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
"""Input tag contract of the Sink and output tags of the Source."""

import time

import pmt
from gnuradio import gr_unittest

from qa_common import (Receiver, Sender, configs, evidence, free_udp_port, make_tag, pdict,
                       test_signal, wait_done)

SEG = "segmented_live"
STRICT = "strict_stop"


def run(testcase, data, tags, sink_policy=SEG, source_policy=SEG, fmt="cf32", expect_rx_done=True):
    port = free_udp_port()
    ts, ss, tr, sr = configs(port, fmt=fmt, key=None, sink_policy=sink_policy,
                             source_policy=source_policy,
                             transport=dict(reconnect_total_timeout_ms=1500, startup_timeout_ms=5000))
    rx = Receiver(tr, sr, fmt)
    tx = Sender(ts, ss, fmt, data, tags)
    rx.tb.start()
    time.sleep(0.2)
    tx.tb.start()
    testcase.assertIsNotNone(wait_done(tx.tb, 30), "sender flowgraph did not finish")
    rx_wait = wait_done(rx.tb, 30)
    if expect_rx_done:
        testcase.assertIsNotNone(rx_wait, "receiver flowgraph did not finish")
    return tx, rx


class qa_tags(gr_unittest.TestCase):

    def test_capture_time_is_carried_and_interpolated(self):
        n = 1000
        data = test_signal("cf32", n)
        t0 = 1794484426201407000
        tags = [make_tag(100, "rwsrt.capture_time", pdict(time_ns=t0, uncertainty_ns=250))]
        tx, rx = run(self, data, tags)
        self.assertEqual(rx.data(), data)
        records = rx.tags("rwsrt.record")
        times = dict(rx.tags("rwsrt.capture_time"))
        # The tag splits the record at its exact item.
        self.assertIn(100, [o for o, _ in records])
        # No time is invented for samples before the anchor.
        self.assertTrue(all(o >= 100 for o in times))
        # Every record from the anchor on carries a time: 5000 ns per sample
        # at 200 kS/s, exact, so the uncertainty does not grow.
        later = [o for o, _ in records if o >= 100]
        self.assertEqual(sorted(times), later)
        for offset in later:
            self.assertEqual(times[offset]["time_ns"], t0 + (offset - 100) * 5000)
            self.assertEqual(times[offset]["uncertainty_ns"], 250)
        self.assertEqual(tx.status()["capture_time_tags"], 1)
        self.assertEqual(rx.status()["capture_time_tags"], len(later))
        self.assertEqual(rx.status()["session"]["end"], "complete")

    def test_inexact_rate_adds_rounding_uncertainty(self):
        # 3 samples per second cannot be expressed in whole nanoseconds.
        n = 4
        data = test_signal("f32", n)
        port = free_udp_port()
        ts, ss, tr, sr = configs(port, fmt="f32", key=None, policy=SEG,
                                 stream=dict(sample_rate_num=3, record_limit_bytes=100))
        rx = Receiver(tr, sr, "f32")
        tags = [make_tag(0, "rwsrt.capture_time", pdict(time_ns=10**9, uncertainty_ns=40))]
        tx = Sender(ts, ss, "f32", data, tags, rate=1000)  # paced faster than declared: test only
        rx.tb.start()
        time.sleep(0.2)
        tx.tb.start()
        self.assertIsNotNone(wait_done(tx.tb, 20))
        self.assertIsNotNone(wait_done(rx.tb, 20))
        times = dict(rx.tags("rwsrt.capture_time"))
        self.assertEqual(times[0], {"time_ns": 10**9, "uncertainty_ns": 40})
        self.assertEqual(times[1], {"time_ns": 10**9 + 333333333, "uncertainty_ns": 41})
        self.assertEqual(times[3], {"time_ns": 2 * 10**9, "uncertainty_ns": 40})  # exact again

    def test_known_upstream_gap_reaches_the_output_as_a_tag(self):
        n = 2000
        data = test_signal("cf32", n)
        tags = [make_tag(500, "rwsrt.input_discontinuity", pdict(missing_samples=1000))]
        tx, rx = run(self, data, tags)
        self.assertEqual(rx.data(), data)  # nothing is zero-filled or dropped
        s, r = tx.status(), rx.status()
        self.assertEqual((s["upstream_known_gaps"], s["upstream_known_gap_samples"]), (1, 1000))
        breaks = rx.tags("rwsrt.discontinuity")
        self.assertEqual(len(breaks), 1)
        offset, brk = breaks[0]
        self.assertEqual(offset, 500)  # on the first sample after the gap
        self.assertEqual(brk["reasons"], "sample_gap")
        self.assertTrue(brk["missing_known"])
        self.assertEqual(brk["missing_samples"], 1000)
        self.assertEqual(brk["previous"]["sample_index"], 500)
        self.assertEqual(brk["next"]["sample_index"], 1500)
        record = dict(rx.tags("rwsrt.record"))[500]
        self.assertEqual((record["segment_id"], record["first_sample_index"]), (0, 1500))
        self.assertEqual(r["reported_missing_samples"], 1000)
        self.assertEqual(r["session"]["continuity"]["known_missing_samples"], 1000)
        # END was seen, but the run was not observed without a gap.
        self.assertEqual(r["session"]["end"], "incomplete")
        self.assertEqual(r["next_sample_index"], n + 1000)

    def test_unknown_upstream_gap_starts_a_segment(self):
        n = 1200
        data = test_signal("cf32", n)
        tags = [make_tag(700, "rwsrt.input_discontinuity", pdict(unknown=True))]
        tx, rx = run(self, data, tags)
        self.assertEqual(rx.data(), data)
        self.assertEqual(tx.status()["upstream_unknown_gaps"], 1)
        self.assertEqual(tx.status()["malformed_tags"], 0)
        offset, brk = rx.tags("rwsrt.discontinuity")[0]
        self.assertEqual(offset, 700)
        self.assertEqual(brk["reasons"], "upstream_unknown_gap")
        self.assertFalse(brk["missing_known"])
        self.assertNotIn("missing_samples", brk)  # unknown is never reported as zero
        record = dict(rx.tags("rwsrt.record"))[700]
        self.assertEqual((record["segment_id"], record["first_sample_index"]), (1, 0))
        self.assertEqual(rx.status()["unknown_breaks"], 1)
        self.assertEqual(rx.status()["segment_id"], 1)

    def test_time_anchor_is_dropped_by_an_unknown_gap(self):
        n = 900
        data = test_signal("cf32", n)
        tags = [make_tag(0, "rwsrt.capture_time", pdict(time_ns=10**12, uncertainty_ns=5)),
                make_tag(300, "rwsrt.input_discontinuity", pdict(unknown=True))]
        tx, rx = run(self, data, tags)
        times = dict(rx.tags("rwsrt.capture_time"))
        self.assertTrue(times and all(o < 300 for o in times))  # none after the gap

    def test_unreadable_discontinuity_is_treated_as_unknown_not_ignored(self):
        data = test_signal("cf32", 600)
        tags = [make_tag(200, "rwsrt.input_discontinuity", pmt.intern("oops"))]
        tx, rx = run(self, data, tags)
        self.assertEqual(tx.status()["upstream_unknown_gaps"], 1)
        self.assertEqual(tx.status()["malformed_tags"], 1)
        self.assertEqual(rx.tags("rwsrt.discontinuity")[0][1]["reasons"], "upstream_unknown_gap")

    def test_unreadable_capture_time_invents_nothing(self):
        data = test_signal("cf32", 600)
        tags = [make_tag(10, "rwsrt.capture_time", pmt.from_double(1.0)),
                make_tag(20, "rwsrt.capture_time", pdict(time_ns=5))]  # no uncertainty given
        tx, rx = run(self, data, tags)
        self.assertEqual(tx.status()["malformed_tags"], 2)
        self.assertEqual(rx.tags("rwsrt.capture_time"), [])
        self.assertEqual(rx.data(), data)

    def test_other_tags_are_counted_not_transported(self):
        data = test_signal("cf32", 600)
        tags = [make_tag(5, "rx_time", pmt.make_tuple(pmt.from_uint64(1), pmt.from_double(0.5))),
                make_tag(6, "my_tag", pmt.PMT_T),
                make_tag(7, "rx_rate", pmt.from_double(200000.0))]  # equal to the configured rate
        tx, rx = run(self, data, tags)
        self.assertEqual(tx.status()["untransported_tags"], 3)
        keys = {pmt.symbol_to_string(t.key) for t in rx.out.tags()}
        self.assertEqual(keys, {"rwsrt.record"})
        self.assertEqual(rx.status()["session"]["end"], "complete")

    def test_rate_change_tag_is_rejected(self):
        data = test_signal("cf32", 40000)
        tags = [make_tag(2000, "rx_rate", pmt.from_double(250000.0))]
        tx, rx = run(self, data, tags, source_policy=STRICT)
        s, r = tx.status(), rx.status()
        self.assertEqual(s["session"]["failure"], "configuration")
        self.assertFalse(s["natural_end"])
        self.assertEqual(s["session"]["counters"]["end_sent"], 0)
        self.assertEqual(s["input_samples_consumed"], 2000)  # nothing after the change
        self.assertNotEqual(r["session"]["end"], "complete")
        self.assertLessEqual(r["output_samples_emitted"], 2000)

    def test_strict_sink_fails_on_an_upstream_gap(self):
        for value, detail in ((pdict(missing_samples=10), "sample gap"), (pdict(unknown=True), "unknown")):
            data = test_signal("cf32", 40000)
            tags = [make_tag(3000, "rwsrt.input_discontinuity", value)]
            tx, rx = run(self, data, tags, sink_policy=STRICT, source_policy=STRICT)
            s, r = tx.status(), rx.status()
            self.assertEqual(s["session"]["failure"], "continuity")
            self.assertIn(detail, s["session"]["failure_detail"])
            self.assertEqual(s["session"]["counters"]["end_sent"], 0)  # no success, no END
            self.assertEqual(s["input_samples_consumed"], 3000)
            # The receiver got at most the samples from before the break, and
            # no END. It may have got none: the failing Sink closes at once,
            # and whatever the receiver's playout delay still held is gone.
            got = rx.data()
            self.assertEqual(got, data[:len(got)])
            self.assertLessEqual(len(got), 3000)
            self.assertFalse(r["end_emitted"])
            self.assertFalse(r["session"]["completed"])
            self.assertEqual(r["session"]["failure"], "connection_lost")
            # "Termination unknown" needs a run to have been observed at all.
            self.assertEqual(r["session"]["termination_unknown"], len(got) > 0)
            evidence("strict_sink_upstream_gap", kind=detail, receiver_samples=len(got))

    def test_strict_source_fails_on_the_first_gap(self):
        n = 40000
        data = test_signal("cf32", n)
        tags = [make_tag(5000, "rwsrt.input_discontinuity", pdict(missing_samples=7))]
        tx, rx = run(self, data, tags, sink_policy=SEG, source_policy=STRICT)
        r = rx.status()
        got = rx.data()
        self.assertEqual(r["session"]["failure"], "continuity")
        self.assertFalse(r["session"]["completed"])
        self.assertEqual(got, data[:len(got)])
        self.assertLessEqual(len(got), 5000)  # nothing from beyond the gap is output
        self.assertEqual(rx.tags("rwsrt.discontinuity"), [])
        self.assertFalse(r["end_emitted"])


if __name__ == '__main__':
    gr_unittest.run(qa_tags)
