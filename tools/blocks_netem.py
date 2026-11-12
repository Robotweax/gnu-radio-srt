#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Runs the two GNU Radio blocks, with AES-CTR, across netem paths.

The M4 matrix measured the transport with the reference peers in plaintext.
This check runs the released artefacts instead: examples/headless_sender.py
and examples/headless_receiver.py (srt_sink and srt_source in real
flowgraphs), encrypted, in segmented mode, through the same namespaces and
profiles. It answers one question per run: was every sample either delivered
or reported as missing (as a gap of known size, as the late join at the
start, or, when the END record itself is lost, as an end the receiver
declares unknown)? It does not measure age or timeliness.

Run inside a privileged, disposable container with the module installed and
importable (tools/dev/blocks-netem.sh does that):

  blocks_netem.py --out DIR [--seconds 60] [--profiles N0,N1,N2] [--reps 1]

Writes DIR/<profile>_rep<i>/{sender.jsonl,receiver.jsonl,receiver.events.jsonl,
run.json} and DIR/summary.json. Exits nonzero if any gate of any run fails.
"""

import argparse
import json
import os
import secrets
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qual_common as qc  # noqa: E402
import run_matrix as rm  # noqa: E402

EXAMPLES = os.path.join(rm.ROOT, "examples")
RATE = 100000
LATENCY_MS = 250
RECONNECT_BUDGET_S = 60  # the blocks' default reconnect_total_timeout_ms


def last_json(path):
    lines = [l for l in open(path).read().splitlines() if l.startswith("{")]
    return json.loads(lines[-1]) if lines else None


def run_one(profile_name, rep, slot, seconds, out_root):
    name = "%s_rep%d" % (profile_name, rep)
    out = os.path.join(out_root, name)
    os.makedirs(out, exist_ok=True)
    profile = rm.PROFILES["profiles"][profile_name]
    wire = qc.offered_wire_bits_per_second(RATE)
    s, x, r = "rwq%ds" % slot, "rwq%dx" % slot, "rwq%dr" % slot
    rx_ip = "10.%d.2.2" % (100 + slot)
    # A fresh random passphrase per run, passed through the environment only
    # and never written to the evidence.
    passphrase = secrets.token_hex(16)
    env = dict(os.environ, RWSRT_PASSPHRASE=passphrase)
    run = {"name": name, "profile": profile_name, "rate": RATE, "latency_ms": LATENCY_MS,
           "seconds": seconds, "security": "ctr", "policy": "segmented_live",
           "secrets": "random passphrase per run, environment only, not recorded"}
    try:
        run["topology"] = rm.sh([rm.TOPOLOGY, "up", str(slot)]).strip()
        fq = rm.forward_queue_packets(profile, wire)
        run["forward_netem"] = rm.apply_netem(x, "xr", profile["forward"], wire, fq)
        run["reverse_netem"] = rm.apply_netem(x, "xs", profile["reverse"], wire)
        common = ["--port", "9000", "--rate", str(RATE), "--latency-ms", str(LATENCY_MS),
                  "--policy", "segmented_live"]
        rx = subprocess.Popen(
            ["ip", "netns", "exec", r, sys.executable, os.path.join(EXAMPLES, "headless_receiver.py"),
             "--role", "listener", "--bind", rx_ip] + common,
            stdout=open(os.path.join(out, "receiver.jsonl"), "w"),
            stderr=open(os.path.join(out, "receiver.events.jsonl"), "w"), env=env)
        tx = subprocess.Popen(
            ["ip", "netns", "exec", s, sys.executable, os.path.join(EXAMPLES, "headless_sender.py"),
             "--role", "caller", "--host", rx_ip, "--seconds", str(seconds)] + common,
            stdout=open(os.path.join(out, "sender.jsonl"), "w"),
            stderr=open(os.path.join(out, "sender.err"), "w"), env=env)
        hung = False
        try:
            run["sender_exit"] = tx.wait(timeout=seconds + 60)
            # If END is lost the Source cannot know that the run is over. It
            # waits for the sender to come back until its reconnect budget
            # (60 s by default) ends, then reports the end as unknown.
            run["receiver_exit"] = rx.wait(timeout=RECONNECT_BUDGET_S + 30)
        except subprocess.TimeoutExpired:
            hung = True
            for p in (tx, rx):
                p.kill()
                p.wait()
        run["hung"] = hung
    finally:
        rm.sh([rm.TOPOLOGY, "down", str(slot)], check=False)

    for f in os.listdir(out):
        assert passphrase not in open(os.path.join(out, f), errors="replace").read(), "passphrase leaked into " + f

    tx_st = last_json(os.path.join(out, "sender.jsonl"))
    rx_st = last_json(os.path.join(out, "receiver.jsonl"))
    gates = {"processes_ended": not run["hung"]}
    figures = {}
    if tx_st and rx_st and not run["hung"]:
        sent = tx_st["input_samples_consumed"]
        delivered = rx_st["consumer"]["samples"]
        missing = rx_st["reported_missing_samples"]
        figures = {
            "sent": sent, "delivered": delivered, "reported_missing": missing,
            "delivered_fraction": delivered / sent if sent else 0.0,
            "discontinuity_tags": rx_st["discontinuity_tags"],
            "contiguous_stretches": rx_st["consumer"]["contiguous_stretches"],
            "unknown_breaks": rx_st["unknown_breaks"],
            "end": rx_st["session"]["end"],
            "termination_unknown": rx_st["session"]["termination_unknown"],
            "sender_end_acknowledged_by_transport": tx_st["session"]["end_acknowledged_by_transport"],
            "sender_failure": tx_st["session"]["failure"],
            "receiver_failure": rx_st["session"]["failure"],
            "connections": rx_st["session"]["counters"]["connections"],
            "key_length": rx_st["session"]["effective"]["key_length"],
            "link_retransmitted_seen_by_sender": tx_st["session"]["link"]["retransmitted"],
            "undecrypted": rx_st["session"]["link"]["undecrypted"],
        }
        # In segmented mode the Sink does not wait for the connection: what
        # it consumed before the path was up has expired, and the Source
        # joins the run at a later sample and says so with an initial_join
        # tag that carries the index of its first sample.
        joined_at = 0
        unknown_other = figures["unknown_breaks"]
        for line in open(os.path.join(out, "receiver.events.jsonl")):
            if not line.startswith("{"):
                continue
            tag = json.loads(line).get("tag", {})
            if "initial_join" in tag.get("reasons", "") and not tag.get("has_previous", True):
                joined_at = tag["next"]["sample_index"]
                unknown_other -= 1
        figures["joined_at_sample"] = joined_at
        figures["sender_expired_samples"] = tx_st["tx_queue"]["dropped_expired_samples"]
        gates["sent_expected_count"] = sent == RATE * seconds
        # END is one unrepeated packet and can be lost like any other. The
        # Source must then say that it does not know how the run ended; that
        # is a reported outcome, not a silent one.
        end_lost = (figures["end"] == "none" and figures["termination_unknown"]
                    and figures["receiver_failure"] == "reconnect_timeout")
        figures["end_lost"] = end_lost
        gates["sender_no_failure"] = figures["sender_failure"] == "none"
        gates["end_observed_or_reported_unknown"] = (
            (figures["end"] in ("complete", "incomplete") and figures["receiver_failure"] == "none") or end_lost)
        # The claim under test: nothing vanishes silently. Without END the
        # receiver cannot know the size of a missing tail; the runner can,
        # and records it.
        accounted = joined_at + delivered + missing
        figures["unreported_tail_samples"] = sent - accounted
        gates["every_sample_delivered_or_reported"] = unknown_other == 0 and (
            accounted == sent or (end_lost and accounted <= sent))
        # One tag per break, and the consumer saw each of them.
        gates["tags_match_stretches"] = (
            figures["contiguous_stretches"] == figures["discontinuity_tags"] + 1)
        gates["encrypted"] = rx_st["config"]["security_mode"] == "ctr" and figures["undecrypted"] == 0
        if profile_name == "N0":
            gates["clean_path_exact"] = delivered == sent and figures["end"] == "complete"
    else:
        gates["status_available"] = False
    run["figures"] = figures
    run["gates"] = gates
    run["verdict"] = "pass" if all(gates.values()) else "fail"
    json.dump(run, open(os.path.join(out, "run.json"), "w"), indent=2)
    return run


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    ap.add_argument("--seconds", type=int, default=60)
    ap.add_argument("--profiles", default="N0,N1,N2")
    ap.add_argument("--reps", type=int, default=1)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    runs = []
    for rep in range(args.reps):
        for i, profile in enumerate(args.profiles.split(",")):
            run = run_one(profile, rep, i, args.seconds, args.out)
            runs.append(run)
            f = run["figures"]
            print("%s %s joined_at=%s delivered=%s missing=%s tags=%s end=%s end_lost=%s failed_gates=%s" % (
                run["name"], run["verdict"], f.get("joined_at_sample"), f.get("delivered"), f.get("reported_missing"),
                f.get("discontinuity_tags"), f.get("end"), f.get("end_lost"),
                [k for k, v in run["gates"].items() if not v]), flush=True)
    summary = {"runs": [{k: r[k] for k in ("name", "profile", "seconds", "forward_netem", "reverse_netem",
                                             "figures", "gates", "verdict") if k in r} for r in runs],
               "overall": "pass" if all(r["verdict"] == "pass" for r in runs) else "fail"}
    json.dump(summary, open(os.path.join(args.out, "summary.json"), "w"), indent=2)
    print("overall", summary["overall"])
    return 0 if summary["overall"] == "pass" else 1


if __name__ == "__main__":
    sys.exit(main())
