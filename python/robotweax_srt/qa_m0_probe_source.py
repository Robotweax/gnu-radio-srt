#!/usr/bin/env python
# -*- coding: utf-8 -*-
#
# Copyright 2026 Robotweax.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
"""M0 lifecycle qualification for the Source side on the pinned GNU Radio.

Characterises how the scheduler treats an empty-queue source under two wait
strategies. Each test prints its observations as one M0-EVIDENCE JSON line.
"""

import json
import os
import resource
import time

from gnuradio import blocks, gr, gr_unittest

try:
    from gnuradio.robotweax_srt import m0_probe_source
except ImportError:
    import sys
    dirname, filename = os.path.split(os.path.abspath(__file__))
    sys.path.append(os.path.join(dirname, "bindings"))
    from gnuradio.robotweax_srt import m0_probe_source

STOP_BOUND_MS = 1000.0
INJECTIONS = 12


def evidence(name, **fields):
    print("M0-EVIDENCE " + json.dumps({"test": name, **fields}, sort_keys=True), flush=True)


def cpu_seconds():
    u = resource.getrusage(resource.RUSAGE_SELF)
    return u.ru_utime + u.ru_stime


def run_injections(wait_inside_work):
    """Idle for a while, then push small batches at varying phases."""
    tb = gr.top_block()
    src = m0_probe_source(0, wait_inside_work)
    snk = blocks.vector_sink_c()
    tb.connect(src, snk)
    cpu0 = cpu_seconds()
    tb.start()
    time.sleep(1.0)
    idle = json.loads(src.report())
    for i in range(INJECTIONS):
        src.inject(100)
        time.sleep(0.31 + 0.017 * i)  # drift across any scheduler period
    t0 = time.monotonic()
    tb.stop()
    tb.wait()
    wall_stop_ms = (time.monotonic() - t0) * 1000.0
    r = json.loads(src.report())
    r.update(wall_stop_ms=wall_stop_ms, cpu_seconds=cpu_seconds() - cpu0,
             idle_work_calls_first_second=idle["work_calls"],
             items_downstream=len(snk.data()))
    return r


class qa_m0_probe_source(gr_unittest.TestCase):

    def test_return_zero_is_parked_by_the_scheduler(self):
        """Characterisation: returning 0 items costs far more than the wait.

        The pinned runtime re-calls a source that produced nothing only after
        its own timeout, so queue data can sit undelivered much longer than
        the block's 20 ms wait. This pins that behaviour; if a runtime update
        changes it, this test fails and the Source design can be revisited.
        """
        r = run_injections(False)
        evidence("source_return_zero", **r)
        self.assertEqual(r["items_downstream"], INJECTIONS * 100)
        self.assertLess(r["idle_work_calls_first_second"], 10)
        self.assertGreater(r["max_delivery_ms"], 100.0)
        self.assertLess(r["wall_stop_ms"], STOP_BOUND_MS)

    def test_wait_inside_work_delivers_promptly(self):
        """Waiting inside work(): prompt delivery, bounded stop, no spinning."""
        r = run_injections(True)
        evidence("source_wait_inside_work", **r)
        self.assertEqual(r["items_downstream"], INJECTIONS * 100)
        self.assertEqual(r["deliveries"], INJECTIONS)
        self.assertLess(r["max_delivery_ms"], 20.0)
        self.assertLess(r["wall_stop_ms"], STOP_BOUND_MS)
        self.assertLess(r["cpu_seconds"], 0.5)
        self.assertEqual(r["interruptions_seen"], 1)

    def test_work_done_ends_the_flowgraph(self):
        """WORK_DONE from the source completes tb.run() without a stop()."""
        for wait_inside_work in (False, True):
            tb = gr.top_block()
            src = m0_probe_source(300, wait_inside_work)
            snk = blocks.vector_sink_c()
            tb.connect(src, snk)
            src.inject(1000)
            t0 = time.monotonic()
            tb.run()
            run_ms = (time.monotonic() - t0) * 1000.0
            r = json.loads(src.report())
            evidence("source_work_done", wall_run_ms=run_ms,
                     items_downstream=len(snk.data()), **r)
            self.assertTrue(r["returned_work_done"])
            self.assertGreater(run_ms, 250.0)
            self.assertLess(run_ms, 300.0 + STOP_BOUND_MS)
            # Everything queued before completion reaches downstream.
            self.assertEqual(len(snk.data()), 1000)
            if wait_inside_work:
                self.assertLess(run_ms, 400.0)


if __name__ == '__main__':
    gr_unittest.run(qa_m0_probe_source)
