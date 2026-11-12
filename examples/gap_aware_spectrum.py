#!/usr/bin/env python3
#
# Copyright 2026 Robotweax.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
"""Gap-aware spectrum of a stream received through the SRT Source.

  export RWSRT_PASSPHRASE='a passphrase of 10 to 79 bytes'
  gap_aware_spectrum.py --port 9000 --policy segmented_live

An FFT over samples from both sides of a gap shows energy that was never in
the signal. The gap_aware_fft block below builds every FFT window from
contiguous samples only: at a rwsrt.discontinuity tag it discards the
unfinished window and starts again at the tagged sample. The first spectrum
after a break carries a "spectrum.restart" tag so a display can mark it.

Without --gui it prints one JSON line per second (windows, restarts, peak
frequency). With --gui it shows a live spectrum; that needs GNU Radio's Qt
GUI on a machine with a display and is not covered by the automated tests.
"""

import argparse
import json
import sys
import threading

import numpy
import pmt
from gnuradio import gr

DISCONTINUITY = pmt.intern("rwsrt.discontinuity")
RESTART = pmt.intern("spectrum.restart")


class gap_aware_fft(gr.basic_block):
    """complex stream in, vectors of power in dB out, one per contiguous window."""

    def __init__(self, fft_size=1024):
        gr.basic_block.__init__(self, "gap_aware_fft", [numpy.complex64], [(numpy.float32, fft_size)])
        self.fft_size = fft_size
        self.window = numpy.hanning(fft_size).astype(numpy.float32)
        self.partial = numpy.zeros(0, dtype=numpy.complex64)
        self.restart_pending = False
        self.windows = 0
        self.restarts = 0
        self.discarded_samples = 0
        self.window_starts = []      # absolute input offset of each window (kept short)
        self.partial_start = 0
        self.last_peak_bin = -1
        self.set_tag_propagation_policy(gr.TPP_DONT)

    def forecast(self, noutput_items, ninputs):
        return [max(1, self.fft_size - len(self.partial))]

    def _emit(self, out, produced):
        spectrum = numpy.fft.fftshift(numpy.fft.fft(self.partial * self.window))
        power = (numpy.abs(spectrum) ** 2) / self.fft_size
        out[produced][:] = 10.0 * numpy.log10(power + 1e-20)
        self.last_peak_bin = int(numpy.argmax(power))
        if self.restart_pending:
            self.add_item_tag(0, self.nitems_written(0) + produced, RESTART, pmt.PMT_T)
            self.restart_pending = False
        self.windows += 1
        self.window_starts.append(self.partial_start)
        del self.window_starts[:-4096]
        self.partial = numpy.zeros(0, dtype=numpy.complex64)

    def general_work(self, input_items, output_items):
        samples = input_items[0]
        out = output_items[0]
        base = self.nitems_read(0)
        breaks = sorted(t.offset - base for t in
                        self.get_tags_in_window(0, 0, len(samples), DISCONTINUITY))
        produced = consumed = 0
        while consumed < len(samples) and produced < len(out):
            if breaks and breaks[0] == consumed:
                # The sample at this position is the first after a break:
                # nothing collected so far may share a window with it.
                breaks.pop(0)
                self.discarded_samples += len(self.partial)
                self.partial = numpy.zeros(0, dtype=numpy.complex64)
                self.restart_pending = True
                self.restarts += 1
            limit = breaks[0] if breaks else len(samples)
            take = min(self.fft_size - len(self.partial), limit - consumed)
            if len(self.partial) == 0:
                self.partial_start = base + consumed
            self.partial = numpy.concatenate((self.partial, samples[consumed:consumed + take]))
            consumed += take
            if len(self.partial) == self.fft_size:
                self._emit(out, produced)
                produced += 1
        self.consume(0, consumed)
        return produced


def main():
    from gnuradio import blocks, robotweax_srt
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--role", choices=["caller", "listener"], default="listener")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=9000)
    ap.add_argument("--rate", type=int, default=100000, help="must equal the sender's rate")
    ap.add_argument("--fft-size", type=int, default=1024)
    ap.add_argument("--policy", choices=["strict_stop", "segmented_live"], default="segmented_live")
    ap.add_argument("--passphrase-env", default="RWSRT_PASSPHRASE")
    ap.add_argument("--plaintext", action="store_true")
    ap.add_argument("--gui", action="store_true", help="live plot (needs gnuradio.qtgui and a display)")
    args = ap.parse_args()

    transport = robotweax_srt.make_transport_config(
        role=args.role, host=args.host, bind_address=args.bind, port=args.port,
        security="none" if args.plaintext else "ctr",
        passphrase_env="" if args.plaintext else args.passphrase_env)
    stream = robotweax_srt.make_stream_config(format="cf32", sample_rate_num=args.rate,
                                              continuity=args.policy)
    tb = gr.top_block()
    source = robotweax_srt.srt_source(transport, stream)
    fft = gap_aware_fft(args.fft_size)
    tb.connect(source, fft)

    app = None
    if args.gui:
        from PyQt5 import Qt
        import sip
        from gnuradio import qtgui
        app = Qt.QApplication(sys.argv)
        plot = qtgui.vector_sink_f(args.fft_size, -args.rate / 2.0, args.rate / float(args.fft_size),
                                   "Frequency (Hz)", "Power (dB)", "Gap-aware spectrum", 1)
        plot.set_y_axis(-120, 20)
        tb.connect(fft, plot)
        window = sip.wrapinstance(plot.qwidget(), Qt.QWidget)
        window.show()
    else:
        tb.connect(fft, blocks.null_sink(gr.sizeof_float * args.fft_size))

    def report():
        st = source.status()
        peak_hz = (fft.last_peak_bin - args.fft_size // 2) * args.rate / float(args.fft_size)
        return {"windows": fft.windows, "restarts": fft.restarts,
                "samples_discarded_at_breaks": fft.discarded_samples,
                "peak_hz": peak_hz if fft.last_peak_bin >= 0 else None,
                "state": st["session"]["state"], "failure": st["session"]["failure"],
                "end": st["session"]["end"], "received": st["output_samples_emitted"],
                "reported_missing": st["reported_missing_samples"]}

    tb.start()
    done = threading.Event()
    waiter = threading.Thread(target=lambda: (tb.wait(), done.set()), daemon=True)
    waiter.start()
    if app is not None:
        timer = Qt.QTimer()
        timer.timeout.connect(lambda: app.quit() if done.is_set() else None)
        timer.start(200)
        app.exec_()
        tb.stop()
    else:
        try:
            while not done.wait(1.0):
                print(json.dumps(report()), flush=True)
        except KeyboardInterrupt:
            tb.stop()
    waiter.join(5)
    final = report()
    print(json.dumps(final), flush=True)
    return 0 if final["failure"] == "none" else 1


if __name__ == "__main__":
    sys.exit(main())
