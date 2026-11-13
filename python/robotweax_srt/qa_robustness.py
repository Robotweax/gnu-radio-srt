#!/usr/bin/env python
#
# Copyright 2026 Robotweax.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
"""Behaviour of both blocks under scripted faults (plan section 12 B).

Faults come from tools/impair/udp_proxy.py: outages, a vanished peer,
dropped records and seeded random loss. They are functional checks on
loopback, not network qualification and not performance results.
"""

import time

import numpy
from gnuradio import blocks, gr, gr_unittest

from qa_common import (RATE, Proxy, Receiver, Sender, check_coordinates, configs, evidence,
                       free_udp_port, proxied_configs, robotweax_srt, stop_and_time, test_signal,
                       wait_done, wait_for)

SEG = "segmented_live"
STRICT = "strict_stop"
# Short timers so that faults play out in seconds.
FAST = dict(peer_idle_timeout_ms=1000, connect_timeout_ms=500, reconnect_initial_ms=100,
            reconnect_max_ms=400, reconnect_total_timeout_ms=15000, startup_timeout_ms=8000)


def start_pair(rx, tx, gap=0.2):
    rx.tb.start()
    time.sleep(gap)
    tx.tb.start()


class paused_source(gr.sync_block):
    """Emits `before` samples, pauses, then emits the rest: a stalled sender."""

    def __init__(self, data, before, pause_s):
        gr.sync_block.__init__(self, "paused_source", None, [numpy.complex64])
        self.data = numpy.array(data, dtype=numpy.complex64)
        self.pos = 0
        self.before = before
        self.pause_s = pause_s
        self.paused = False

    def work(self, input_items, output_items):
        out = output_items[0]
        if self.pos == self.before and not self.paused:
            self.paused = True
            time.sleep(self.pause_s)
        limit = self.before if self.pos < self.before else len(self.data)
        n = min(len(out), limit - self.pos, 2000)
        if n == 0:
            return -1 if self.pos >= len(self.data) else 0
        out[:n] = self.data[self.pos:self.pos + n]
        self.pos += n
        time.sleep(n / float(RATE))  # paced
        return n


class qa_robustness(gr_unittest.TestCase):

    def accounted(self, tx, rx, n):
        """Segmented mode: every sample is delivered or reported missing."""
        r = rx.status()
        first = rx.tags("rwsrt.record")[0][1]["first_sample_index"]
        self.assertEqual(r["unknown_breaks"] - (1 if first > 0 else 0), 0)
        self.assertEqual(first + r["output_samples_emitted"] + r["reported_missing_samples"], n)
        return r

    # ---------------------------------------------------------------- outages

    def test_short_outage_segmented_gaps_without_reconnect(self):
        """Outage shorter than the idle timeout: the connection survives, SRT
        gives up on what became too late, and the gap is reported exactly."""
        n = 3 * RATE
        data = test_signal("cf32", n)
        proxy, ts, ss, tr, sr = proxied_configs("--outage", "1.0:0.8", key=None, policy=SEG)
        rx, tx = Receiver(tr, sr, "cf32"), Sender(ts, ss, "cf32", data)
        start_pair(rx, tx)
        self.assertIsNotNone(wait_done(tx.tb, 30))
        self.assertIsNotNone(wait_done(rx.tb, 30))
        counters = proxy.close()
        r = self.accounted(tx, rx, n)
        got = check_coordinates(self, rx, data)
        breaks = rx.tags("rwsrt.discontinuity")
        self.assertGreater(counters["dropped_outage"], 0)
        self.assertGreater(r["reported_missing_samples"], 0)
        self.assertEqual(r["session"]["counters"]["connections"], 1)  # no reconnect
        self.assertTrue(all("reconnect" not in b["reasons"] and b["missing_known"] for _, b in breaks))
        self.assertTrue(r["end_emitted"])
        self.assertEqual(r["session"]["end"], "incomplete")
        self.assertEqual(got[-1], data[-1])  # output resumed after the outage
        evidence("short_outage_segmented", missing=r["reported_missing_samples"], breaks=len(breaks),
                 proxy_dropped=counters["dropped_outage"],
                 sender_srt_dropped=tx.status()["session"]["link"]["send_dropped"])

    def test_long_outage_segmented_reconnects_within_one_run(self):
        """Outage longer than the idle timeout: both ends reconnect, the run
        and its coordinates continue, and the boundary is tagged."""
        n = 6 * RATE
        data = test_signal("cf32", n)
        proxy, ts, ss, tr, sr = proxied_configs("--outage", "1.0:2.5", key=None, policy=SEG, transport=FAST)
        rx, tx = Receiver(tr, sr, "cf32"), Sender(ts, ss, "cf32", data)
        start_pair(rx, tx)
        self.assertIsNotNone(wait_done(tx.tb, 40))
        self.assertIsNotNone(wait_done(rx.tb, 40))
        proxy.close()
        r = self.accounted(tx, rx, n)
        s = tx.status()
        check_coordinates(self, rx, data)
        breaks = rx.tags("rwsrt.discontinuity")
        reconnects = [b for _, b in breaks if "reconnect" in b["reasons"]]
        self.assertEqual(len(reconnects), 1)
        b = reconnects[0]
        self.assertEqual(b["connection_generation"], 2)
        self.assertEqual(b["previous"]["run_id"], b["next"]["run_id"])  # same run
        self.assertNotIn("new_run", b["reasons"])
        self.assertTrue(b["missing_known"])
        self.assertEqual(b["missing_samples"], b["next"]["sample_index"] - b["previous"]["sample_index"])
        self.assertEqual(r["session"]["counters"]["connections"], 2)
        self.assertEqual(s["session"]["counters"]["connections"], 2)
        self.assertEqual(r["session"]["continuity"]["runs"], 1)
        self.assertEqual(s["session"]["failure"], "none")
        self.assertTrue(s["session"]["completed"])
        self.assertTrue(r["end_emitted"])
        # The sender did not keep a backlog through the outage.
        q = s["tx_queue"]
        self.assertGreater(q["dropped_overflow_samples"] + q["dropped_expired_samples"], RATE)
        self.assertLessEqual(q["high_water_records"], 8192)
        evidence("long_outage_reconnect", missing=r["reported_missing_samples"],
                 reconnect_gap=b["missing_samples"], breaks=len(breaks),
                 sender_dropped=q["dropped_overflow_samples"] + q["dropped_expired_samples"],
                 sender_attempts=s["session"]["counters"]["connect_attempts"])

    def test_repeated_reconnects(self):
        n = 13 * RATE
        data = test_signal("cf32", n)
        proxy, ts, ss, tr, sr = proxied_configs("--outage", "1.0:2.0", "--outage", "5.0:2.0", "--outage",
                                                "9.0:2.0", key=None, policy=SEG, transport=FAST)
        rx, tx = Receiver(tr, sr, "cf32"), Sender(ts, ss, "cf32", data)
        start_pair(rx, tx)
        self.assertIsNotNone(wait_done(tx.tb, 60))
        self.assertIsNotNone(wait_done(rx.tb, 60))
        proxy.close()
        r = self.accounted(tx, rx, n)
        check_coordinates(self, rx, data)
        reconnects = [b for _, b in rx.tags("rwsrt.discontinuity") if "reconnect" in b["reasons"]]
        self.assertEqual(len(reconnects), 3)
        self.assertEqual([b["connection_generation"] for b in reconnects], [2, 3, 4])
        self.assertEqual(r["session"]["counters"]["connections"], 4)
        self.assertEqual(r["session"]["continuity"]["runs"], 1)
        self.assertTrue(r["end_emitted"])
        self.assertEqual(r["srt_runtime_leases"], 0)
        evidence("repeated_reconnects", missing=r["reported_missing_samples"],
                 received=r["output_samples_emitted"])

    def test_outage_resumes_with_default_timers(self):
        """Plan profile N5 in miniature: a 5 s outage with default settings.
        Output must resume within 10 s of the path coming back."""
        n = 17 * RATE
        data = test_signal("cf32", n)
        proxy, ts, ss, tr, sr = proxied_configs("--outage", "1.0:5.0", key=None, policy=SEG)
        rx, tx = Receiver(tr, sr, "cf32"), Sender(ts, ss, "cf32", data)
        start_pair(rx, tx)
        t0 = time.monotonic()  # the proxy clock starts with the first packet, about now
        self.assertTrue(wait_for(lambda: rx.status()["output_samples_emitted"] > RATE // 2, 10))
        before = None
        resumed_at = None
        while time.monotonic() - t0 < 16.5:
            emitted = rx.status()["output_samples_emitted"]
            t = time.monotonic() - t0
            if 3.0 < t < 5.5:
                before = emitted  # in the middle of the outage
            if before is not None and resumed_at is None and t > 6.0 and emitted > before:
                resumed_at = t
            time.sleep(0.05)
        self.assertIsNotNone(wait_done(tx.tb, 40))
        self.assertIsNotNone(wait_done(rx.tb, 40))
        proxy.close()
        self.assertIsNotNone(resumed_at, "output never resumed")
        resume_delay = resumed_at - 6.0  # the path is back 6.0 s after the first packet
        r = self.accounted(tx, rx, n)
        check_coordinates(self, rx, data)
        self.assertLess(resume_delay, 10.0)
        evidence("outage_default_timers", resume_delay_s=resume_delay,
                 connections=r["session"]["counters"]["connections"],
                 missing=r["reported_missing_samples"])

    def test_outage_in_strict_mode_fails_both_ends(self):
        n = 4 * RATE
        data = test_signal("cf32", n)
        proxy, ts, ss, tr, sr = proxied_configs("--outage", "1.0:0.8", key=None, policy=STRICT)
        rx, tx = Receiver(tr, sr, "cf32"), Sender(ts, ss, "cf32", data)
        start_pair(rx, tx)
        self.assertIsNotNone(wait_done(tx.tb, 30))
        self.assertIsNotNone(wait_done(rx.tb, 30))
        proxy.close()
        s, r = tx.status(), rx.status()
        got = rx.data()
        # No gap ever reached the output: what was emitted is an exact prefix.
        self.assertEqual(got, data[:len(got)])
        self.assertLess(len(got), n)
        self.assertEqual(rx.tags("rwsrt.discontinuity"), [])
        self.assertIn(r["session"]["failure"], ("continuity", "connection_lost"))
        self.assertIn(s["session"]["failure"], ("continuity", "connection_lost"))
        for side in (s, r):
            self.assertFalse(side["session"]["completed"])
        self.assertFalse(r["end_emitted"])
        self.assertEqual(s["session"]["counters"]["end_sent"], 0)
        evidence("outage_strict", sink_failure=s["session"]["failure"],
                 sink_detail=s["session"]["failure_detail"], source_failure=r["session"]["failure"],
                 source_detail=r["session"]["failure_detail"], emitted=len(got),
                 consumed=s["input_samples_consumed"])

    # ------------------------------------------------------- lost END / DATA

    def test_lost_end_is_never_reported_as_completion(self):
        n = RATE
        data = test_signal("cf32", n)
        results = {}
        for policy in (STRICT, SEG):
            proxy, ts, ss, tr, sr = proxied_configs("--drop-end", key=None, sink_policy=STRICT,
                                                    source_policy=policy,
                                                    transport=dict(FAST, reconnect_total_timeout_ms=2000))
            rx, tx = Receiver(tr, sr, "cf32"), Sender(ts, ss, "cf32", data)
            start_pair(rx, tx)
            self.assertIsNotNone(wait_done(tx.tb, 30))
            self.assertIsNotNone(wait_done(rx.tb, 30))
            counters = proxy.close()
            s, r = tx.status(), rx.status()
            self.assertGreaterEqual(counters["dropped_end"], 1)
            self.assertEqual(rx.data(), data)  # every sample arrived ...
            self.assertFalse(r["end_emitted"])  # ... but the end did not
            self.assertEqual(r["session"]["end"], "none")
            self.assertTrue(r["session"]["termination_unknown"])
            self.assertFalse(r["session"]["completed"])
            self.assertEqual(r["session"]["failure"],
                             "connection_lost" if policy == STRICT else "reconnect_timeout")
            # The sender handed END to SRT and can say no more than that.
            self.assertEqual(s["session"]["counters"]["end_sent"], 1)
            self.assertTrue(s["session"]["completed"])
            self.assertFalse(s["session"]["end_acknowledged_by_transport"])
            self.assertLess(s["last_stop_ms"], 1000.0)  # bounded wait for the lost END
            results[policy] = dict(sink_stop_ms=s["last_stop_ms"], end_packets_dropped=counters["dropped_end"])
        evidence("lost_end", **results)

    def test_end_acknowledged_by_transport_on_a_clean_run(self):
        data = test_signal("cf32", 20000)
        port = free_udp_port()
        ts, ss, tr, sr = configs(port, key=None)
        rx, tx = Receiver(tr, sr, "cf32"), Sender(ts, ss, "cf32", data)
        start_pair(rx, tx)
        self.assertIsNotNone(wait_done(tx.tb, 30))
        self.assertIsNotNone(wait_done(rx.tb, 30))
        self.assertTrue(tx.status()["session"]["end_acknowledged_by_transport"])
        self.assertEqual(rx.status()["session"]["end"], "complete")

    def test_lost_final_record_is_revealed_by_end(self):
        n = 100000 + 50  # final record: 50 samples... or less, if the flush split it
        data = test_signal("cf32", n)
        for policy in (SEG, STRICT):
            proxy, ts, ss, tr, sr = proxied_configs("--drop-sample", n - 1, key=None, sink_policy=STRICT,
                                                    source_policy=policy, transport=FAST)
            rx, tx = Receiver(tr, sr, "cf32"), Sender(ts, ss, "cf32", data)
            start_pair(rx, tx)
            self.assertIsNotNone(wait_done(tx.tb, 30))
            self.assertIsNotNone(wait_done(rx.tb, 30))
            counters = proxy.close()
            r = rx.status()
            got = rx.data()
            self.assertGreaterEqual(counters["dropped_sample"], 1)
            self.assertEqual(got, data[:len(got)])
            missing = n - len(got)
            self.assertGreater(missing, 0)
            self.assertLessEqual(missing, 138)
            self.assertFalse(r["session"]["completed"] and r["session"]["end"] == "complete")
            if policy == SEG:
                # END arrived and exposed the missing tail. No sample follows,
                # so there is no tag: the break is counted and published.
                self.assertTrue(r["end_emitted"])
                self.assertEqual(r["session"]["end"], "incomplete")
                self.assertEqual(r["untagged_discontinuities"], 1)
                self.assertEqual(r["reported_missing_samples"], missing)
                self.assertEqual(rx.tags("rwsrt.discontinuity"), [])
                self.assertEqual(len(got), r["output_samples_emitted"])  # nothing invented
            else:
                self.assertEqual(r["session"]["failure"], "continuity")
                self.assertFalse(r["end_emitted"])
            evidence("lost_final_record", policy=policy, missing=missing,
                     packets_dropped=counters["dropped_sample"])

    # ------------------------------------------- late delivery, peer notice

    def test_lateness_bound_discards_what_a_stalling_path_delivers_late(self):
        """A path that holds packets and then delivers them all loses
        nothing, so SRT hands over samples that are far too old. With
        max_lateness_ms the Source discards them and tags the gap."""
        n = 4 * RATE
        data = test_signal("cf32", n)
        results = {}
        for bound in (0, 100):
            proxy, ts, ss, tr, sr = proxied_configs("--stall", "1.0:0.8", key=None, policy=SEG,
                                                    transport=dict(max_lateness_ms=bound))
            rx, tx = Receiver(tr, sr, "cf32"), Sender(ts, ss, "cf32", data)
            start_pair(rx, tx)
            self.assertIsNotNone(wait_done(tx.tb, 30))
            self.assertIsNotNone(wait_done(rx.tb, 30))
            counters = proxy.close()
            self.assertGreater(counters["stalled"], 100)
            r = self.accounted(tx, rx, n)
            c = r["session"]["counters"]
            self.assertEqual(r["config"]["max_lateness_ms"], bound)
            self.assertGreater(c["max_lateness_seen_ms"], 300.0)
            self.assertEqual(r["session"]["failure"], "none")
            if bound == 0:
                self.assertEqual(c["late_samples_dropped"], 0)
                self.assertEqual(rx.data(), data)  # everything, some of it very late
            else:
                dropped = c["late_samples_dropped"]
                self.assertGreater(dropped, 0.3 * RATE)
                self.assertLess(dropped, 0.9 * RATE)
                self.assertEqual(r["reported_missing_samples"], dropped)
                gaps = [t for t in rx.tags("rwsrt.discontinuity") if t[1].get("missing_known")]
                self.assertEqual(len(gaps), 1)  # one contiguous stretch was late
                self.assertEqual(gaps[0][1]["missing_samples"], dropped)
                got = rx.data()
                at = gaps[0][0]
                self.assertEqual(got[:at], data[:at])
                self.assertEqual(got[at:], data[at + dropped:])  # nothing invented, nothing shifted
            results["bound_%d" % bound] = dict(late_samples_dropped=c["late_samples_dropped"],
                                               max_lateness_seen_ms=c["max_lateness_seen_ms"])
        evidence("lateness_bound", **results)

    def test_sink_is_told_when_a_strict_source_ends_the_run(self):
        """A strict Source that sees a loss ends the run. The Sink used to
        see only a closed connection; now the Source says why."""
        n = 4 * RATE
        data = test_signal("cf32", n)
        for sink_policy in (STRICT, SEG):
            proxy, ts, ss, tr, sr = proxied_configs("--drop-sample", RATE // 2, key=None,
                                                    sink_policy=sink_policy, source_policy=STRICT,
                                                    transport=FAST)
            rx, tx = Receiver(tr, sr, "cf32"), Sender(ts, ss, "cf32", data)
            start_pair(rx, tx)
            self.assertIsNotNone(wait_done(rx.tb, 30))
            self.assertIsNotNone(wait_done(tx.tb, 30))
            proxy.close()
            s, r = tx.status(), rx.status()
            self.assertEqual(r["session"]["failure"], "continuity")
            self.assertEqual(r["session"]["counters"]["peer_notices_sent"], 1)
            self.assertEqual(s["session"]["failure"], "continuity")
            self.assertTrue(s["session"]["failure_detail"].startswith("reported by the peer: "),
                            s["session"]["failure_detail"])
            self.assertEqual(s["session"]["counters"]["peer_notices_received"], 1)
            self.assertFalse(s["session"]["completed"])
            self.assertEqual(s["session"]["counters"]["end_sent"], 0)
            self.assertLess(s["input_samples_consumed"], n)  # it stopped; it did not send on

    # ---------------------------------------------------------- peer vanishes

    def test_vanished_peer_is_detected_within_the_idle_timeout(self):
        results = {}
        for policy in (STRICT, SEG):
            proxy, ts, ss, tr, sr = proxied_configs("--blackhole-after", "1.0", key=None, policy=policy,
                                                    transport=dict(FAST, reconnect_total_timeout_ms=2500))
            rx = Receiver(tr, sr, "cf32")
            tx = Sender(ts, ss, "cf32", None, endless=True)
            start_pair(rx, tx)
            t0 = time.monotonic()
            if policy == STRICT:
                tx_wait = wait_done(tx.tb, 15)  # ends by itself
                self.assertIsNotNone(tx_wait)
            else:
                # Segmented: keeps trying within its reconnect budget, then fails.
                self.assertTrue(wait_for(lambda: tx.status()["session"]["counters"]["connection_losses"] >= 1, 10))
                detected = time.monotonic() - t0
                tx_wait = wait_done(tx.tb, 15)
                self.assertIsNotNone(tx_wait)
            self.assertIsNotNone(wait_done(rx.tb, 15))
            proxy.close()
            s, r = tx.status(), rx.status()
            self.assertFalse(s["session"]["completed"])
            self.assertFalse(r["session"]["completed"])
            self.assertTrue(r["session"]["termination_unknown"])
            if policy == STRICT:
                self.assertIn(s["session"]["failure"], ("connection_lost", "continuity"))
                self.assertEqual(r["session"]["failure"], "connection_lost")
                # 1 s until the blackhole, 1 s idle timeout, plus margin.
                self.assertLess(tx_wait, 4.0)
                results[policy] = dict(sink_ended_after_s=tx_wait, sink_failure=s["session"]["failure"])
            else:
                self.assertEqual(s["session"]["failure"], "reconnect_timeout")
                self.assertEqual(r["session"]["failure"], "reconnect_timeout")
                self.assertLess(detected, 4.0)
                self.assertGreater(s["session"]["counters"]["connect_attempts"], 2)
                results[policy] = dict(loss_detected_after_s=detected,
                                       attempts=s["session"]["counters"]["connect_attempts"])
            self.assertLess(s["tx_queue"]["high_water_records"], 8193)
        evidence("vanished_peer", **results)

    # ------------------------------------------------------------ random loss

    def test_recoverable_random_loss_is_invisible_in_strict_mode(self):
        """2 % independent loss in both directions, seeded. SRT retransmits
        within the latency budget, so a strict run still completes exactly.
        A functional check on loopback, not a loss-recovery result."""
        n = 2 * RATE
        data = test_signal("cf32", n)
        proxy, ts, ss, tr, sr = proxied_configs("--loss", "0.02", "--seed", "7", policy=STRICT)
        rx, tx = Receiver(tr, sr, "cf32"), Sender(ts, ss, "cf32", data)
        start_pair(rx, tx)
        self.assertIsNotNone(wait_done(tx.tb, 30))
        self.assertIsNotNone(wait_done(rx.tb, 30))
        counters = proxy.close()
        s, r = tx.status(), rx.status()
        self.assertGreater(counters["dropped_loss"], 20)
        evidence("random_loss_strict", proxy_dropped=counters["dropped_loss"],
                 retransmitted=s["session"]["link"]["retransmitted"],
                 sender_failure=s["session"]["failure"], receiver_failure=r["session"]["failure"],
                 receiver_end=r["session"]["end"], received=len(rx.data()))
        self.assertEqual(rx.data(), data)
        self.assertEqual(r["session"]["end"], "complete")
        self.assertEqual(r["discontinuity_tags"], 0)
        self.assertGreater(s["session"]["link"]["retransmitted"], 0)
        self.assertEqual(s["session"]["link"]["send_dropped"], 0)

    # ------------------------------------------------------ several instances

    def test_two_pairs_run_and_stop_independently(self):
        n = 2 * RATE
        data = test_signal("cf32", n)
        pa, pb = free_udp_port(), free_udp_port()
        tsa, ssa, tra, sra = configs(pa, key=None)
        tsb, ssb, trb, srb = configs(pb, fmt="f32", sink_role="listener")
        rxa, txa = Receiver(tra, sra, "cf32"), Sender(tsa, ssa, "cf32", data)
        rxb, txb = Receiver(trb, srb, "f32"), Sender(tsb, ssb, "f32", None, endless=True)
        start_pair(rxa, txa)
        start_pair(rxb, txb)
        self.assertTrue(wait_for(lambda: rxb.status()["output_samples_emitted"] > 10000, 10))
        self.assertEqual(rxa.status()["srt_runtime_leases"], 4)
        # Stop pair B in the middle of pair A's transfer.
        stop_ms = max(stop_and_time(txb.tb), stop_and_time(rxb.tb))
        self.assertLess(stop_ms, 1000.0)
        self.assertEqual(rxa.status()["srt_runtime_leases"], 2)
        self.assertIsNotNone(wait_done(txa.tb, 30))
        self.assertIsNotNone(wait_done(rxa.tb, 30))
        self.assertEqual(rxa.data(), data)  # pair A never noticed
        self.assertEqual(rxa.status()["session"]["end"], "complete")
        self.assertEqual(rxa.status()["discontinuity_tags"], 0)
        self.assertEqual(rxa.status()["srt_runtime_leases"], 0)

    def test_second_caller_is_refused_and_the_first_is_unaffected(self):
        n = 3 * RATE
        data = test_signal("cf32", n)
        port = free_udp_port()
        ts, ss, tr, sr = configs(port, key=None)
        rx, tx = Receiver(tr, sr, "cf32"), Sender(ts, ss, "cf32", data)
        start_pair(rx, tx)
        self.assertTrue(wait_for(lambda: rx.status()["output_samples_emitted"] > 10000, 10))
        ts2, ss2, _, _ = configs(port, key=None, transport=dict(startup_timeout_ms=1200, connect_timeout_ms=400))
        intruder = Sender(ts2, ss2, "cf32", test_signal("cf32", 1000, start=5000))
        intruder.tb.start()
        self.assertIsNotNone(wait_done(intruder.tb, 10))
        i = intruder.status()
        self.assertEqual(i["session"]["counters"]["connections"], 0)  # refused in the handshake
        self.assertEqual(i["input_samples_consumed"], 0)
        self.assertEqual(i["session"]["failure"], "startup_timeout")  # "busy" is retried, then gives up
        self.assertIsNotNone(wait_done(tx.tb, 30))
        self.assertIsNotNone(wait_done(rx.tb, 30))
        r = rx.status()
        self.assertEqual(rx.data(), data)
        self.assertEqual(r["session"]["end"], "complete")
        self.assertGreaterEqual(r["session"]["counters"]["rejected_peers"], 1)
        self.assertEqual(r["session"]["counters"]["connections"], 1)
        evidence("second_caller", rejected=r["session"]["counters"]["rejected_peers"],
                 intruder_attempts=i["session"]["counters"]["connect_attempts"])

    def test_wrong_stream_id_is_refused_in_the_handshake(self):
        port = free_udp_port()
        ts, ss, _, _ = configs(port, key=None, transport=dict(stream_id="rwsrt/v1/other"))
        _, _, tr, sr = configs(port, key=None, transport=dict(startup_timeout_ms=2000))
        rx, tx = Receiver(tr, sr, "cf32"), Sender(ts, ss, "cf32", test_signal("cf32", 20000))
        start_pair(rx, tx)
        tx_wait = wait_done(tx.tb, 10)
        self.assertIsNotNone(tx_wait)
        self.assertIsNotNone(wait_done(rx.tb, 10))
        s, r = tx.status(), rx.status()
        self.assertEqual(s["session"]["failure"], "configuration")  # terminal: retrying cannot help
        self.assertLess(tx_wait, 2.0)
        self.assertEqual(s["session"]["counters"]["connections"], 0)
        self.assertEqual(s["input_samples_consumed"], 0)
        self.assertEqual(rx.data(), [])
        self.assertGreaterEqual(r["session"]["counters"]["rejected_peers"], 1)
        self.assertEqual(r["session"]["counters"]["connections"], 0)
        evidence("wrong_stream_id", sender_ended_after_s=tx_wait, detail=s["session"]["failure_detail"])

    # ------------------------------------------------------------ sender stall

    def test_stalled_upstream_keeps_the_connection_and_loses_nothing(self):
        n, before = 60000, 30007  # the pause starts inside a record
        data = test_signal("cf32", n)
        port = free_udp_port()
        ts, ss, tr, sr = configs(port, key=None)
        rx = Receiver(tr, sr, "cf32")
        tx_tb = gr.top_block()
        sink = robotweax_srt.srt_sink(ts, ss)
        # Keep a Python reference: GNU Radio holds Python blocks by raw pointer
        # and crashes if the object is collected while the flowgraph lives.
        source = paused_source(data, before, 2.0)
        tx_tb.connect(source, sink)
        rx.tb.start()
        time.sleep(0.2)
        tx_tb.start()
        # During the pause the partial record must already be at the receiver:
        # the flush interval holds while upstream delivers nothing.
        self.assertTrue(wait_for(lambda: rx.status()["output_samples_emitted"] == before, 1.6),
                        "partial record was held back during the stall: %d" %
                        rx.status()["output_samples_emitted"])
        mid = sink.status()
        self.assertFalse(mid["partial_record_pending"])
        self.assertEqual(mid["session"]["state"], "connected")
        self.assertIsNotNone(wait_done(tx_tb, 30))
        self.assertIsNotNone(wait_done(rx.tb, 30))
        r = rx.status()
        self.assertEqual(rx.data(), data)
        self.assertEqual(r["session"]["end"], "complete")
        self.assertEqual(r["discontinuity_tags"], 0)
        self.assertEqual(r["session"]["counters"]["connections"], 1)
        self.assertEqual(sink.status()["session"]["failure"], "none")


if __name__ == '__main__':
    gr_unittest.run(qa_robustness)
