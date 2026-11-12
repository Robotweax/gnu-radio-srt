#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validates a directory of qualification runs and summarises it.

  validate_results.py RESULTS_DIR [--summary-dir DIR]

Verdicts are derived again from the recorded evidence, never taken from a
run's own verdict.json; a difference between the two is reported as an
error. Failed and inconclusive runs stay in the summary. Exit status: 0 no
mandatory gate failed, 1 at least one failed, 2 evidence is missing or
inconsistent.
"""

import argparse
import json
import os
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qual_common as qc  # noqa: E402

METRICS = ("delivered_fraction", "on_time_fraction", "usable_on_time_fraction")
AGES = ("p50", "p95", "p99")


def spread(values):
    values = [v for v in values if v is not None]
    if not values:
        return None
    return {"median": statistics.median(values), "min": min(values), "max": max(values), "n": len(values)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("results")
    ap.add_argument("--summary-dir")
    args = ap.parse_args()
    runs = sorted(d for d in os.listdir(args.results) if os.path.isdir(os.path.join(args.results, d)))
    if not runs:
        print("no runs found in " + args.results)
        return 2
    groups, rows, inconsistent = {}, [], []
    for name in runs:
        path = os.path.join(args.results, name)
        verdict = qc.judge(path)
        stored = None
        if os.path.exists(os.path.join(path, "verdict.json")):
            stored = json.load(open(os.path.join(path, "verdict.json")))
        if stored is None or stored["overall"] != verdict["overall"]:
            inconsistent.append(name)
        row = {"run": name, "overall": verdict["overall"],
               "failed_gates": [g for g, v in verdict["gates"].items() if v["result"] == "fail"],
               "inconclusive_gates": [g for g, v in verdict["gates"].items() if v["result"] == "inconclusive"],
               "reasons": {g: v["reason"] for g, v in verdict["gates"].items() if v["result"] != "pass"}}
        if os.path.exists(os.path.join(path, "manifest.json")) and os.path.exists(os.path.join(path, "metrics.json")):
            cfg = json.load(open(os.path.join(path, "manifest.json")))["config"]
            metrics = json.load(open(os.path.join(path, "metrics.json")))
            key = (cfg["profile"], cfg["rate"], cfg["latency_ms"], cfg["transport"])
            groups.setdefault(key, []).append((verdict, metrics))
            row["config"] = cfg
            row["metrics"] = {k: metrics.get(k) for k in METRICS + ("missing_samples", "recovery_s", "age_ms",
                                                                    "sender_cpu_seconds", "receiver_cpu_seconds")}
        rows.append(row)

    table = []
    for key in sorted(groups):
        profile, rate, latency, transport = key
        entries = groups[key]
        ms = [m for _, m in entries if "error" not in m]
        item = {"profile": profile, "rate": rate, "latency_ms": latency, "transport": transport,
                "runs": len(entries),
                "verdicts": {v: [e[0]["overall"] for e in entries].count(v) for v in ("pass", "fail", "inconclusive")}}
        for k in METRICS:
            item[k] = spread([m.get(k) for m in ms])
        for k in AGES:
            item["age_" + k + "_ms"] = spread([m["age_ms"][k] for m in ms if m.get("age_ms")])
        item["recovery_s"] = spread([m.get("recovery_s") for m in ms])
        item["cpu_seconds_sender"] = spread([m.get("sender_cpu_seconds") for m in ms])
        item["cpu_seconds_receiver"] = spread([m.get("receiver_cpu_seconds") for m in ms])
        table.append(item)

    def pct(s):
        return "-" if s is None else "%.3f (%.3f-%.3f)" % (100 * s["median"], 100 * s["min"], 100 * s["max"])

    def ms_(s):
        return "-" if s is None else "%.0f (%.0f-%.0f)" % (s["median"], s["min"], s["max"])

    lines = ["| Profile | Rate | Latency | Transport | Runs | Delivered % | On time % | Usable on time % | p50 ms | p99 ms | Verdicts p/f/i |",
             "|---|---:|---:|---|---:|---:|---:|---:|---:|---:|---|"]
    for t in table:
        v = t["verdicts"]
        lines.append("| %s | %d | %d | %s | %d | %s | %s | %s | %s | %s | %d/%d/%d |" % (
            t["profile"], t["rate"], t["latency_ms"], t["transport"], t["runs"], pct(t["delivered_fraction"]),
            pct(t["on_time_fraction"]), pct(t["usable_on_time_fraction"]), ms_(t["age_p50_ms"]),
            ms_(t["age_p99_ms"]), v["pass"], v["fail"], v["inconclusive"]))
    text = "\n".join(lines)
    print(text)
    failed = [r for r in rows if r["overall"] == "fail"]
    inconclusive = [r for r in rows if r["overall"] == "inconclusive"]
    print("\nruns %d: pass %d, fail %d, inconclusive %d" % (
        len(rows), len(rows) - len(failed) - len(inconclusive), len(failed), len(inconclusive)))
    for r in failed + inconclusive:
        print("  %-12s %s: %s" % (r["overall"], r["run"], r["reasons"]))
    for name in inconsistent:
        print("  INCONSISTENT stored verdict: " + name)
    if args.summary_dir:
        os.makedirs(args.summary_dir, exist_ok=True)
        with open(os.path.join(args.summary_dir, "summary.json"), "w") as f:
            json.dump({"table": table, "runs": rows}, f, indent=1)
        with open(os.path.join(args.summary_dir, "summary.md"), "w") as f:
            f.write("Median (minimum-maximum) over the runs of each configuration.\n\n" + text + "\n")
        env = os.path.join(args.results, "environment.json")
        if os.path.exists(env):
            with open(os.path.join(args.summary_dir, "environment.json"), "w") as f:
                f.write(open(env).read())
    if inconsistent:
        return 2
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
