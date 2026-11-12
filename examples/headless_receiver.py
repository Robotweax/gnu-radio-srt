#!/usr/bin/env python3
#
# Copyright 2026 Robotweax.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
"""Headless receiver: SRT Source into a tag-aware counter.

  export RWSRT_PASSPHRASE='a passphrase of 10 to 79 bytes'
  headless_receiver.py --port 9000

Prints one JSON status line per second on stdout, every discontinuity tag on
stderr, and a final status line when the flowgraph ends. Exit status 0 only
if the run was observed completely (clean start, no gap, END).
"""

import argparse
import json
import sys
import threading

import numpy
import pmt
from gnuradio import gr, robotweax_srt


class gap_aware_counter(gr.sync_block):
    """Counts samples per contiguous stretch and reports every break.

    A stand-in for real processing: it shows where a downstream block must
    reset its state. Samples before and after a break are never combined.
    """

    def __init__(self):
        gr.sync_block.__init__(self, "gap_aware_counter", [numpy.complex64], None)
        self.samples = 0
        self.stretches = 1
        self.current_stretch = 0

    def work(self, input_items, output_items):
        n = len(input_items[0])
        start = self.nitems_read(0)
        breaks = self.get_tags_in_window(0, 0, n, pmt.intern("rwsrt.discontinuity"))
        cursor = 0
        for tag in sorted(breaks, key=lambda t: t.offset):
            at = tag.offset - start
            self.current_stretch += at - cursor  # samples before the break
            print(json.dumps({"event": "discontinuity", "output_offset": tag.offset,
                              "contiguous_before": self.current_stretch,
                              "tag": pmt.to_python(tag.value)}), file=sys.stderr, flush=True)
            self.stretches += 1
            self.current_stretch = 0  # processing state is reset here
            cursor = at
        self.current_stretch += n - cursor
        self.samples += n
        return n


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--role", choices=["caller", "listener"], default="listener")
    ap.add_argument("--host", default="127.0.0.1", help="peer address (caller)")
    ap.add_argument("--bind", default="127.0.0.1", help="local address (listener)")
    ap.add_argument("--port", type=int, default=9000)
    ap.add_argument("--rate", type=int, default=100000, help="must equal the sender's rate")
    ap.add_argument("--latency-ms", type=int, default=250)
    ap.add_argument("--policy", choices=["strict_stop", "segmented_live"], default="strict_stop")
    ap.add_argument("--passphrase-env", default="RWSRT_PASSPHRASE")
    ap.add_argument("--plaintext", action="store_true", help="explicit local-test mode, no encryption")
    args = ap.parse_args()

    transport = robotweax_srt.make_transport_config(
        role=args.role, host=args.host, bind_address=args.bind, port=args.port,
        latency_ms=args.latency_ms, security="none" if args.plaintext else "ctr",
        passphrase_env="" if args.plaintext else args.passphrase_env)
    stream = robotweax_srt.make_stream_config(format="cf32", sample_rate_num=args.rate,
                                              continuity=args.policy)

    tb = gr.top_block()
    source = robotweax_srt.srt_source(transport, stream)
    counter = gap_aware_counter()
    tb.connect(source, counter)

    tb.start()
    done = threading.Event()
    waiter = threading.Thread(target=lambda: (tb.wait(), done.set()))
    waiter.start()
    try:
        while not done.wait(1.0):
            print(source.status_json(), flush=True)
    except KeyboardInterrupt:
        tb.stop()
    waiter.join()
    status = source.status()
    status["consumer"] = {"samples": counter.samples, "contiguous_stretches": counter.stretches}
    print(json.dumps(status), flush=True)
    if status["session"]["end"] != "complete":
        print("receiver: end=%s failure=%s %s" % (status["session"]["end"], status["session"]["failure"],
                                                  status["session"]["failure_detail"]), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
