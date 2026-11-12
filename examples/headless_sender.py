#!/usr/bin/env python3
#
# Copyright 2026 Robotweax.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
"""Headless sender: a deterministic test tone through the SRT Sink.

  export RWSRT_PASSPHRASE='a passphrase of 10 to 79 bytes'
  headless_sender.py --host 127.0.0.1 --port 9000 --seconds 5

Prints one JSON status line per second on stdout and a final one when the
flowgraph ends. Exit status 0 only if the run completed (END handed to SRT).
"""

import argparse
import json
import sys
import threading

from gnuradio import analog, blocks, gr, robotweax_srt


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--role", choices=["caller", "listener"], default="caller")
    ap.add_argument("--host", default="127.0.0.1", help="peer address (caller)")
    ap.add_argument("--bind", default="127.0.0.1", help="local address (listener)")
    ap.add_argument("--port", type=int, default=9000)
    ap.add_argument("--rate", type=int, default=100000, help="samples per second")
    ap.add_argument("--seconds", type=float, default=5.0, help="0: run until interrupted")
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
    tone = analog.sig_source_c(args.rate, analog.GR_COS_WAVE, args.rate / 20.0, 0.5)
    # A synthetic source must be paced: unthrottled it measures throughput,
    # not a real-time stream. Real hardware provides its own pacing.
    throttle = blocks.throttle(gr.sizeof_gr_complex, args.rate, True)
    sink = robotweax_srt.srt_sink(transport, stream)
    chain = [tone, throttle]
    if args.seconds > 0:
        chain.append(blocks.head(gr.sizeof_gr_complex, int(args.rate * args.seconds)))
    tb.connect(*chain, sink)

    tb.start()
    done = threading.Event()
    waiter = threading.Thread(target=lambda: (tb.wait(), done.set()))
    waiter.start()
    try:
        while not done.wait(1.0):
            print(sink.status_json(), flush=True)
    except KeyboardInterrupt:
        tb.stop()  # cancellation: no END is sent
    waiter.join()
    status = sink.status()
    print(json.dumps(status), flush=True)
    if not status["session"]["completed"]:
        print("sender: %s %s" % (status["session"]["failure"], status["session"]["failure_detail"]),
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
