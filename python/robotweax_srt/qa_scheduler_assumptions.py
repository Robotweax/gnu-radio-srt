#!/usr/bin/env python
#
# Copyright 2026 Robotweax.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
"""Pins a behaviour of the GNU Radio scheduler that the Source design
depends on (docs/decisions.md D7). If a runtime update changes it, this test
fails and the design can be revisited. It tests GNU Radio, not this module.
"""

import time

import numpy
from gnuradio import blocks, gr, gr_unittest


class idle_source(gr.sync_block):
    """A source that never has data and returns 0 items at once."""

    def __init__(self):
        gr.sync_block.__init__(self, "idle_source", None, [numpy.complex64])
        self.calls = 0

    def work(self, input_items, output_items):
        self.calls += 1
        return 0


class qa_scheduler_assumptions(gr_unittest.TestCase):

    def test_a_source_returning_zero_items_is_parked(self):
        tb = gr.top_block()
        src = idle_source()
        tb.connect(src, blocks.null_sink(gr.sizeof_gr_complex))
        tb.start()
        time.sleep(2.0)
        tb.stop()
        tb.wait()
        per_second = src.calls / 2.0
        print("M2-EVIDENCE {\"test\": \"scheduler_parks_zero_return\", \"calls_per_second\": %.1f}" % per_second,
              flush=True)
        # About 4 per second on 3.10.9: the scheduler waits roughly 250 ms.
        # A block that polled its queue this way would add that much latency.
        self.assertGreater(per_second, 1.0)
        self.assertLess(per_second, 10.0)


if __name__ == '__main__':
    gr_unittest.run(qa_scheduler_assumptions)
