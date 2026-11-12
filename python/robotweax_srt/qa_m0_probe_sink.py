#!/usr/bin/env python
# -*- coding: utf-8 -*-
#
# Copyright 2026 Robotweax.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
"""M0 lifecycle qualification for the Sink side on the pinned GNU Radio.

Each test prints its observations as one JSON line (prefix M0-EVIDENCE) so a
run log doubles as the evidence quoted in docs/decisions.md.
"""

import json
import os
import socket
import time

from gnuradio import blocks, gr, gr_unittest

try:
    from gnuradio.robotweax_srt import m0_probe_sink, m0_probe_source
except ImportError:
    import sys
    dirname, filename = os.path.split(os.path.abspath(__file__))
    sys.path.append(os.path.join(dirname, "bindings"))
    from gnuradio.robotweax_srt import m0_probe_sink, m0_probe_source

STOP_BOUND_MS = 1000.0  # plan: stop_timeout_ms on the reference host


def free_udp_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def thread_count():
    with open("/proc/self/status") as f:
        for line in f:
            if line.startswith("Threads:"):
                return int(line.split()[1])
    return -1


def thread_names():
    names = []
    for tid in os.listdir("/proc/self/task"):
        try:
            with open("/proc/self/task/%s/comm" % tid) as f:
                names.append(f.read().strip())
        except OSError:
            pass  # the thread exited while we were listing
    return sorted(names)


def evidence(name, **fields):
    print("M0-EVIDENCE " + json.dumps({"test": name, **fields}, sort_keys=True), flush=True)


def stop_and_time(tb):
    t0 = time.monotonic()
    tb.stop()
    tb.wait()
    return (time.monotonic() - t0) * 1000.0


class qa_m0_probe_sink(gr_unittest.TestCase):

    def test_rejects_invalid_configuration(self):
        with self.assertRaises(Exception):
            m0_probe_sink(False, "not-an-address", 9000)
        with self.assertRaises(Exception):
            m0_probe_sink(False, "127.0.0.1", 0)
        with self.assertRaises(Exception):
            m0_probe_sink(False, "127.0.0.1", 65536)

    def test_natural_eof_is_distinguishable(self):
        """Finite upstream: input is marked done and fully consumed at stop()."""
        n = 100000
        tb = gr.top_block()
        src = blocks.vector_source_c([complex(i, -i) for i in range(n)], False)
        snk = m0_probe_sink(False, "127.0.0.1", free_udp_port())
        tb.connect(src, snk)
        t0 = time.monotonic()
        tb.run()
        run_ms = (time.monotonic() - t0) * 1000.0
        r = json.loads(snk.report())
        evidence("natural_eof", run_ms=run_ms, **r)
        self.assertEqual(r["items_consumed"], n)
        self.assertTrue(r["stop_input_done"])
        self.assertEqual(r["stop_items_pending"], 0)
        self.assertEqual(r["effective_stops"], 1)
        self.assertEqual(r["runtime_leases"], 0)
        self.assertLess(r["stop_ms"], STOP_BOUND_MS)

    def test_head_limited_eof(self):
        """EOF produced by an intermediate block (head), not the source."""
        n = 4096 + 17
        tb = gr.top_block()
        src = blocks.null_source(gr.sizeof_gr_complex)
        head = blocks.head(gr.sizeof_gr_complex, n)
        snk = m0_probe_sink(False, "127.0.0.1", free_udp_port())
        tb.connect(src, head, snk)
        tb.run()
        r = json.loads(snk.report())
        evidence("head_limited_eof", **r)
        self.assertEqual(r["items_consumed"], n)
        self.assertTrue(r["stop_input_done"])
        self.assertEqual(r["stop_items_pending"], 0)

    def test_empty_finite_run(self):
        """Upstream finishes without ever producing a sample."""
        tb = gr.top_block()
        src = blocks.null_source(gr.sizeof_gr_complex)
        head = blocks.head(gr.sizeof_gr_complex, 0)
        snk = m0_probe_sink(False, "127.0.0.1", free_udp_port())
        tb.connect(src, head, snk)
        tb.run()
        r = json.loads(snk.report())
        evidence("empty_finite_run", **r)
        self.assertEqual(r["items_consumed"], 0)
        self.assertEqual(r["work_calls"], 0)
        self.assertTrue(r["stop_input_done"])
        self.assertEqual(r["stop_items_pending"], 0)

    def test_user_stop_is_cancellation(self):
        """Endless upstream + tb.stop(): input must not look like EOF."""
        for listener in (False, True):
            tb = gr.top_block()
            src = blocks.null_source(gr.sizeof_gr_complex)
            thr = blocks.throttle(gr.sizeof_gr_complex, 100000)
            snk = m0_probe_sink(listener, "127.0.0.1", free_udp_port())
            tb.connect(src, thr, snk)
            tb.start()
            time.sleep(0.6)
            wall_ms = stop_and_time(tb)
            r = json.loads(snk.report())
            evidence("user_stop", listener=listener, wall_stop_ms=wall_ms, **r)
            self.assertGreater(r["items_consumed"], 0)
            self.assertFalse(r["stop_input_done"])
            self.assertEqual(r["connections"], 0)
            self.assertEqual(r["setup_errors"], 0)
            self.assertEqual(r["worker_state"], 3)
            self.assertEqual(r["runtime_leases"], 0)
            self.assertLess(wall_ms, STOP_BOUND_MS)
            self.assertLess(r["max_wait_slice_ms"], 60.0)

    def test_stop_with_no_samples_and_no_peer(self):
        """A flowgraph that never delivers a sample still stops in bounds.

        Upstream is the empty-queue probe source. (A slow blocks.throttle is
        unsuitable here: its uninterruptible sleep, not this module, would
        then decide how long tb.stop() takes.)
        """
        tb = gr.top_block()
        src = m0_probe_source(0, True)
        snk = m0_probe_sink(False, "127.0.0.1", free_udp_port())
        tb.connect(src, snk)
        tb.start()
        time.sleep(0.3)
        wall_ms = stop_and_time(tb)
        r = json.loads(snk.report())
        evidence("stop_no_samples_no_peer", wall_stop_ms=wall_ms, **r)
        self.assertEqual(r["work_calls"], 0)
        self.assertFalse(r["stop_input_done"])
        self.assertLess(wall_ms, STOP_BOUND_MS)
        self.assertEqual(r["runtime_leases"], 0)

    def test_two_instances_connect_and_stop_independently(self):
        """Listener and Caller probes in one process share the SRT runtime."""
        port = free_udp_port()

        def graph(listener):
            tb = gr.top_block()
            src = blocks.null_source(gr.sizeof_gr_complex)
            thr = blocks.throttle(gr.sizeof_gr_complex, 100000)
            snk = m0_probe_sink(listener, "127.0.0.1", port)
            tb.connect(src, thr, snk)
            return tb, snk

        tb_l, snk_l = graph(True)
        tb_c, snk_c = graph(False)
        tb_l.start()
        tb_c.start()
        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            if json.loads(snk_l.report())["worker_state"] == 2 and \
                    json.loads(snk_c.report())["worker_state"] == 2:
                break
            time.sleep(0.02)
        both = (json.loads(snk_l.report()), json.loads(snk_c.report()))
        # Stopping one instance must not clean SRT up under the other.
        caller_stop_ms = stop_and_time(tb_c)
        time.sleep(0.3)
        mid = json.loads(snk_l.report())
        listener_stop_ms = stop_and_time(tb_l)
        end = json.loads(snk_l.report())
        evidence("two_instances", connected_listener=both[0], connected_caller=both[1],
                 caller_stop_ms=caller_stop_ms, listener_after_caller_stop=mid,
                 listener_stop_ms=listener_stop_ms, final=end)
        self.assertEqual(both[0]["worker_state"], 2)
        self.assertEqual(both[1]["worker_state"], 2)
        self.assertEqual(both[0]["runtime_leases"], 2)
        self.assertEqual(mid["runtime_leases"], 1)
        self.assertEqual(mid["setup_errors"], 0)
        self.assertEqual(end["runtime_leases"], 0)
        self.assertLess(caller_stop_ms, STOP_BOUND_MS)
        self.assertLess(listener_stop_ms, STOP_BOUND_MS)

    def test_start_stop_cycles_do_not_leak(self):
        """100 start/stop cycles of one block: bounded stops, no thread/fd leak."""
        cycles = 100
        tb = gr.top_block()
        src = blocks.null_source(gr.sizeof_gr_complex)
        thr = blocks.throttle(gr.sizeof_gr_complex, 100000)
        snk = m0_probe_sink(False, "127.0.0.1", free_udp_port())
        tb.connect(src, thr, snk)

        def one_cycle():
            tb.start()
            time.sleep(0.03)
            return stop_and_time(tb)

        one_cycle()  # warm-up: lazily created process-wide state settles
        names_before = thread_names()
        threads_before = thread_count()
        fds_before = len(os.listdir("/proc/self/fd"))
        worst = max(one_cycle() for _ in range(cycles))
        # Sampled immediately, then again after a settle period: a thread
        # that is still unwinding is not a leak, one that persists is.
        threads_immediately = thread_count()
        settle_ms = 0
        while thread_count() != threads_before and settle_ms < 1000:
            time.sleep(0.01)
            settle_ms += 10
        threads_after = thread_count()
        names_after = thread_names()
        fds_after = len(os.listdir("/proc/self/fd"))
        r = json.loads(snk.report())
        evidence("start_stop_cycles", cycles=cycles, worst_wall_stop_ms=worst,
                 threads_before=threads_before, threads_immediately=threads_immediately,
                 threads_after=threads_after, thread_settle_ms=settle_ms,
                 thread_names_before=names_before, thread_names_after=names_after,
                 fds_before=fds_before, fds_after=fds_after, **r)
        self.assertEqual(r["start_calls"], cycles + 1)
        self.assertEqual(r["effective_stops"], cycles + 1)
        self.assertEqual(r["runtime_leases"], 0)
        self.assertLess(worst, STOP_BOUND_MS)
        self.assertEqual(threads_after, threads_before)
        self.assertEqual(fds_after, fds_before)

if __name__ == '__main__':
    gr_unittest.run(qa_m0_probe_sink)
