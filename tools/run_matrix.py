#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Runs network qualification experiments (plan section 12 C to E).

Each run puts a reference sender and a reference receiver into separate
network namespaces joined by a router namespace with netem, sends a paced
deterministic test signal for a fixed time, and writes one evidence
directory. SRT, UDP and TCP use the same peers, framing, queue and
validation; only the transport differs.

Run inside a privileged, disposable container (tools/dev/netem.sh does that):

  run_matrix.py --bin DIR --out DIR --plan primary|secondary|secondary-rep|smoke [--parallel N]

Definitions of metrics and gates: docs/benchmarking.md and tools/qual_common.py.
Nothing here asserts an outcome; tools/validate_results.py reads the evidence.
"""

import argparse
import concurrent.futures
import json
import os
import platform
import queue
import shutil
import subprocess
import sys
import threading
import time

import numpy

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qual_common as qc  # noqa: E402

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
TOPOLOGY = os.path.join(ROOT, "tests", "netem", "topology.sh")
PROFILES = json.load(open(os.path.join(ROOT, "tests", "netem", "profiles.json")))
TRANSPORTS = ("srt", "udp", "tcp")


def plan_runs(name, seconds, reps):
    """Run list in execution order. Transport order rotates with the
    repetition so that no transport always runs first or last."""
    runs = []

    def add(profile, rate, latency, rep, transports=TRANSPORTS):
        order = list(transports)
        shift = rep % len(order)
        for t in order[shift:] + order[:shift]:
            runs.append(dict(profile=profile, transport=t, rate=rate, latency_ms=latency, rep=rep,
                             seconds=seconds))

    if name == "primary":
        for rep in range(reps):
            for profile in ("N0", "N1", "N2"):
                add(profile, 100000, 250, rep)
    elif name == "secondary":
        for profile in ("N3", "N4", "N5", "N6"):
            add(profile, 100000, 250, 0)
        for latency in (100, 500):
            for profile in ("N0", "N1", "N2"):
                add(profile, 100000, latency, 0, ("srt", "udp"))
        for profile in ("N0", "N1", "N2"):
            add(profile, 1000000, 250, 0)
    elif name == "followup":
        # Added after the first results; see "Changes" in docs/benchmarking.md.
        add("N3", 100000, 250, 1, ("tcp",))
        add("N4b", 100000, 250, 0)
        add("N2", 100000, 250, 5, ("udp",))
    elif name == "rc2":
        # After 0.1.0-rc1: the optional lateness bound of the Source
        # (max_lateness_ms), on the profile where M4 found unbounded age
        # (N4), with and without it, and on N0 and N2 to see what it costs
        # where the path keeps up. SRT only: the baselines have no such bound.
        for rep in range(reps):
            for profile, late in (("N4", 0), ("N4", 150), ("N2", 150), ("N0", 150)):
                runs.append(dict(profile=profile, transport="srt", rate=100000, latency_ms=250,
                                 rep=rep, seconds=seconds, max_lateness_ms=late))
    elif name == "secondary-rep":
        # Repetitions of two secondary groups that had one run per cell:
        # burst loss (N3) at 100 kS/s and 1 MS/s on N0 to N2. Declared in
        # docs/benchmarking.md before the runs.
        for rep in range(reps):
            add("N3", 100000, 250, rep)
            for profile in ("N0", "N1", "N2"):
                add(profile, 1000000, 250, rep)
    elif name == "smoke":
        for profile in ("N0", "N2", "N5"):
            add(profile, 100000, 250, 0)
    else:
        raise SystemExit("unknown plan " + name)
    return runs


def sh(argv, check=True, timeout=30):
    p = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
    if check and p.returncode != 0:
        raise RuntimeError("%s failed: %s" % (" ".join(argv), p.stderr.strip()))
    return p.stdout


def netem_args(spec, wire_bps):
    args = []
    if "delay_ms" in spec:
        args += ["delay", "%dms" % spec["delay_ms"]]
        if spec.get("jitter_ms"):
            args += ["%dms" % spec["jitter_ms"]]
    if spec.get("loss"):
        args += ["loss"] + spec["loss"].split()
    if spec.get("rate"):
        args += ["rate", "%dbit" % int(wire_bps * float(spec["rate"][1:]))]
    return args


def apply_netem(ns, dev, spec, wire_bps, queue_packets=100000):
    args = netem_args(spec, wire_bps)
    if not args:
        sh(["ip", "netns", "exec", ns, "tc", "qdisc", "del", "dev", dev, "root"], check=False)
        return "none"
    # Default: a deep queue, so the profile and not the emulator's buffer
    # decides what is lost. Profiles with "queue_ms" set a realistic depth.
    sh(["ip", "netns", "exec", ns, "tc", "qdisc", "replace", "dev", dev, "root", "netem",
        "limit", str(queue_packets)] + args)
    return "limit %d %s" % (queue_packets, " ".join(args))


def forward_queue_packets(profile, wire_bps):
    """Queue depth of the forward bottleneck in packets (1244 bytes each)."""
    ms = profile["forward"].get("queue_ms")
    if not ms:
        return 100000
    factor = float(profile.get("event", {}).get("forward_rate", profile["forward"]["rate"])[1:])
    return max(50, int(ms / 1000.0 * wire_bps * factor / 8 / 1244))


def reduce_timing(path, t0_ns, cfg, warmup_s, restore_ns):
    """Reduces the receiver's per-record log to the metrics of benchmarking.md."""
    rate = cfg["rate"]
    total = cfg["samples"]
    lo = warmup_s * rate
    dtype = numpy.dtype([("i", "<u8"), ("n", "<u4"), ("r", "<u4"), ("t", "<i8")])
    rec = numpy.fromfile(path, dtype=dtype) if os.path.exists(path) else numpy.zeros(0, dtype=dtype)
    out = {"records_logged": int(len(rec)), "window": {"first_sample": lo, "end_sample": total,
                                                       "offered_samples": total - lo}}
    deadline = cfg["latency_ms"] + qc.DEADLINE_MARGIN_MS
    out["deadline_ms"] = deadline
    # Age of the first (oldest) sample of each record against its nominal
    # capture time t0 + index / rate on the shared monotonic clock.
    idx = rec["i"].astype(numpy.float64)
    age_ms = (rec["t"].astype(numpy.float64) - t0_ns) / 1e6 - idx * 1000.0 / rate
    # Records are clipped to the measurement window [lo, total).
    start = numpy.maximum(rec["i"].astype(numpy.int64), lo)
    stop = numpy.minimum(rec["i"].astype(numpy.int64) + rec["n"].astype(numpy.int64), total)
    inside = numpy.maximum(stop - start, 0)
    sel = inside > 0
    n = inside[sel]
    a = age_ms[sel]
    delivered = int(n.sum())
    offered = total - lo
    out["delivered_samples"] = delivered
    out["missing_samples"] = offered - delivered
    out["delivered_fraction"] = delivered / offered
    quantiles = {"p50": None, "p95": None, "p99": None, "max": None, "min": None}
    if delivered:
        order = numpy.argsort(a)
        cum = numpy.cumsum(n[order])
        for name, q in (("p50", 0.50), ("p95", 0.95), ("p99", 0.99)):
            quantiles[name] = float(a[order][min(numpy.searchsorted(cum, q * delivered), len(a) - 1)])
        quantiles["max"] = float(a.max())
        quantiles["min"] = float(a.min())
    out["age_ms"] = quantiles
    on_time = sel & (age_ms <= deadline)
    out["on_time_samples"] = int(inside[on_time].sum())
    out["on_time_fraction"] = out["on_time_samples"] / offered
    # Usable: samples in analysis windows of WINDOW consecutive samples that
    # were delivered completely and on time. A record spans at most two windows.
    W = qc.WINDOW
    first_w = -(-lo // W)
    last_w = total // W
    cover = numpy.zeros(max(last_w - first_w, 0), dtype=numpy.int64)
    if len(cover):
        i0 = rec["i"][on_time].astype(numpy.int64)
        n0 = rec["n"][on_time].astype(numpy.int64)
        w = i0 // W
        head = numpy.minimum(n0, (w + 1) * W - i0)
        for win, cnt in ((w, head), (w + 1, n0 - head)):
            ok = (win >= first_w) & (win < last_w) & (cnt > 0)
            numpy.add.at(cover, win[ok] - first_w, cnt[ok])
    usable = int((cover == W).sum()) * W
    out["analysis_windows"] = int(len(cover))
    out["usable_on_time_samples"] = usable
    out["usable_on_time_fraction"] = usable / (len(cover) * W) if len(cover) else None
    out["recovery_s"] = None
    if restore_ns is not None:
        after = (rec["t"] > restore_ns) & (age_ms <= deadline)
        if after.any():
            out["recovery_s"] = float((rec["t"][after].min() - restore_ns) / 1e9)
    return out


def run_one(cfg, slot, out_root, bins, warmup_s):
    name = "%s_%s_r%d_l%d_rep%d" % (cfg["profile"], cfg["transport"], cfg["rate"], cfg["latency_ms"], cfg["rep"])
    if cfg.get("max_lateness_ms"):
        name += "_late%d" % cfg["max_lateness_ms"]
    out = os.path.join(out_root, name)
    os.makedirs(out, exist_ok=True)
    profile = PROFILES["profiles"][cfg["profile"]]
    cfg = dict(cfg, samples=cfg["rate"] * cfg["seconds"], format="cf32",
               policy="strict" if cfg["profile"] == "N0" else "segmented")
    wire = qc.offered_wire_bits_per_second(cfg["rate"])
    manifest = {"name": name, "config": cfg, "profile": profile, "warmup_s": warmup_s,
                "offered_wire_bits_per_second": wire, "slot": slot, "hung": False,
                "started_unix": time.time(), "loadavg_start": os.getloadavg(),
                "clock": "CLOCK_MONOTONIC shared by sender, receiver and runner (same host)",
                "secrets": "none: all transports run in plaintext"}
    network = {}
    s, x, r = "rwq%ds" % slot, "rwq%dx" % slot, "rwq%dr" % slot
    rx_ip = "10.%d.2.2" % (100 + slot)
    try:
        network["topology"] = sh([TOPOLOGY, "up", str(slot)]).strip()
        fq = forward_queue_packets(profile, wire)
        network["forward_netem"] = apply_netem(x, "xr", profile["forward"], wire, fq)
        network["reverse_netem"] = apply_netem(x, "xs", profile["reverse"], wire)
        ping = sh(["ip", "netns", "exec", s, "ping", "-c", "20", "-i", "0.02", "-q", rx_ip], check=False)
        network["ping_before"] = [l for l in ping.splitlines() if "rtt" in l or "loss" in l]

        common = ["--transport=" + cfg["transport"], "--plaintext", "--port=9000",
                  "--rate=%d" % cfg["rate"], "--latency-ms=%d" % cfg["latency_ms"],
                  "--policy=" + cfg["policy"], "--startup-timeout-ms=10000"]
        timing = os.path.join(out, "timing.bin")
        rx_err = open(os.path.join(out, "receiver.events.jsonl"), "w")
        rx = subprocess.Popen(["ip", "netns", "exec", r, os.path.join(bins, "rwsrt_ref_receiver"),
                               "--role=listener", "--bind=" + rx_ip, "--events",
                               "--timeout-ms=%d" % ((cfg["seconds"] + 40) * 1000),
                               "--timing-log=" + timing] + common +
                              (["--max-lateness-ms=%d" % cfg["max_lateness_ms"]]
                               if cfg.get("max_lateness_ms") else []),
                              stdout=open(os.path.join(out, "receiver.jsonl"), "w"), stderr=rx_err)
        time.sleep(0.5)
        t_start = time.monotonic()
        tx = subprocess.Popen(["ip", "netns", "exec", s, os.path.join(bins, "rwsrt_ref_sender"),
                               "--role=caller", "--host=" + rx_ip, "--samples=%d" % cfg["samples"]] + common,
                              stdout=open(os.path.join(out, "sender.jsonl"), "w"),
                              stderr=open(os.path.join(out, "sender.err"), "w"))

        restore_ns = None
        event = profile.get("event")
        if event:
            # 40 s into the measurement window for full-length runs; earlier
            # only in short harness checks, which are not qualification data.
            at = warmup_s + min(PROFILES["event_start_s"], (cfg["seconds"] - warmup_s) * 0.3)
            network["event_at_s"] = at
            time.sleep(max(0.0, t_start + at - time.monotonic()))
            if event["kind"] == "outage":
                for dev in ("xr", "xs"):
                    sh(["ip", "netns", "exec", x, "tc", "qdisc", "replace", "dev", dev, "root", "netem",
                        "loss", "100%"])
            else:
                fwd = dict(profile["forward"], rate=event["forward_rate"])
                apply_netem(x, "xr", fwd, wire, fq)
            network["event_applied_mono_ns"] = time.monotonic_ns()
            network["event_qdisc"] = sh(["ip", "netns", "exec", x, "tc", "qdisc", "show", "dev", "xr"]).strip()
            time.sleep(event["duration_s"])
            apply_netem(x, "xr", profile["forward"], wire, fq)
            apply_netem(x, "xs", profile["reverse"], wire)
            restore_ns = time.monotonic_ns()
            network["event_restored_mono_ns"] = restore_ns

        for proc in (tx, rx):
            try:
                proc.wait(timeout=max(5.0, t_start + cfg["seconds"] + 60 - time.monotonic()))
            except subprocess.TimeoutExpired:
                proc.kill()
                manifest["hung"] = True
        manifest["exit"] = {"sender": tx.returncode, "receiver": rx.returncode}
        for dev, key in (("xr", "forward"), ("xs", "reverse")):
            network[key + "_qdisc_stats"] = sh(["ip", "netns", "exec", x, "tc", "-s", "qdisc", "show", "dev", dev],
                                              check=False).strip()
        txj = qc.read_last_json(os.path.join(out, "sender.jsonl"))
        t0 = txj["t0_mono_ns"] if txj else None
        metrics = reduce_timing(timing, t0, cfg, warmup_s, restore_ns) if t0 else {"error": "no sender result"}
        if txj:
            metrics["sender_cpu_seconds"] = txj["resources"]["cpu_seconds"]
            metrics["sender_max_rss_kib"] = txj["resources"]["max_rss_kib"]
        rxj = qc.read_last_json(os.path.join(out, "receiver.jsonl"))
        if rxj:
            metrics["receiver_cpu_seconds"] = rxj["resources"]["cpu_seconds"]
            metrics["receiver_max_rss_kib"] = rxj["resources"]["max_rss_kib"]
            metrics["queue_high_water_records"] = {"sender": txj["queue"]["high_water_records"] if txj else None,
                                                   "receiver": rxj["queue"]["high_water_records"]}
    except Exception as e:  # the run is recorded as failed, never dropped
        manifest["error"] = repr(e)
        metrics = {"error": repr(e)}
    finally:
        sh([TOPOLOGY, "down", str(slot)], check=False)
        if os.path.exists(os.path.join(out, "timing.bin")):
            os.remove(os.path.join(out, "timing.bin"))  # reduced above; too large to keep
    manifest["loadavg_end"] = os.getloadavg()
    manifest["duration_s"] = round(time.time() - manifest["started_unix"], 1)
    for fname, obj in (("manifest.json", manifest), ("network.json", network), ("metrics.json", metrics)):
        with open(os.path.join(out, fname), "w") as f:
            json.dump(obj, f, indent=1)
    verdict = qc.judge(out)
    with open(os.path.join(out, "verdict.json"), "w") as f:
        json.dump(verdict, f, indent=1)
    return name, verdict["overall"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True, help="directory with rwsrt_ref_sender and rwsrt_ref_receiver")
    ap.add_argument("--out", required=True)
    ap.add_argument("--plan", required=True)
    ap.add_argument("--seconds", type=int, default=120)
    ap.add_argument("--warmup", type=int, default=10)
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--parallel", type=int, default=1)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    runs = plan_runs(args.plan, args.seconds, args.reps)
    environment = {
        "plan": args.plan, "seconds": args.seconds, "warmup_s": args.warmup, "parallel": args.parallel,
        "machine": platform.machine(), "kernel": platform.release(), "cpus": os.cpu_count(),
        "iproute2": sh(["ip", "-V"]).strip(),
        "note": "container on a shared, virtualised host; not a reference host for timing",
        "run_order": ["%s/%s/r%d/l%d/rep%d" % (r["profile"], r["transport"], r["rate"], r["latency_ms"], r["rep"])
                      for r in runs],
    }
    lock = os.path.join(ROOT, "dependencies.lock.json")
    environment["robotweax_srt_commit"] = json.load(open(lock))["robotweax_srt"]["commit"]
    with open(os.path.join(args.out, "environment.json"), "w") as f:
        json.dump(environment, f, indent=1)

    slots = queue.Queue()
    for i in range(args.parallel):
        slots.put(i)
    print_lock = threading.Lock()

    def work(cfg):
        slot = slots.get()
        try:
            name, overall = run_one(cfg, slot, args.out, args.bin, args.warmup)
        finally:
            slots.put(slot)
        with print_lock:
            print("%-40s %s" % (name, overall), flush=True)
        return overall

    with concurrent.futures.ThreadPoolExecutor(args.parallel) as pool:
        results = list(pool.map(work, runs))
    print("runs: %d, pass %d, fail %d, inconclusive %d" % (
        len(results), results.count("pass"), results.count("fail"), results.count("inconclusive")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
