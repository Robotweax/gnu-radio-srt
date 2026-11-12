#!/usr/bin/env python
#
# Copyright 2026 Robotweax.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
"""Start, stop, failure and reconnect behaviour of both blocks."""

import os
import time

from gnuradio import blocks, gr, gr_unittest

from qa_common import (KEY_ENV, OTHER_KEY_ENV, RATE, STOP_BOUND_MS, Receiver, Sender, configs,
                       evidence, free_udp_port, robotweax_srt, stop_and_time, test_signal,
                       thread_count, wait_done, wait_for)

SEG = "segmented_live"
STRICT = "strict_stop"


class qa_lifecycle(gr_unittest.TestCase):

    def test_user_stop_is_cancellation_not_completion(self):
        port = free_udp_port()
        ts, ss, tr, sr = configs(port, key=KEY_ENV)
        rx = Receiver(tr, sr, "cf32")
        tx = Sender(ts, ss, "cf32", None, endless=True)
        rx.tb.start()
        time.sleep(0.2)
        tx.tb.start()
        self.assertTrue(wait_for(lambda: rx.status()["output_samples_emitted"] > 20000, 10))
        stop_ms = stop_and_time(tx.tb)
        s = tx.status()
        self.assertLess(stop_ms, STOP_BOUND_MS)
        self.assertFalse(s["natural_end"])
        self.assertFalse(s["end_enqueued"])
        self.assertEqual(s["session"]["counters"]["end_sent"], 0)  # no END on cancel
        self.assertFalse(s["session"]["completed"])
        self.assertEqual(s["session"]["failure"], "none")  # cancellation is not a failure
        # The strict receiver ends by itself and does not report success.
        waited = wait_done(rx.tb, 15)
        self.assertIsNotNone(waited, "receiver did not end after the sender went away")
        r = rx.status()
        got = rx.data()
        self.assertGreater(len(got), 20000)
        self.assertEqual(got, test_like_null(len(got)))
        self.assertEqual(r["session"]["failure"], "connection_lost")
        self.assertTrue(r["session"]["termination_unknown"])
        self.assertFalse(r["end_emitted"])
        self.assertFalse(r["session"]["completed"])
        self.assertEqual(r["session"]["end"], "none")
        evidence("user_stop", sink_stop_ms=stop_ms, receiver_ended_after_s=waited,
                 received=len(got), consumed=s["input_samples_consumed"])

    def test_stop_with_no_peer_is_bounded(self):
        results = []
        for block in ("sink", "source"):
            for role in ("caller", "listener"):
                for policy in (STRICT, SEG):
                    port = free_udp_port()
                    ts, ss, tr, sr = configs(port, sink_role=role, policy=policy)
                    if block == "sink":
                        side = Sender(ts, ss, "cf32", None, endless=True)
                    else:
                        t = robotweax_srt.make_transport_config(role=role, host="127.0.0.1", port=port,
                                                                passphrase_env=KEY_ENV)
                        side = Receiver(t, sr, "cf32")
                    side.tb.start()
                    time.sleep(0.3)
                    running = side.status()
                    self.assertTrue(running["running"])
                    self.assertEqual(running["srt_runtime_leases"], 1)
                    ms = stop_and_time(side.tb)
                    st = side.status()
                    results.append((block, role, policy, round(ms, 1)))
                    self.assertLess(ms, STOP_BOUND_MS, (block, role, policy))
                    self.assertFalse(st["running"])
                    self.assertEqual(st["srt_runtime_leases"], 0)
                    self.assertEqual(st["session"]["counters"]["connections"], 0)
                    self.assertLess(st["session"]["counters"]["max_wait_slice_ms"], 60.0)
                    if block == "sink" and policy == STRICT:
                        self.assertEqual(st["input_samples_consumed"], 0)
        evidence("stop_no_peer", worst_ms=max(r[3] for r in results), cases=results)

    def test_no_peer_ends_at_the_startup_deadline(self):
        for block in ("sink", "source"):
            port = free_udp_port()
            ts, ss, tr, sr = configs(port, transport=dict(startup_timeout_ms=1500, connect_timeout_ms=500))
            side = Sender(ts, ss, "cf32", None, endless=True) if block == "sink" else Receiver(tr, sr, "cf32")
            side.tb.start()
            waited = wait_done(side.tb, 10)
            self.assertIsNotNone(waited, block + " did not end by itself")
            st = side.status()
            self.assertEqual(st["session"]["failure"], "startup_timeout")
            self.assertFalse(st["session"]["completed"])
            self.assertGreater(waited, 1.4)
            self.assertLess(waited, 3.0)
            evidence("startup_deadline", block=block, ended_after_s=waited,
                     attempts=st["session"]["counters"]["connect_attempts"])

    def test_wrong_key_ends_both_sides_without_data(self):
        port = free_udp_port()
        ts, ss, tr, sr = configs(port, sink_key=OTHER_KEY_ENV, source_key=KEY_ENV,
                                 transport=dict(startup_timeout_ms=2500))
        rx = Receiver(tr, sr, "cf32")
        tx = Sender(ts, ss, "cf32", test_signal("cf32", 20000))
        rx.tb.start()
        time.sleep(0.2)
        tx.tb.start()
        tx_wait = wait_done(tx.tb, 10)
        rx_wait = wait_done(rx.tb, 10)
        self.assertIsNotNone(tx_wait)
        self.assertIsNotNone(rx_wait)
        s, r = tx.status(), rx.status()
        self.assertEqual(s["session"]["failure"], "credentials")
        self.assertLess(tx_wait, 2.0)  # terminal, not retried until a timeout
        self.assertEqual(s["input_samples_consumed"], 0)
        self.assertEqual(rx.data(), [])
        self.assertEqual(r["session"]["counters"]["records_received"], 0)
        self.assertEqual(r["session"]["failure"], "startup_timeout")
        for secret in (os.environ[KEY_ENV], os.environ[OTHER_KEY_ENV]):
            self.assertNotIn(secret, tx.sink.status_json() + rx.source.status_json())
        evidence("wrong_key", sender_ended_after_s=tx_wait, detail=s["session"]["failure_detail"])

    def test_encrypted_sender_is_not_downgraded_by_a_plaintext_receiver(self):
        port = free_udp_port()
        ts, ss, tr, sr = configs(port, sink_key=KEY_ENV, source_key=None,
                                 transport=dict(startup_timeout_ms=2500))
        rx = Receiver(tr, sr, "cf32")
        tx = Sender(ts, ss, "cf32", test_signal("cf32", 20000))
        rx.tb.start()
        time.sleep(0.2)
        tx.tb.start()
        self.assertIsNotNone(wait_done(tx.tb, 10))
        self.assertIsNotNone(wait_done(rx.tb, 10))
        self.assertEqual(tx.status()["session"]["failure"], "credentials")
        self.assertEqual(rx.data(), [])

    def test_format_and_rate_mismatch_are_terminal_on_the_source(self):
        for stream in (dict(sample_rate_num=48000), None):
            port = free_udp_port()
            fmt_rx = "cf32" if stream else "f32"
            ts, ss, _, _ = configs(port, fmt="cf32", key=None, policy=SEG,
                                   transport=dict(reconnect=False))
            _, _, tr, sr = configs(port, fmt=fmt_rx, key=None, source_stream=stream)
            rx = Receiver(tr, sr, fmt_rx)
            tx = Sender(ts, ss, "cf32", test_signal("cf32", 100000))
            rx.tb.start()
            time.sleep(0.2)
            tx.tb.start()
            self.assertIsNotNone(wait_done(rx.tb, 10))
            r = rx.status()
            self.assertEqual(r["session"]["failure"], "configuration")
            self.assertEqual(rx.data(), [])  # never reinterpreted
            self.assertIn("differs from configuration", r["session"]["failure_detail"])
            self.assertIsNotNone(wait_done(tx.tb, 15))

    def test_restart_creates_a_new_run(self):
        port = free_udp_port()
        ts, ss, tr, sr = configs(port, key=None, policy=SEG)
        tx = Sender(ts, ss, "cf32", None, endless=True)
        runs = []
        for _ in range(3):
            tx.tb.start()
            time.sleep(0.1)
            runs.append(tx.status()["run_id"])
            self.assertLess(stop_and_time(tx.tb), STOP_BOUND_MS)
        self.assertEqual(len(set(runs)), 3)
        self.assertTrue(all(len(r) == 32 for r in runs))

    def test_start_stop_cycles_do_not_leak(self):
        cycles = 100
        port = free_udp_port()
        ts, ss, tr, sr = configs(port, key=KEY_ENV, policy=SEG)
        tx = Sender(ts, ss, "cf32", None, endless=True)
        rx = Receiver(tr, sr, "cf32")

        def cycle():
            rx.tb.start()
            tx.tb.start()
            time.sleep(0.15)
            a = stop_and_time(tx.tb)
            b = stop_and_time(rx.tb)
            return max(a, b)

        cycle()  # warm-up
        threads, fds = thread_count(), len(os.listdir("/proc/self/fd"))
        worst = max(cycle() for _ in range(cycles))
        settle_ms = 0
        while thread_count() != threads and settle_ms < 1000:
            time.sleep(0.01)
            settle_ms += 10
        evidence("start_stop_cycles", cycles=cycles, worst_stop_ms=worst, threads_before=threads,
                 threads_after=thread_count(), thread_settle_ms=settle_ms, fds_before=fds,
                 fds_after=len(os.listdir("/proc/self/fd")),
                 connections=rx.status()["session"]["counters"]["connections"])
        self.assertLess(worst, STOP_BOUND_MS)
        self.assertEqual(thread_count(), threads)
        self.assertEqual(len(os.listdir("/proc/self/fd")), fds)
        self.assertEqual(tx.status()["srt_runtime_leases"], 0)

    def test_late_receiver_gets_an_initial_join_tag(self):
        n = 200000  # one second
        data = test_signal("cf32", n)
        port = free_udp_port()
        ts, ss, tr, sr = configs(port, key=None, policy=SEG, sink_role="listener")
        tx = Sender(ts, ss, "cf32", data)
        rx = Receiver(tr, sr, "cf32")
        tx.tb.start()
        time.sleep(0.5)  # the segmented Sink consumes and discards meanwhile
        rx.tb.start()
        self.assertIsNotNone(wait_done(tx.tb, 30))
        self.assertIsNotNone(wait_done(rx.tb, 30))
        got = rx.data()
        breaks = rx.tags("rwsrt.discontinuity")
        self.assertGreaterEqual(len(breaks), 1)
        offset, brk = breaks[0]
        self.assertEqual(offset, 0)  # on the very first output sample
        self.assertIn("initial_join", brk["reasons"])
        self.assertFalse(brk["has_previous"])
        self.assertNotIn("previous", brk)
        self.assertFalse(brk["missing_known"])
        first = dict(rx.tags("rwsrt.record"))[0]["first_sample_index"]
        self.assertGreater(first, 0)  # does not claim to have seen the start
        s, r = tx.status(), rx.status()
        self.assertEqual(s["input_samples_consumed"], n)
        self.assertGreater(s["tx_queue"]["dropped_overflow_samples"] + s["tx_queue"]["dropped_expired_samples"], 0)
        self.assertEqual(r["session"]["end"], "incomplete")  # END seen, start not
        # Every output sample is the right sample for its coordinate.
        pos = 0
        for off, rec in rx.tags("rwsrt.record"):
            c, i = rec["sample_count"], rec["first_sample_index"]
            self.assertEqual(got[off:off + c], data[i:i + c])
            pos = off + c
        self.assertEqual(pos, len(got))
        evidence("late_receiver", first_index=first, received=len(got),
                 sender_dropped=s["tx_queue"]["dropped_overflow_samples"] + s["tx_queue"]["dropped_expired_samples"])

    def test_sender_restart_is_a_new_run_for_a_segmented_source(self):
        port = free_udp_port()
        ts, ss, tr, sr = configs(port, key=None, policy=SEG,
                                 transport=dict(reconnect_total_timeout_ms=8000))
        rx = Receiver(tr, sr, "cf32")
        rx.tb.start()
        time.sleep(0.2)
        tx1 = Sender(ts, ss, "cf32", None, endless=True)
        tx1.tb.start()
        self.assertTrue(wait_for(lambda: rx.status()["output_samples_emitted"] > 20000, 10))
        stop_and_time(tx1.tb)  # cancelled: no END
        self.assertTrue(wait_for(lambda: rx.status()["session"]["state"] in ("listening", "retry_wait"), 10))
        mid = rx.status()
        self.assertTrue(mid["session"]["termination_unknown"])
        data = test_signal("cf32", 30000)
        tx2 = Sender(ts, ss, "cf32", data)
        tx2.tb.start()
        self.assertIsNotNone(wait_done(tx2.tb, 20))
        self.assertIsNotNone(wait_done(rx.tb, 20))
        r = rx.status()
        breaks = rx.tags("rwsrt.discontinuity")
        new_run = [(o, b) for o, b in breaks if "new_run" in b["reasons"]]
        self.assertEqual(len(new_run), 1)
        offset, brk = new_run[0]
        self.assertIn("reconnect", brk["reasons"])
        self.assertFalse(brk["missing_known"])
        self.assertNotEqual(brk["previous"]["run_id"], brk["next"]["run_id"])
        self.assertEqual(brk["connection_generation"], 2)
        got = rx.data()
        # Samples before the tag are run 1, from the tag on run 2: never merged silently.
        self.assertEqual(got[offset:], data[brk["next"]["sample_index"]:])
        self.assertEqual(r["session"]["continuity"]["runs"], 2)
        self.assertEqual(r["session"]["counters"]["connections"], 2)
        self.assertEqual(r["run_id"], tx2.status()["run_id"])
        self.assertFalse(r["session"]["termination_unknown"])  # the second run ended with END
        self.assertTrue(r["end_emitted"])
        evidence("new_run", tag_offset=offset, reasons=brk["reasons"], end=r["session"]["end"])

    def test_slow_consumer_segmented_reports_every_dropped_sample(self):
        n = 300000  # 1.5 s of stream into a consumer running at a quarter of the rate
        data = test_signal("cf32", n)
        port = free_udp_port()
        ts, ss, tr, sr = configs(port, key=None, sink_policy=STRICT, source_policy=SEG)
        rx = Receiver(tr, sr, "cf32", slow_rate=RATE // 4)
        tx = Sender(ts, ss, "cf32", data)
        rx.tb.start()
        time.sleep(0.2)
        tx.tb.start()
        self.assertIsNotNone(wait_done(tx.tb, 30))
        self.assertIsNotNone(wait_done(rx.tb, 60))
        r = rx.status()
        got = rx.data()
        q = r["rx_queue"]
        dropped = q["dropped_overflow_samples"] + q["dropped_expired_samples"]
        self.assertGreater(dropped, 0)
        breaks = rx.tags("rwsrt.discontinuity")
        tagged = sum(b["missing_samples"] for _, b in breaks)
        self.assertTrue(all("receiver_queue_drop" in b["reasons"] and b["missing_known"] for _, b in breaks))
        # Queue-drop metrics and discontinuity reports agree, and nothing is unaccounted.
        self.assertEqual(r["reported_missing_samples"], dropped)
        self.assertEqual(len(got) + dropped, n)
        self.assertEqual(tagged + (r["reported_missing_samples"] - tagged), dropped)
        self.assertEqual(r["receiver_queue_drop_breaks"], r["discontinuity_tags"] + r["untagged_discontinuities"])
        # What was output is correct for its coordinates, and not stale backlog.
        for off, rec in rx.tags("rwsrt.record"):
            c, i = rec["sample_count"], rec["first_sample_index"]
            self.assertEqual(got[off:off + c], data[i:i + c])
        self.assertLessEqual(q["high_water_records"], 8192)
        self.assertTrue(r["end_emitted"])
        self.assertEqual(r["session"]["end"], "complete")  # the network delivered everything
        evidence("slow_consumer_segmented", received=len(got), dropped=dropped, tags=len(breaks),
                 untagged=r["untagged_discontinuities"])

    def test_slow_consumer_strict_is_a_terminal_failure(self):
        n = 300000
        port = free_udp_port()
        ts, ss, tr, sr = configs(port, key=None, policy=STRICT)
        rx = Receiver(tr, sr, "cf32", slow_rate=RATE // 4)
        tx = Sender(ts, ss, "cf32", test_signal("cf32", n))
        rx.tb.start()
        time.sleep(0.2)
        tx.tb.start()
        self.assertIsNotNone(wait_done(rx.tb, 30))
        r = rx.status()
        self.assertEqual(r["session"]["failure"], "queue_overflow")
        self.assertFalse(r["session"]["completed"])
        self.assertLess(len(rx.data()), n)
        self.assertEqual(rx.tags("rwsrt.discontinuity"), [])  # strict never passes a break
        self.assertIsNotNone(wait_done(tx.tb, 30))


def test_like_null(n):
    """blocks.null_source produces zeros."""
    return [0j] * n


if __name__ == '__main__':
    gr_unittest.run(qa_lifecycle)
