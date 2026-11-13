#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Deterministic single-peer UDP relay for robustness tests.

Sits between an SRT Caller and an SRT Listener on one host and applies
scripted faults. It is a fault injector for repeatable functional tests, not
a network emulator: no delay, jitter, reordering or rate limit. Qualification
under realistic impairment uses netem (milestone M4).

  udp_proxy.py --listen 127.0.0.1:9100 --target 127.0.0.1:9000 --outage 1.0:2.5

Faults (times in seconds since the first relayed packet):
  --outage START:DURATION    drop everything, both directions (repeatable)
  --blackhole-after T        drop everything from T on (peer vanished)
  --loss P [--seed N]        drop each packet independently with probability P
  --drop-end                 drop SRT data packets carrying an RWSR END record
  --drop-end-first N         drop only the first N such packets
  --stall START:DURATION     hold sender-to-receiver packets that arrive in the
                             interval and release them, in order, at its end:
                             a path that queues instead of dropping
  --drop-sample N            drop SRT data packets whose RWSR DATA record
                             contains sample index N of segment 0
The last two read the RWSR header and therefore need plaintext streams.
Retransmissions of a dropped record are dropped too.

Prints {"event": "ready"} once bound. On SIGTERM or SIGINT it writes its
counters as one JSON object to stdout (and to --stats-file) and exits.
"""

import argparse
import json
import random
import selectors
import signal
import socket
import struct
import sys
import time

SRT_HEADER = 16
RWSR_HEADER = 96


def endpoint(value):
    host, port = value.rsplit(":", 1)
    socket.inet_pton(socket.AF_INET, host)
    return host, int(port)


def interval(value):
    start, duration = value.split(":")
    return float(start), float(start) + float(duration)


def rwsr_record(datagram):
    """(kind, segment, first_index, count) of the RWSR record in an SRT data
    packet, or None for control packets, encrypted or foreign payloads."""
    if len(datagram) < SRT_HEADER + RWSR_HEADER or datagram[0] & 0x80:
        return None  # control packet, or too short
    p = datagram[SRT_HEADER:]
    if p[0:4] != b"RWSR" or p[4] != 1:
        return None
    segment, first = struct.unpack_from(">QQ", p, 32)
    count = struct.unpack_from(">I", p, 48)[0]
    return p[5], segment, first, count


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--listen", type=endpoint, required=True)
    ap.add_argument("--target", type=endpoint, required=True)
    ap.add_argument("--outage", type=interval, action="append", default=[])
    ap.add_argument("--blackhole-after", type=float)
    ap.add_argument("--loss", type=float, default=0.0)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--drop-end", action="store_true")
    ap.add_argument("--drop-end-first", type=int, default=0)
    ap.add_argument("--drop-sample", type=int)
    ap.add_argument("--stall", type=interval)
    ap.add_argument("--stats-file")
    args = ap.parse_args()

    counters = dict(forward=0, reverse=0, dropped_outage=0, dropped_blackhole=0, dropped_loss=0,
                    dropped_end=0, dropped_sample=0, data_records_seen=0, end_records_seen=0,
                    send_errors=0, stalled=0)
    rng = random.Random(args.seed)
    stop = {"flag": False}
    signal.signal(signal.SIGTERM, lambda *_: stop.update(flag=True))
    signal.signal(signal.SIGINT, lambda *_: stop.update(flag=True))

    front = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    back = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    for s in (front, back):
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
        s.setblocking(False)
    front.bind(args.listen)
    back.connect(args.target)
    selector = selectors.DefaultSelector()
    selector.register(front, selectors.EVENT_READ, "front")
    selector.register(back, selectors.EVENT_READ, "back")
    print(json.dumps({"event": "ready", "listen": "%s:%d" % args.listen}), flush=True)

    caller = None
    t0 = None

    def verdict(datagram, now):
        t = now - t0
        if args.blackhole_after is not None and t >= args.blackhole_after:
            return "dropped_blackhole"
        if any(a <= t < b for a, b in args.outage):
            return "dropped_outage"
        if args.drop_end or args.drop_end_first or args.drop_sample is not None:
            rec = rwsr_record(datagram)
            if rec is not None:
                kind, segment, first, count = rec
                if kind == 2:
                    counters["end_records_seen"] += 1
                    if args.drop_end or counters["dropped_end"] < args.drop_end_first:
                        return "dropped_end"
                elif kind == 1:
                    counters["data_records_seen"] += 1
                    if (args.drop_sample is not None and segment == 0 and
                            first <= args.drop_sample < first + count):
                        return "dropped_sample"
        if args.loss > 0.0 and rng.random() < args.loss:
            return "dropped_loss"
        return None

    held = []  # forward datagrams waiting for the end of the stall

    while not stop["flag"]:
        if held and time.monotonic() - t0 >= args.stall[1]:
            for datagram in held:  # in arrival order, before anything newer
                try:
                    back.send(datagram)
                    counters["forward"] += 1
                except (BlockingIOError, ConnectionRefusedError, OSError):
                    counters["send_errors"] += 1
            held = []
        for key, _ in selector.select(timeout=0.005 if held else 0.05):
            sock = key.fileobj
            while True:
                try:
                    if key.data == "front":
                        datagram, addr = sock.recvfrom(2048)
                        caller = addr  # one peer: the most recent sender
                    else:
                        datagram = sock.recv(2048)
                except (BlockingIOError, ConnectionRefusedError):
                    break
                now = time.monotonic()
                if t0 is None:
                    t0 = now
                why = verdict(datagram, now)
                if why:
                    counters[why] += 1
                    continue
                if (key.data == "front" and args.stall and
                        (held or args.stall[0] <= now - t0 < args.stall[1])):
                    # Also while the release is pending, so that order is kept.
                    held.append(datagram)
                    counters["stalled"] += 1
                    continue
                try:
                    if key.data == "front":
                        back.send(datagram)
                        counters["forward"] += 1
                    elif caller is not None:
                        front.sendto(datagram, caller)
                        counters["reverse"] += 1
                except (BlockingIOError, ConnectionRefusedError, OSError):
                    counters["send_errors"] += 1  # e.g. the listener is not up yet

    text = json.dumps(counters, sort_keys=True)
    if args.stats_file:
        with open(args.stats_file, "w") as f:
            f.write(text + "\n")
    print(text, flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
