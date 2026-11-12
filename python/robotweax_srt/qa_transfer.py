#!/usr/bin/env python
#
# Copyright 2026 Robotweax.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
"""Sink -> SRT -> Source in two flowgraphs: values, counts, record tags, END."""

import math
import struct
import time

from gnuradio import gr_unittest

from qa_common import (KEY_ENV, PER_RECORD, Receiver, Sender, configs, evidence, free_udp_port,
                       no_secret, test_signal, wait_done, wait_for)


def transfer(testcase, fmt, sink_role, key, data, receiver_first=True, start_gap=0.2,
             max_noutput=None, **cfg):
    port = free_udp_port()
    ts, ss, tr, sr = configs(port, fmt=fmt, sink_role=sink_role, key=key, **cfg)
    rx = Receiver(tr, sr, fmt, max_noutput=max_noutput)
    tx = Sender(ts, ss, fmt, data)
    first, second = (rx, tx) if receiver_first else (tx, rx)
    first.tb.start()
    time.sleep(start_gap)
    second.tb.start()
    tx_wait = wait_done(tx.tb, 30)
    rx_wait = wait_done(rx.tb, 30)
    testcase.assertIsNotNone(tx_wait, "sender flowgraph did not finish")
    testcase.assertIsNotNone(rx_wait, "receiver flowgraph did not finish")
    return tx, rx


class qa_transfer(gr_unittest.TestCase):

    def check_complete(self, tx, rx, fmt, data):
        n = len(data)
        s, r = tx.status(), rx.status()
        got = rx.data()
        self.assertEqual(len(got), n)
        self.assertEqual(got, list(data))  # values and order, exactly
        # Sink: natural end, END handed to SRT exactly once.
        self.assertTrue(s["natural_end"])
        self.assertEqual(s["input_samples_consumed"], n)
        self.assertEqual(s["session"]["failure"], "none")
        self.assertTrue(s["session"]["completed"])
        self.assertEqual(s["session"]["counters"]["end_sent"], 1)
        self.assertEqual(s["session"]["counters"]["samples_send_accepted"], n)
        self.assertEqual(s["tx_queue"]["dropped_overflow_records"], 0)
        self.assertEqual(s["tx_queue"]["dropped_expired_records"], 0)
        # Source: receiver-observed complete, nothing missing, nothing invented.
        self.assertEqual(r["output_samples_emitted"], n)
        self.assertTrue(r["end_emitted"])
        self.assertEqual(r["session"]["end"], "complete")
        self.assertTrue(r["session"]["completed"])
        self.assertFalse(r["session"]["termination_unknown"])
        self.assertEqual(r["session"]["failure"], "none")
        self.assertEqual(r["discontinuity_tags"], 0)
        self.assertEqual(r["untagged_discontinuities"], 0)
        self.assertEqual(rx.tags("rwsrt.discontinuity"), [])
        self.assertEqual(r["next_sample_index"], n)
        self.assertEqual(s["run_id"], r.get("run_id", s["run_id"]))
        # One record tag per record, on its first sample, with its coordinates.
        per = PER_RECORD[fmt]
        tags = rx.tags("rwsrt.record")
        covered = 0
        for i, (offset, value) in enumerate(tags):
            self.assertEqual(offset, covered)
            self.assertEqual(value["first_sample_index"], covered)
            self.assertEqual(value["segment_id"], 0)
            self.assertEqual(value["record_sequence"], i)
            self.assertEqual(value["run_id"], s["run_id"])
            self.assertEqual((value["rate_num"], value["rate_den"]), (200000, 1))
            self.assertLessEqual(value["sample_count"], per)
            covered += value["sample_count"]
        self.assertEqual(covered, n)
        self.assertGreaterEqual(len(tags), math.ceil(n / per))
        for side in (s, r):
            self.assertFalse(side["running"])
            self.assertEqual(side["srt_runtime_leases"], 0)
            self.assertLess(side["last_stop_ms"], 1000.0)
            self.assertEqual(side["session"]["effective"]["payload_size"], 1200)
        no_secret(self, s, r)
        return s, r

    def test_both_formats_both_topologies(self):
        n = 60000 + 17  # ends in a partial record
        for fmt, sink_role, key in (("cf32", "caller", KEY_ENV), ("cf32", "listener", None),
                                    ("f32", "caller", None), ("f32", "listener", KEY_ENV)):
            data = test_signal(fmt, n)
            tx, rx = transfer(self, fmt, sink_role, key, data)
            s, r = self.check_complete(tx, rx, fmt, data)
            self.assertEqual(s["session"]["effective"]["km_state"], 2 if key else 0)
            self.assertEqual(r["session"]["effective"]["km_state"], 2 if key else 0)
            evidence("transfer", fmt=fmt, sink_role=sink_role, encrypted=bool(key), samples=n,
                     records=len(rx.tags("rwsrt.record")), sink_stop_ms=s["last_stop_ms"],
                     source_stop_ms=r["last_stop_ms"], rtt_ms=s["session"]["link"]["rtt_ms"])

    def test_float_bit_patterns_survive(self):
        patterns = [0x00000000, 0x80000000, 0x7f800000, 0xff800000, 0x00000001, 0x807fffff,
                    0x3f800000, 0x7f7fffff, 0x12345678]
        data = [struct.unpack("<f", struct.pack("<I", p))[0] for p in patterns] * 50
        tx, rx = transfer(self, "f32", "caller", None, data)
        got = rx.data()
        self.assertEqual([struct.pack("<f", v) for v in got], [struct.pack("<f", v) for v in data])

    def test_nan_payloads_survive(self):
        # NaN payload bits, checked through the complex path so that no
        # Python float conversion can quiet them.
        import numpy as np
        raw = np.array([0x7fc01234, 0x7fa00001, 0xffc00000, 0x7f800001] * 64, dtype="<u4")
        data = raw.view("<f4").astype(np.complex64)  # NaN real parts, zero imaginary
        data.real.view("<u4")[:] = raw
        from gnuradio import blocks, gr
        port = free_udp_port()
        ts, ss, tr, sr = configs(port, key=None)
        rx = Receiver(tr, sr, "cf32")
        tx_tb = gr.top_block()
        from qa_common import robotweax_srt
        sink = robotweax_srt.srt_sink(ts, ss)
        src = blocks.vector_source_c(data.tolist(), False)
        tx_tb.connect(src, blocks.throttle(gr.sizeof_gr_complex, 200000, False), sink)
        rx.tb.start()
        time.sleep(0.2)
        tx_tb.start()
        self.assertIsNotNone(wait_done(tx_tb, 20))
        self.assertIsNotNone(wait_done(rx.tb, 20))
        got = np.array(rx.data(), dtype=np.complex64)
        sent = np.array(data.tolist(), dtype=np.complex64)  # what the vector source held
        self.assertEqual(got.tobytes(), sent.tobytes())
        self.assertTrue(np.isnan(got.real).all())

    def test_empty_run_is_complete(self):
        tx, rx = transfer(self, "cf32", "caller", KEY_ENV, [])
        s, r = tx.status(), rx.status()
        self.assertEqual(rx.data(), [])
        self.assertTrue(s["natural_end"])
        self.assertEqual(s["records_assembled"], 1)  # END only
        self.assertEqual(r["session"]["end"], "complete")
        self.assertEqual(r["output_samples_emitted"], 0)
        self.assertEqual(rx.tags("rwsrt.record"), [])

    def test_partial_output_keeps_tags_at_exact_offsets(self):
        # 37 items per work() call: every 138-sample record is split across calls.
        n = 138 * 40 + 5
        data = test_signal("cf32", n)
        tx, rx = transfer(self, "cf32", "caller", None, data, max_noutput=37)
        self.check_complete(tx, rx, "cf32", data)
        offsets = [o for o, _ in rx.tags("rwsrt.record")]
        self.assertEqual(len(offsets), len(set(offsets)))  # each tag exactly once

    def test_strict_sink_started_first_loses_nothing(self):
        # No peer for 0.8 s: the strict Sink consumes nothing meanwhile.
        n = 30000
        data = test_signal("cf32", n)
        port = free_udp_port()
        ts, ss, tr, sr = configs(port, key=KEY_ENV)
        tx = Sender(ts, ss, "cf32", data)
        rx = Receiver(tr, sr, "cf32")
        tx.tb.start()
        time.sleep(0.8)
        before = tx.status()
        self.assertEqual(before["input_samples_consumed"], 0)
        self.assertIn(before["session"]["state"], ("connecting", "retry_wait"))
        rx.tb.start()
        self.assertIsNotNone(wait_done(tx.tb, 30))
        self.assertIsNotNone(wait_done(rx.tb, 30))
        s, r = self.check_complete(tx, rx, "cf32", data)
        evidence("strict_sink_first", attempts=s["session"]["counters"]["connect_attempts"])

    def test_small_scheduler_chunks_and_flush_interval(self):
        # A slow stream: 2000 S/s arriving 20 items at a time (10 ms apart),
        # so records are sealed by the 2 ms flush interval, not by filling up.
        n = 600
        data = test_signal("f32", n)
        port = free_udp_port()
        ts, ss, tr, sr = configs(port, fmt="f32", key=None, stream=dict(sample_rate_num=2000))
        rx = Receiver(tr, sr, "f32")
        tx = Sender(ts, ss, "f32", data, rate=2000, chunk=20)
        rx.tb.start()
        time.sleep(0.2)
        tx.tb.start()
        self.assertIsNotNone(wait_done(tx.tb, 30))
        self.assertIsNotNone(wait_done(rx.tb, 30))
        self.assertEqual(rx.data(), data)
        tags = rx.tags("rwsrt.record")
        self.assertGreater(len(tags), math.ceil(n / 276))  # more, smaller records
        self.assertEqual(sum(v["sample_count"] for _, v in tags), n)
        self.assertEqual(rx.status()["session"]["end"], "complete")
        evidence("flush_interval", records=len(tags), samples=n)

    def test_status_messages_are_published(self):
        n = 100000  # half a second, several 100 ms status intervals
        data = test_signal("cf32", n)
        tx, rx = transfer(self, "cf32", "caller", KEY_ENV, data)
        import pmt
        for side, direction in ((tx, "send"), (rx, "receive")):
            self.assertTrue(wait_for(lambda: side.msgs.num_messages() >= 3, 2.0))
            count = side.msgs.num_messages()
            msgs = [pmt.to_python(side.msgs.get_message(i)) for i in range(count)]
            snapshots = [m for m in msgs if "session" in m]
            self.assertGreaterEqual(len(snapshots), 3)
            self.assertEqual(snapshots[0]["direction"], direction)
            self.assertTrue(snapshots[0]["running"])
            self.assertTrue(any(m["session"]["state"] == "connected" for m in snapshots))
            no_secret(self, msgs)
            # The PMT dictionary and the JSON form describe the same thing.
            # (The snapshot published at stop() is not asserted here: message
            # subscribers inside a finishing flowgraph may already be gone.
            # status() after the run is the reliable final state.)
            self.assertEqual(set(snapshots[-1].keys()), set(side.status().keys()))

if __name__ == '__main__':
    gr_unittest.run(qa_transfer)
