#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Long-run check of the two blocks (plan section 12 B: 30-minute run without
leaked threads or sockets and without unbounded memory growth).

Runs inside the development image against the installed module:

  tools/dev/run.sh tools/dev/soak.sh [seconds]        default 1800

Two runs in parallel, each a sender and a receiver process using the
headless examples at 100 kS/s cf32, AES-CTR:

  clean     strict mode, direct. Must be receiver-observed complete with the
            exact sample count.
  outages   segmented mode through the fault proxy, a 7 s outage every 120 s
            (longer than the 5 s idle timeout, so both ends reconnect). Every
            sample must be delivered or reported missing.

Resource criteria, fixed before the first run. Samples are taken every 10 s.
"Early" is the window from 2 to 7 minutes, "late" the last 5 minutes.
  threads   max(late) <= max(early)
  fds       max(late) <= max(early)
  memory    median RSS(late) <= median RSS(early) + 16 MiB
Runs shorter than 12 minutes cannot separate the windows; they report the
resource criteria as "inconclusive" and are for checking the harness only.

Writes artifacts/soak/: samples.csv, the final status of every process, and
verdict.json. Exit status 0 only if every criterion passes.
"""

import json
import os
import signal
import statistics
import subprocess
import sys
import time

RATE = 100000
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
OUT = os.path.join(ROOT, "artifacts", "soak")
SECRET_ENV = "RWSRT_PASSPHRASE"


def proc_sample(pid):
    try:
        with open("/proc/%d/status" % pid) as f:
            status = f.read()
        fds = len(os.listdir("/proc/%d/fd" % pid))
    except OSError:
        return None
    fields = dict(line.split(":", 1) for line in status.splitlines() if ":" in line)
    return dict(rss_kib=int(fields["VmRSS"].split()[0]), threads=int(fields["Threads"]), fds=fds)


def spawn(name, argv):
    out = open(os.path.join(OUT, name + ".jsonl"), "w")
    err = open(os.path.join(OUT, name + ".err"), "w")
    return subprocess.Popen(argv, stdout=out, stderr=err)


def last_json(name):
    with open(os.path.join(OUT, name + ".jsonl")) as f:
        lines = [l for l in f.read().splitlines() if l.startswith("{")]
    return json.loads(lines[-1]) if lines else None


def resources(samples, seconds):
    verdict = {}
    if seconds < 720:
        return {"threads": "inconclusive", "fds": "inconclusive", "memory": "inconclusive",
                "reason": "run shorter than 12 minutes"}
    early = [s for s in samples if 120 <= s["t"] <= 420]
    late = [s for s in samples if s["t"] >= seconds - 300]
    for key in ("threads", "fds"):
        verdict[key] = "pass" if max(s[key] for s in late) <= max(s[key] for s in early) else "fail"
        verdict[key + "_early_max"] = max(s[key] for s in early)
        verdict[key + "_late_max"] = max(s[key] for s in late)
    e = statistics.median(s["rss_kib"] for s in early)
    l = statistics.median(s["rss_kib"] for s in late)
    verdict["memory"] = "pass" if l <= e + 16 * 1024 else "fail"
    verdict["rss_early_median_kib"] = e
    verdict["rss_late_median_kib"] = l
    verdict["rss_max_kib"] = max(s["rss_kib"] for s in samples)
    return verdict


def main():
    seconds = int(sys.argv[1]) if len(sys.argv) > 1 else 1800
    os.makedirs(OUT, exist_ok=True)
    os.environ[SECRET_ENV] = "soak-run-passphrase-not-a-secret"
    total = RATE * seconds
    ex = os.path.join(ROOT, "examples")
    py = sys.executable

    procs = {}
    procs["clean_rx"] = spawn("clean_rx", [py, ex + "/headless_receiver.py", "--port", "9301"])
    outages = []
    t = 60
    while t + 30 < seconds:
        outages += ["--outage", "%d:7" % t]
        t += 120
    proxy = subprocess.Popen([py, "-I", os.path.join(ROOT, "tools", "impair", "udp_proxy.py"),
                              "--listen", "127.0.0.1:9303", "--target", "127.0.0.1:9302"] + outages,
                             stdout=subprocess.PIPE, text=True)
    proxy.stdout.readline()
    procs["outage_rx"] = spawn("outage_rx", [py, ex + "/headless_receiver.py", "--port", "9302",
                                             "--policy", "segmented_live"])
    time.sleep(1.0)
    procs["clean_tx"] = spawn("clean_tx", [py, ex + "/headless_sender.py", "--port", "9301",
                                           "--seconds", str(seconds)])
    procs["outage_tx"] = spawn("outage_tx", [py, ex + "/headless_sender.py", "--port", "9303",
                                             "--seconds", str(seconds), "--policy", "segmented_live"])

    samples = {name: [] for name in procs}
    t0 = time.monotonic()
    with open(os.path.join(OUT, "samples.csv"), "w") as csv:
        csv.write("t,process,rss_kib,threads,fds\n")
        while any(p.poll() is None for p in procs.values()):
            if time.monotonic() - t0 > seconds + 120:
                break  # a hang is a failure, reported below
            time.sleep(10 if seconds >= 300 else 2)
            now = time.monotonic() - t0
            for name, p in procs.items():
                s = proc_sample(p.pid) if p.poll() is None else None
                if s:
                    s["t"] = round(now, 1)
                    samples[name].append(s)
                    csv.write("%.1f,%s,%d,%d,%d\n" % (now, name, s["rss_kib"], s["threads"], s["fds"]))
            csv.flush()
    hung = [n for n, p in procs.items() if p.poll() is None]
    for n in hung:
        procs[n].kill()
    proxy.send_signal(signal.SIGTERM)
    proxy_counters = json.loads(proxy.communicate(timeout=10)[0].splitlines()[-1])

    verdict = {"seconds": seconds, "samples_expected": total, "hung": hung, "proxy": proxy_counters,
               "exit": {n: p.returncode for n, p in procs.items()}, "resources": {}}
    for name in procs:
        verdict["resources"][name] = resources(samples[name], seconds) if samples[name] else {"error": "no samples"}

    gates = {}
    ctx, crx = last_json("clean_tx"), last_json("clean_rx")
    gates["clean_complete"] = bool(
        ctx and crx and not hung and ctx["input_samples_consumed"] == total and
        crx["consumer"]["samples"] == total and crx["session"]["end"] == "complete" and
        crx["consumer"]["contiguous_stretches"] == 1 and crx["discontinuity_tags"] == 0 and
        ctx["session"]["completed"] and procs["clean_tx"].returncode == 0 and procs["clean_rx"].returncode == 0)
    otx, orx = last_json("outage_tx"), last_json("outage_rx")
    if otx and orx:
        with open(os.path.join(OUT, "outage_rx.err")) as f:
            events = [json.loads(l) for l in f.read().splitlines() if l.startswith("{\"event\"")]
        first = 0
        if events and "initial_join" in events[0]["tag"]["reasons"]:
            first = events[0]["tag"]["next"]["sample_index"]
        accounted = first + orx["consumer"]["samples"] + orx["reported_missing_samples"]
        verdict["outages"] = {
            "first_index": first, "received": orx["consumer"]["samples"],
            "reported_missing": orx["reported_missing_samples"], "accounted": accounted,
            "unknown_breaks": orx["unknown_breaks"], "discontinuities": len(events),
            "connections": orx["session"]["counters"]["connections"],
            "sender_connections": otx["session"]["counters"]["connections"],
            "end": orx["session"]["end"], "sender_failure": otx["session"]["failure"],
            "receiver_failure": orx["session"]["failure"],
            "sender_queue_high_water_records": otx["tx_queue"]["high_water_records"],
            "receiver_queue_high_water_records": orx["rx_queue"]["high_water_records"],
        }
        gates["outages_accounted"] = (not hung and accounted == total and
                                      otx["input_samples_consumed"] == total and
                                      orx["unknown_breaks"] == (1 if first else 0) and
                                      orx["end_emitted"] and otx["session"]["completed"])
        gates["outages_reconnected"] = (orx["session"]["counters"]["connections"] == len(outages) // 2 + 1
                                        if seconds >= 120 else True)
        gates["queues_bounded"] = (otx["tx_queue"]["high_water_records"] <= 8192 and
                                   orx["rx_queue"]["high_water_records"] <= 8192)
    else:
        gates["outages_accounted"] = False
    for name, res in verdict["resources"].items():
        for key in ("threads", "fds", "memory"):
            gates["%s_%s" % (name, key)] = res.get(key, "fail")
    verdict["gates"] = {k: (v if isinstance(v, str) else ("pass" if v else "fail")) for k, v in gates.items()}
    results = set(verdict["gates"].values())
    verdict["overall"] = "fail" if "fail" in results else ("inconclusive" if "inconclusive" in results else "pass")
    for name in procs:  # keep only the final status line of each process
        final = last_json(name)
        with open(os.path.join(OUT, name + ".final.json"), "w") as f:
            json.dump(final, f, indent=1)
        os.remove(os.path.join(OUT, name + ".jsonl"))
    with open(os.path.join(OUT, "verdict.json"), "w") as f:
        json.dump(verdict, f, indent=2)
    print(json.dumps({"overall": verdict["overall"], "gates": verdict["gates"]}))
    return 0 if verdict["overall"] == "pass" else 1


if __name__ == "__main__":
    sys.exit(main())
