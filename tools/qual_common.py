# SPDX-License-Identifier: GPL-3.0-or-later
"""Definitions shared by tools/run_matrix.py and tools/validate_results.py.

Everything that decides a verdict lives here, so the runner and the
validator cannot disagree. The definitions are fixed in docs/benchmarking.md
before any headline run.
"""

import json
import math
import os

WINDOW = 1024              # samples per analysis window
DEADLINE_MARGIN_MS = 150   # deadline = requested latency + this
RECOVERY_BUDGET_S = 10.0   # profile N5
PER_RECORD = {"cf32": 138, "f32": 276}
QUEUE_MAX_RECORDS = 8192
REQUIRED_FILES = ("manifest.json", "sender.jsonl", "receiver.jsonl", "network.json",
                  "metrics.json", "verdict.json")
PRIMARY = {"rate": 100000, "latency_ms": 250, "profiles": ("N0", "N1", "N2")}


def offered_wire_bits_per_second(rate, fmt="cf32"):
    """Upper bound of the wire bit rate: payload, RWSR headers, and 44 bytes
    of SRT, UDP and IPv4 header per record."""
    sample_bytes = 8 if fmt == "cf32" else 4
    records = math.ceil(rate / PER_RECORD[fmt]) + 501  # full records plus flush records
    return (rate * sample_bytes + records * (96 + 44)) * 8


def read_last_json(path):
    try:
        with open(path) as f:
            lines = [l for l in f.read().splitlines() if l.startswith("{")]
        return json.loads(lines[-1]) if lines else None
    except OSError:
        return None


def judge(run_dir):
    """Derives the verdict of one run directory from its recorded evidence.

    Returns {"gates": {name: {"result": pass|fail|inconclusive, "mandatory":
    bool, "reason": str}}, "overall": ...}. Missing evidence is never a pass.
    """
    gates = {}

    def gate(name, result, mandatory, reason=""):
        gates[name] = {"result": result, "mandatory": mandatory, "reason": reason}

    missing = [f for f in REQUIRED_FILES if f != "verdict.json" and
               not os.path.exists(os.path.join(run_dir, f))]
    if missing:
        gate("evidence_present", "fail", True, "missing: " + ", ".join(missing))
        return {"gates": gates, "overall": "fail"}
    gate("evidence_present", "pass", True)

    manifest = json.load(open(os.path.join(run_dir, "manifest.json")))
    metrics = json.load(open(os.path.join(run_dir, "metrics.json")))
    tx = read_last_json(os.path.join(run_dir, "sender.jsonl"))
    rx = read_last_json(os.path.join(run_dir, "receiver.jsonl"))
    cfg = manifest["config"]
    transport, profile = cfg["transport"], cfg["profile"]
    is_srt = transport == "srt"

    if tx is None or rx is None or manifest.get("hung"):
        gate("processes_ended", "fail", True, "a peer hung or printed no result")
        return {"gates": gates, "overall": "fail"}
    gate("processes_ended", "pass", True)

    gate("no_altered_samples", "pass" if rx["samples_altered"] == 0 else "fail", True,
         "%d altered" % rx["samples_altered"])

    total = cfg["samples"]
    if rx["end_seen"]:
        first = rx["first_index"] if rx["samples_received"] else 0
        accounted = first + rx["samples_received"] + rx["missing_known_samples"]
        allowed_unknown = 1 if first > 0 else 0
        ok = accounted == total and rx["unknown_breaks"] <= allowed_unknown and rx["end_index"] == total
        gate("samples_accounted", "pass" if ok else "fail", True,
             "first %d + received %d + reported missing %d = %d of %d" %
             (first, rx["samples_received"], rx["missing_known_samples"], accounted, total))
    else:
        # Without END the tail is unknown. That is an expected outcome for a
        # transport without retransmission, and for any transport when the
        # run ends in a failure; it is never counted as accounted.
        gate("samples_accounted", "inconclusive", is_srt and profile in ("N0", "N1", "N2"),
             "END not observed: " + str(rx["session"].get("failure")))

    if profile == "N0":
        ok = (metrics["missing_samples"] == 0 and rx["session"]["end"] == "complete" and
              rx["samples_received"] == total)
        gate("n0_complete_and_exact", "pass" if ok else "fail", is_srt,
             "missing %d, end %s" % (metrics["missing_samples"], rx["session"]["end"]))

    primary = (is_srt and profile in PRIMARY["profiles"] and cfg["rate"] == PRIMARY["rate"] and
               cfg["latency_ms"] == PRIMARY["latency_ms"])
    if is_srt and profile in ("N0", "N1", "N2"):
        latency = rx["session"]["effective"]["rcv_latency_ms"]
        budget = latency + DEADLINE_MARGIN_MS
        p99 = metrics["age_ms"]["p99"]
        if p99 is None:
            gate("p99_age_within_budget", "inconclusive", primary, "no samples in the window")
        else:
            gate("p99_age_within_budget", "pass" if p99 <= budget else "fail", primary,
                 "p99 %.1f ms, budget %d ms (effective latency %d + %d)" %
                 (p99, budget, latency, DEADLINE_MARGIN_MS))

    if profile in ("N4", "N5"):
        hw = max(tx["queue"]["high_water_records"], rx["queue"]["high_water_records"])
        gate("queues_bounded", "pass" if hw <= QUEUE_MAX_RECORDS else "fail", True,
             "high water %d records" % hw)
    if profile == "N5":
        rec = metrics.get("recovery_s")
        if rec is None:
            gate("recovery_within_budget", "fail", is_srt, "no on-time output after restoration")
        else:
            gate("recovery_within_budget", "pass" if rec <= RECOVERY_BUDGET_S else "fail", is_srt,
                 "%.2f s after restoration" % rec)

    results = [g["result"] for g in gates.values() if g["mandatory"]]
    overall = "fail" if "fail" in results else "inconclusive" if "inconclusive" in results else "pass"
    return {"gates": gates, "overall": overall}
