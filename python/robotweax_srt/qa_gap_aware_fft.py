#!/usr/bin/env python
#
# Copyright 2026 Robotweax.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
"""The gap-aware FFT of examples/gap_aware_spectrum.py: no window may contain
samples from both sides of a discontinuity."""

import os
import sys
import time

import numpy
import pmt
from gnuradio import blocks, gr, gr_unittest

from qa_common import (RATE, Proxy, Receiver, Sender, configs, evidence, free_udp_port, make_tag,
                       proxied_configs, robotweax_srt, wait_done)

sys.path.insert(0, os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                 "..", "..", "examples")))
from gap_aware_spectrum import gap_aware_fft  # noqa: E402

FFT = 256


class qa_gap_aware_fft(gr_unittest.TestCase):

    def test_windows_never_span_a_break(self):
        # Three stretches of 1000, 700 and 300 samples; each a tone in a
        # different bin, so a window mixing two stretches would show two peaks.
        lengths, bins = (1000, 700, 300), (20, 60, 100)
        parts = [numpy.exp(2j * numpy.pi * b * numpy.arange(n) / FFT) for n, b in zip(lengths, bins)]
        data = numpy.concatenate(parts).astype(numpy.complex64)
        tags = [make_tag(1000, "rwsrt.discontinuity", pmt.make_dict()),
                make_tag(1700, "rwsrt.discontinuity", pmt.make_dict())]
        tb = gr.top_block()
        src = blocks.vector_source_c(data.tolist(), False, 1, tags)
        fft = gap_aware_fft(FFT)
        out = blocks.vector_sink_f(FFT)
        tb.connect(src, fft, out)
        tb.run()
        # floor(1000/256) + floor(700/256) + floor(300/256) windows.
        self.assertEqual(fft.windows, 3 + 2 + 1)
        self.assertEqual(fft.restarts, 2)
        self.assertEqual(fft.discarded_samples, (1000 - 3 * FFT) + (700 - 2 * FFT))
        self.assertEqual(fft.window_starts, [0, 256, 512, 1000, 1256, 1700])
        spectra = numpy.array(out.data()).reshape(-1, FFT)
        peaks = [int(numpy.argmax(s)) - FFT // 2 for s in spectra]
        self.assertEqual(peaks, [20, 20, 20, 60, 60, 100])
        for s, own in zip(spectra, peaks):
            # No energy from another stretch's tone: at least 60 dB down.
            others = [b for b in bins if b != own]
            self.assertTrue(all(s[b + FFT // 2] < s[own + FFT // 2] - 60 for b in others))
        restart_offsets = sorted(t.offset for t in out.tags()
                                 if pmt.symbol_to_string(t.key) == "spectrum.restart")
        self.assertEqual(restart_offsets, [3, 5])  # first window after each break

    def test_break_on_the_first_sample_and_adjacent_breaks(self):
        data = numpy.ones(600, dtype=numpy.complex64)
        tags = [make_tag(0, "rwsrt.discontinuity", pmt.make_dict()),
                make_tag(300, "rwsrt.discontinuity", pmt.make_dict()),
                make_tag(301, "rwsrt.discontinuity", pmt.make_dict())]
        tb = gr.top_block()
        src = blocks.vector_source_c(data.tolist(), False, 1, tags)
        fft = gap_aware_fft(FFT)
        out = blocks.vector_sink_f(FFT)
        tb.connect(src, fft, out)
        tb.run()
        self.assertEqual(fft.restarts, 3)
        self.assertEqual(fft.window_starts, [0, 301])
        self.assertEqual(fft.discarded_samples, (300 - FFT) + 1)

    def test_behind_a_real_source_with_an_outage(self):
        # End to end: a tone through Sink, a 0.8 s outage, Source, gap-aware FFT.
        n = 3 * RATE
        tone = numpy.exp(2j * numpy.pi * 0.125 * numpy.arange(n)).astype(numpy.complex64)
        proxy, ts, ss, tr, sr = proxied_configs("--outage", "1.0:0.8", key=None, policy="segmented_live")
        rx_tb = gr.top_block()
        source = robotweax_srt.srt_source(tr, sr)
        fft = gap_aware_fft(FFT)
        out = blocks.vector_sink_f(FFT)
        rx_tb.connect(source, fft, out)
        tx = Sender(ts, ss, "cf32", tone.tolist())
        rx_tb.start()
        time.sleep(0.2)
        tx.tb.start()
        self.assertIsNotNone(wait_done(tx.tb, 30))
        self.assertIsNotNone(wait_done(rx_tb, 30))
        proxy.close()
        st = source.status()
        self.assertGreater(st["discontinuity_tags"], 0)
        self.assertEqual(fft.restarts, st["discontinuity_tags"])
        spectra = numpy.array(out.data()).reshape(-1, FFT)
        # The gap is not a multiple of 8 samples in general, so a window
        # spanning it would smear the tone. Every window stays a clean tone.
        peak = FFT // 2 + FFT // 8
        for s in spectra:
            self.assertEqual(int(numpy.argmax(s)), peak)
            floor = numpy.max(numpy.delete(s, range(peak - 3, peak + 4)))
            self.assertLess(floor, s[peak] - 50)
        evidence("gap_aware_fft_outage", windows=fft.windows, restarts=fft.restarts,
                 discarded=fft.discarded_samples, missing=st["reported_missing_samples"])


if __name__ == '__main__':
    gr_unittest.run(qa_gap_aware_fft)
