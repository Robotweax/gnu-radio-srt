#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""End-to-end tests of the headless reference peers over loopback SRT.

Each case starts a receiver and/or a sender as separate processes, reads the
JSON object each prints on exit, and checks it. No network impairment here:
that is milestone M4. Writes one JSON line per case to --evidence if given.
"""

import argparse
import json
import os
import signal
import socket
import subprocess
import sys
import time

PASSPHRASE_ENV = "RWSRT_TEST_PASSPHRASE"
SECRETS = {"good": "e2e-test-passphrase-A", "other": "e2e-test-passphrase-B"}


def free_udp_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Peer:
    def __init__(self, binary, args, secret=None):
        env = dict(os.environ)
        env.pop(PASSPHRASE_ENV, None)
        argv = [binary] + ["--%s=%s" % (k, v) if v is not True else "--" + k for k, v in args.items()]
        if secret is None:
            argv.append("--plaintext")
        else:
            env[PASSPHRASE_ENV] = SECRETS[secret]
            argv.append("--passphrase-env=" + PASSPHRASE_ENV)
        self.argv = argv
        self.started = time.monotonic()
        self.proc = subprocess.Popen(argv, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

    def finish(self, timeout=60):
        try:
            out, err = self.proc.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            out, err = self.proc.communicate()
            raise AssertionError("peer hung: %s\nstderr: %s" % (" ".join(self.argv), err[-2000:]))
        self.wall_s = time.monotonic() - self.started
        self.code = self.proc.returncode
        self.stderr = err
        lines = [l for l in out.splitlines() if l.startswith("{")]
        if not lines:
            raise AssertionError("no JSON from %s (exit %s)\nstderr: %s" % (self.argv[0], self.code, err[-2000:]))
        self.json = json.loads(lines[-1])
        # No secret may appear in any output.
        for secret in SECRETS.values():
            assert secret not in out and secret not in err, "passphrase leaked into output"
        return self.json


class Suite:
    def __init__(self, sender, receiver, evidence):
        self.sender_bin, self.receiver_bin = sender, receiver
        self.evidence = open(evidence, "w") if evidence else None
        self.failures = []

    def sender(self, secret=None, **args):
        return Peer(self.sender_bin, {k.replace("_", "-"): v for k, v in args.items()}, secret)

    def receiver(self, secret=None, **args):
        return Peer(self.receiver_bin, {k.replace("_", "-"): v for k, v in args.items()}, secret)

    def run(self, case):
        name = case.__name__
        t0 = time.monotonic()
        record = {"case": name}
        try:
            record.update(case(self) or {})
            record["result"] = "pass"
        except AssertionError as e:
            record["result"] = "fail"
            record["error"] = str(e)[:1500]
            self.failures.append(name)
        record["wall_s"] = round(time.monotonic() - t0, 3)
        print("%-44s %s  %.2fs%s" % (name, record["result"].upper(), record["wall_s"],
                                     "" if record["result"] == "pass" else "\n    " + record["error"]), flush=True)
        if self.evidence:
            self.evidence.write(json.dumps(record, sort_keys=True) + "\n")
            self.evidence.flush()


def check(cond, message):
    if not cond:
        raise AssertionError(message)


def summary(tx=None, rx=None):
    out = {}
    if tx is not None:
        s = tx.json["session"]
        out["sender"] = {"exit": tx.code, "samples_produced": tx.json["samples_produced"],
                         "state": s["state"], "failure": s["failure"], "completed": s["completed"],
                         "stop_ms": tx.json["stop_ms"], "queue": tx.json["queue"],
                         "counters": s["counters"], "effective": s["effective"], "link": s["link"]}
    if rx is not None:
        s = rx.json["session"]
        out["receiver"] = {"exit": rx.code,
                           **{k: rx.json[k] for k in ("samples_received", "samples_altered", "records",
                                                      "first_index", "end_seen", "end_index", "discontinuities",
                                                      "reasons", "missing_known_samples", "unknown_breaks",
                                                      "queue_drop_breaks", "stop_ms", "queue")},
                           "state": s["state"], "failure": s["failure"], "failure_detail": s["failure_detail"],
                           "end": s["end"], "termination_unknown": s["termination_unknown"],
                           "completed": s["completed"], "continuity": s["continuity"],
                           "counters": s["counters"], "effective": s["effective"]}
    return out


def complete_transfer(suite, fmt, sink_role, secret, samples, rate="200000", policy="strict", **extra):
    """Finite run: every sample must arrive, in order, unaltered, with END."""
    port = free_udp_port()
    source_role = "listener" if sink_role == "caller" else "caller"
    rx = suite.receiver(secret, role=source_role, port=port, format=fmt, rate=rate, policy=policy,
                        timeout_ms=30000, **extra)
    if source_role == "listener":
        time.sleep(0.2)  # let the listener bind before the caller's first attempt
    tx = suite.sender(secret, role=sink_role, port=port, format=fmt, rate=rate, policy=policy,
                      samples=samples, **extra)
    tx.finish()
    rx.finish()
    out = summary(tx, rx)
    r, t = rx.json, tx.json
    check(tx.code == 0, "sender exit %d: %s" % (tx.code, t["session"]["failure_detail"]))
    check(rx.code == 0, "receiver exit %d: %s" % (rx.code, r["session"]["failure_detail"]))
    check(t["samples_produced"] == samples, "sender produced %d" % t["samples_produced"])
    check(r["samples_received"] == samples, "received %d of %d" % (r["samples_received"], samples))
    check(r["samples_altered"] == 0, "%d samples altered" % r["samples_altered"])
    check(r["discontinuities"] == 0, "unexpected discontinuity: " + r["reasons"])
    check(r["end_seen"] and r["end_index"] == samples, "END index %d" % r["end_index"])
    check(r["session"]["end"] == "complete", "end is " + r["session"]["end"])
    check(not r["session"]["termination_unknown"], "termination_unknown set")
    check(r["session"]["continuity"]["known_missing_samples"] == 0, "missing samples")
    check(t["session"]["counters"]["end_sent"] == 1, "END not sent exactly once")
    check(t["session"]["counters"]["samples_send_accepted"] == samples, "send_accepted mismatch")
    # Socket contract as read back from the library.
    for side in (t, r):
        eff = side["session"]["effective"]
        check(eff["payload_size"] == 1200, "payload size %d" % eff["payload_size"])
        check(eff["km_state"] == (2 if secret else 0), "km_state %d" % eff["km_state"])
        check(side["runtime_leases_after_stop"] == 0, "SRT runtime lease leaked")
        check(side["stop_ms"] < 1000, "stop took %.0f ms" % side["stop_ms"])
        check(side["library_log_messages"] >= 0, "log counter missing")
    check(r["session"]["effective"]["rcv_latency_ms"] == 250, "receiver latency")
    check(t["session"]["effective"]["max_bw"] == 0 and t["session"]["effective"]["input_bw"] > 0,
          "sender bandwidth not derived from the declared rate")
    check(not tx.stderr.strip() and not rx.stderr.strip(), "unexpected stderr output")
    return out


# ------------------------------------------------------------------ cases

def cf32_ctr_sink_caller(suite):
    # 100001 samples: 724 full records and one partial final record.
    return complete_transfer(suite, "cf32", "caller", "good", 100001)


def f32_plaintext_sink_listener(suite):
    return complete_transfer(suite, "f32", "listener", None, 100001)


def cf32_plaintext_sink_caller(suite):
    return complete_transfer(suite, "cf32", "caller", None, 50000)


def f32_ctr_sink_listener(suite):
    return complete_transfer(suite, "f32", "listener", "good", 50000)


def rational_rate_segmented(suite):
    # 1000000/7 samples per second; segmented mode with a clean start.
    port = free_udp_port()
    rx = suite.receiver("good", role="listener", port=port, rate="1000000/7", policy="segmented", timeout_ms=30000)
    time.sleep(0.2)
    tx = suite.sender("good", role="caller", port=port, rate="2000000/14", policy="segmented", samples=40000)
    tx.finish()
    rx.finish()
    r = rx.json
    check(tx.code == 0 and rx.code == 0, "exit %d/%d" % (tx.code, rx.code))
    check(r["samples_altered"] == 0, "altered samples")
    # The sender starts producing before the connection exists, so the
    # receiver may join mid-run; whatever arrived must be accounted for.
    total = r["samples_received"] + r["missing_known_samples"]
    check(r["unknown_breaks"] > 0 or total == 40000, "samples unaccounted: %d" % total)
    check(r["end_seen"] and r["end_index"] == 40000, "END index")
    return summary(tx, rx)


def empty_run(suite):
    out = complete_transfer(suite, "cf32", "caller", "good", 0)
    check(out["receiver"]["records"] == 0, "records in an empty run")
    return out


def single_sample_run(suite):
    return complete_transfer(suite, "f32", "caller", None, 1)


def small_record_limit(suite):
    # 96 + 8: one cf32 sample per record, so framing dominates. A low rate on
    # purpose: this case is about framing, not about records per second.
    port = free_udp_port()
    rx = suite.receiver(None, role="listener", port=port, rate="2000", record_limit=104, timeout_ms=30000)
    time.sleep(0.2)
    tx = suite.sender(None, role="caller", port=port, rate="2000", record_limit=104, samples=1500)
    tx.finish()
    rx.finish()
    check(tx.code == 0 and rx.code == 0, "exit %d/%d" % (tx.code, rx.code))
    check(rx.json["samples_received"] == 1500 and rx.json["records"] == 1500, "sample/record count")
    check(rx.json["samples_altered"] == 0 and rx.json["session"]["end"] == "complete", "not complete")
    check(rx.json["session"]["effective"]["payload_size"] == 104, "payload size readback")
    return summary(tx, rx)


def wrong_key_delivers_nothing(suite):
    port = free_udp_port()
    rx = suite.receiver("good", role="listener", port=port, startup_timeout_ms=2500, timeout_ms=20000)
    time.sleep(0.2)
    tx = suite.sender("other", role="caller", port=port, samples=10000, startup_timeout_ms=2500)
    tx.finish()
    rx.finish()
    check(tx.code == 2 and tx.json["session"]["failure"] == "credentials",
          "sender: exit %d failure %s" % (tx.code, tx.json["session"]["failure"]))
    check(tx.wall_s < 2.0, "credential failure was retried for %.1fs" % tx.wall_s)
    check(tx.json["samples_produced"] == 0, "sender produced samples without a peer")
    check(rx.json["samples_received"] == 0 and rx.json["session"]["counters"]["records_received"] == 0,
          "receiver got data with the wrong key")
    check(rx.json["session"]["failure"] == "startup_timeout", "receiver: " + rx.json["session"]["failure"])
    return summary(tx, rx)


def missing_key_delivers_nothing(suite):
    port = free_udp_port()
    rx = suite.receiver("good", role="listener", port=port, startup_timeout_ms=2500, timeout_ms=20000)
    time.sleep(0.2)
    tx = suite.sender(None, role="caller", port=port, samples=10000, startup_timeout_ms=2500)
    tx.finish()
    rx.finish()
    check(tx.json["session"]["failure"] == "credentials", "sender: " + tx.json["session"]["failure"])
    check(rx.json["samples_received"] == 0, "receiver got plaintext data")
    return summary(tx, rx)


def unexpected_key_is_not_downgraded(suite):
    # The receiver is explicit plaintext; an encrypting sender must fail,
    # and nobody falls back to plaintext.
    port = free_udp_port()
    rx = suite.receiver(None, role="listener", port=port, startup_timeout_ms=2500, timeout_ms=20000)
    time.sleep(0.2)
    tx = suite.sender("good", role="caller", port=port, samples=10000, startup_timeout_ms=2500)
    tx.finish()
    rx.finish()
    check(tx.json["session"]["failure"] == "credentials", "sender: " + tx.json["session"]["failure"])
    check(rx.json["samples_received"] == 0, "receiver got data")
    return summary(tx, rx)


def no_peer_caller_fails_at_startup_deadline(suite):
    tx = suite.sender("good", role="caller", port=free_udp_port(), samples=1000, startup_timeout_ms=1500,
                      connect_timeout_ms=500)
    tx.finish()
    s = tx.json["session"]
    check(tx.code == 2 and s["failure"] == "startup_timeout", "exit %d failure %s" % (tx.code, s["failure"]))
    check(1.4 < tx.wall_s < 4.0, "took %.2fs for a 1.5s startup timeout" % tx.wall_s)
    check(s["counters"]["connect_attempts"] >= 2, "attempts were not retried")
    check(tx.json["stop_ms"] < 1000, "stop took %.0f ms" % tx.json["stop_ms"])
    return summary(tx)


def no_peer_listener_fails_at_startup_deadline(suite):
    rx = suite.receiver("good", role="listener", port=free_udp_port(), startup_timeout_ms=1500, timeout_ms=20000)
    rx.finish()
    check(rx.code == 2 and rx.json["session"]["failure"] == "startup_timeout",
          "exit %d failure %s" % (rx.code, rx.json["session"]["failure"]))
    check(1.4 < rx.wall_s < 4.0, "took %.2fs" % rx.wall_s)
    return summary(None, rx)


def rate_mismatch_is_a_terminal_configuration_error(suite):
    port = free_udp_port()
    rx = suite.receiver(None, role="listener", port=port, rate="48000", timeout_ms=20000)
    time.sleep(0.2)
    tx = suite.sender(None, role="caller", port=port, rate="200000", samples=400000, policy="segmented",
                      no_reconnect=True)
    rx.finish()
    tx.finish()
    r = rx.json
    check(rx.code == 2 and r["session"]["failure"] == "configuration",
          "receiver exit %d failure %s" % (rx.code, r["session"]["failure"]))
    check(r["samples_received"] == 0, "samples were output despite the mismatch")
    return summary(tx, rx)


def format_mismatch_is_a_terminal_configuration_error(suite):
    port = free_udp_port()
    rx = suite.receiver(None, role="listener", port=port, format="f32", timeout_ms=20000)
    time.sleep(0.2)
    tx = suite.sender(None, role="caller", port=port, format="cf32", samples=400000, policy="segmented",
                      no_reconnect=True)
    rx.finish()
    tx.finish()
    check(rx.json["session"]["failure"] == "configuration", rx.json["session"]["failure"])
    check(rx.json["samples_received"] == 0, "samples were output despite the mismatch")
    return summary(tx, rx)


def stream_id_mismatch_is_rejected(suite):
    port = free_udp_port()
    rx = suite.receiver(None, role="listener", port=port, stream_id="rwsrt/v1/a", startup_timeout_ms=2500,
                        timeout_ms=20000)
    time.sleep(0.2)
    tx = suite.sender(None, role="caller", port=port, stream_id="rwsrt/v1/b", samples=20000)
    tx.finish()
    rx.finish()
    r = rx.json
    check(r["samples_received"] == 0, "data accepted from an unexpected stream id")
    check(r["session"]["counters"]["rejected_peers"] >= 1, "peer was not rejected")
    check(r["session"]["counters"]["connections"] == 0, "the peer got a connection")
    check(r["session"]["failure"] == "startup_timeout", r["session"]["failure"])
    # Refused inside the handshake, and terminal for the caller.
    t = tx.json["session"]
    check(t["failure"] == "configuration" and "stream_id" in t["failure_detail"], t["failure_detail"])
    check(t["counters"]["connections"] == 0 and tx.wall_s < 2.0, "caller connected or kept retrying")
    return summary(tx, rx)


def mid_run_join_segmented_and_strict(suite):
    """A sender already running; receivers join late."""
    out = {}
    for policy in ("segmented", "strict"):
        port = free_udp_port()
        # Segmented sender: produces from the start, with or without a peer.
        # The short reconnect budget only matters in the strict case, where the
        # receiver leaves and the sender would otherwise wait 60 s for another.
        tx = suite.sender(None, role="listener", port=port, rate="200000", samples=400000, policy="segmented",
                          reconnect_total_timeout_ms=1500)
        time.sleep(0.7)
        rx = suite.receiver(None, role="caller", port=port, rate="200000", policy=policy, timeout_ms=30000)
        rx.finish()
        tx.finish()
        r = rx.json
        check(r["samples_altered"] == 0, "altered samples")
        if policy == "segmented":
            check(rx.code == 0, "receiver exit %d" % rx.code)
            check("initial_join" in r["reasons"], "initial_join not reported: " + r["reasons"])
            check(r["first_index"] > 0, "claims to have seen the start")
            check(0 < r["samples_received"] < 400000, "received %d" % r["samples_received"])
            check(r["end_seen"] and r["end_index"] == 400000, "END index")
            # Joined mid-run: END was seen, but the run was not observed completely.
            check(r["session"]["end"] == "incomplete", "end is " + r["session"]["end"])
            check(r["first_index"] + r["samples_received"] + r["missing_known_samples"] -
                  0 == 400000 or r["unknown_breaks"] > 1, "samples after the join unaccounted")
        else:
            check(rx.code == 2 and r["session"]["failure"] == "continuity",
                  "strict receiver: exit %d failure %s" % (rx.code, r["session"]["failure"]))
            check(r["samples_received"] == 0, "strict receiver output samples after a mid-run join")
        out[policy] = summary(tx, rx)
    return out


def cancellation_is_not_completion(suite):
    """Sender stops without END: the receiver must not report success."""
    out = {}
    for policy in ("strict", "segmented"):
        port = free_udp_port()
        rx = suite.receiver(None, role="listener", port=port, rate="200000", policy=policy, timeout_ms=30000,
                            reconnect_total_timeout_ms=1500)
        time.sleep(0.2)
        tx = suite.sender(None, role="caller", port=port, rate="200000", policy="strict", cancel_after_ms=800)
        tx.finish()
        rx.finish()
        r = rx.json
        check(tx.code == 3 and tx.json["session"]["counters"]["end_sent"] == 0, "sender sent END on cancel")
        check(r["samples_received"] > 0 and r["samples_altered"] == 0, "no intact data before the cancel")
        check(not r["end_seen"] and r["session"]["end"] == "none", "END reported")
        check(r["session"]["termination_unknown"], "termination_unknown not set")
        check(not r["session"]["completed"] and rx.code == 2, "receiver reported completion")
        check(r["session"]["failure"] == ("connection_lost" if policy == "strict" else "reconnect_timeout"),
              policy + ": " + r["session"]["failure"])
        out[policy] = summary(tx, rx)
    return out


def segmented_sender_without_peer_stays_bounded(suite):
    """No receiver at all: queue and memory stay bounded, old data is discarded."""
    tx = suite.sender(None, role="caller", port=free_udp_port(), rate="1000000", policy="segmented",
                      cancel_after_ms=2000, queue_max_records=512, connect_timeout_ms=500)
    tx.finish()
    q = tx.json["queue"]
    check(tx.code == 3, "exit %d" % tx.code)
    check(tx.json["samples_produced"] > 1000000, "produced only %d" % tx.json["samples_produced"])
    check(q["high_water_records"] <= 512, "queue grew to %d records" % q["high_water_records"])
    check(q["high_water_bytes"] <= 512 * 1200, "queue grew to %d bytes" % q["high_water_bytes"])
    dropped = q["dropped_overflow_samples"] + q["dropped_expired_samples"]
    check(dropped > 0, "nothing was discarded")
    # Every produced sample is either still queued/partial or counted as dropped.
    check(dropped <= tx.json["samples_produced"], "drop accounting exceeds production")
    check(tx.json["samples_produced"] - dropped <= 512 * 138 + 138, "samples unaccounted")
    check(tx.json["stop_ms"] < 1000, "stop took %.0f ms" % tx.json["stop_ms"])
    return summary(tx)


def strict_receiver_stall_is_terminal_not_silent(suite):
    """A consumer that stops reading for longer than queue_max_age."""
    port = free_udp_port()
    rx = suite.receiver(None, role="listener", port=port, rate="200000", policy="strict", timeout_ms=30000,
                        stall_after_samples=20000, stall_ms=600)
    time.sleep(0.2)
    tx = suite.sender(None, role="caller", port=port, rate="200000", policy="strict", samples=400000)
    rx.finish()
    tx.finish()
    r = rx.json
    check(rx.code == 2 and r["session"]["failure"] == "queue_overflow",
          "exit %d failure %s" % (rx.code, r["session"]["failure"]))
    check(r["samples_altered"] == 0 and not r["session"]["completed"], "reported success")
    check(r["samples_received"] < 400000, "all samples output despite the stall")
    return summary(tx, rx)


def segmented_receiver_stall_reports_the_drop(suite):
    port = free_udp_port()
    rx = suite.receiver(None, role="listener", port=port, rate="200000", policy="segmented", timeout_ms=30000,
                        stall_after_samples=20000, stall_ms=600, events=True)
    time.sleep(0.2)
    tx = suite.sender(None, role="caller", port=port, rate="200000", policy="strict", samples=400000)
    tx.finish()
    rx.finish()
    r = rx.json
    check(tx.code == 0 and rx.code == 0, "exit %d/%d" % (tx.code, rx.code))
    check(r["samples_altered"] == 0, "altered samples")
    check(r["queue_drop_breaks"] >= 1 and "receiver_queue_drop" in r["reasons"], "drop not reported: " + r["reasons"])
    dropped = r["queue"]["dropped_overflow_samples"] + r["queue"]["dropped_expired_samples"]
    check(dropped > 0, "no samples were discarded during the stall")
    # Queue-drop metric and reported discontinuities agree, and nothing is unaccounted.
    check(r["missing_known_samples"] == dropped, "reported %d, queue dropped %d" % (r["missing_known_samples"], dropped))
    check(r["samples_received"] + dropped == 400000, "samples unaccounted")
    check(r["end_seen"] and r["end_index"] == 400000, "END index")
    events = [json.loads(l) for l in rx.stderr.splitlines() if l.startswith("{")]
    check(len(events) == r["discontinuities"], "event count")
    return summary(tx, rx)


def invalid_configuration_is_rejected_before_any_socket(suite):
    out = {}
    cases = {
        "record_limit_too_large": dict(record_limit=1457),
        "record_limit_too_small": dict(record_limit=96),
        "zero_rate": dict(rate="0"),
        "bad_port": dict(port=70000),
        "bad_host": dict(host="example.invalid"),
        "bad_latency": dict(latency_ms=5),
    }
    for name, extra in cases.items():
        args = dict(role="caller", port=free_udp_port(), samples=10)
        args.update(extra)
        tx = suite.sender(None, **args)
        out_s, err = tx.proc.communicate(timeout=10)
        check(tx.proc.returncode == 64, "%s: exit %d" % (name, tx.proc.returncode))
        check("configuration" in err, "%s: no explanation: %s" % (name, err))
        out[name] = err.strip()
    # CTR without a usable secret.
    env = dict(os.environ)
    env[PASSPHRASE_ENV] = "short"
    p = subprocess.run([suite.sender_bin, "--role=caller", "--samples=1", "--passphrase-env=" + PASSPHRASE_ENV],
                       env=env, capture_output=True, text=True, timeout=10)
    check(p.returncode == 64 and "passphrase" in p.stderr and "short" not in p.stderr, "short passphrase: " + p.stderr)
    env.pop(PASSPHRASE_ENV)
    p = subprocess.run([suite.sender_bin, "--role=caller", "--samples=1", "--passphrase-env=" + PASSPHRASE_ENV],
                       env=env, capture_output=True, text=True, timeout=10)
    check(p.returncode == 64 and "not set" in p.stderr, "missing variable: " + p.stderr)
    p = subprocess.run([suite.sender_bin, "--role=caller", "--samples=1"], capture_output=True, text=True, timeout=10)
    check(p.returncode == 64, "no security mode given: exit %d" % p.returncode)
    return out


PROXY = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools", "impair", "udp_proxy.py")
FAST = dict(peer_idle_timeout_ms=1000, connect_timeout_ms=500, reconnect_total_timeout_ms=15000)


class FaultProxy:
    def __init__(self, listener_port, *faults):
        self.port = free_udp_port()
        self.proc = subprocess.Popen([sys.executable, "-I", PROXY, "--listen", "127.0.0.1:%d" % self.port,
                                      "--target", "127.0.0.1:%d" % listener_port] + [str(f) for f in faults],
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        check("ready" in self.proc.stdout.readline(), "proxy did not start")

    def close(self):
        self.proc.send_signal(signal.SIGTERM)
        out, _ = self.proc.communicate(timeout=10)
        return json.loads([l for l in out.splitlines() if l.startswith("{")][-1])


def outage_reconnects_within_one_run(suite):
    """Outage longer than the idle timeout: both peers reconnect, the run
    continues, and every sample is delivered or reported missing."""
    port = free_udp_port()
    proxy = FaultProxy(port, "--outage", "1.0:2.5")
    n = 1200000  # 6 s at 200 kS/s
    rx = suite.receiver(None, role="listener", port=port, rate="200000", policy="segmented", timeout_ms=40000,
                        events=True, **FAST)
    time.sleep(0.2)
    tx = suite.sender(None, role="caller", port=proxy.port, rate="200000", policy="segmented", samples=n, **FAST)
    tx.finish()
    rx.finish()
    counters = proxy.close()
    r = rx.json
    check(tx.code == 0 and rx.code == 0, "exit %d/%d" % (tx.code, rx.code))
    check(counters["dropped_outage"] > 0, "the outage dropped nothing")
    check(r["samples_altered"] == 0, "altered samples")
    check("reconnect" in r["reasons"], "reconnect not reported: " + r["reasons"])
    check(r["session"]["counters"]["connections"] == 2, "connections %d" % r["session"]["counters"]["connections"])
    check(r["session"]["continuity"]["runs"] == 1, "run changed across the reconnect")
    events = [json.loads(l) for l in rx.stderr.splitlines() if l.startswith("{")]
    first_index = r["first_index"]
    check(first_index + r["samples_received"] + r["missing_known_samples"] == n and
          r["unknown_breaks"] == (1 if first_index else 0), "samples unaccounted")
    check(r["end_seen"] and r["end_index"] == n, "END index")
    check(tx.json["queue"]["high_water_records"] <= 8192, "sender queue unbounded")
    out = summary(tx, rx)
    out["proxy"] = counters
    out["events"] = events[:4]
    return out


def receiver_killed_then_replaced(suite):
    """SIGKILL of the receiving process: the segmented sender notices within
    the idle timeout, keeps its run, and serves a replacement receiver."""
    port = free_udp_port()
    n = 1600000  # 8 s
    tx = suite.sender(None, role="listener", port=port, rate="200000", policy="segmented", samples=n, **FAST)
    time.sleep(0.2)
    rx1 = suite.receiver(None, role="caller", port=port, rate="200000", policy="segmented", timeout_ms=40000, **FAST)
    time.sleep(1.5)
    rx1.proc.kill()  # no close, no goodbye
    rx1.proc.communicate()
    time.sleep(2.0)  # longer than the idle timeout
    rx2 = suite.receiver(None, role="caller", port=port, rate="200000", policy="segmented", timeout_ms=40000, **FAST)
    rx2.finish()
    tx.finish()
    t, r = tx.json, rx2.json
    check(tx.code == 0, "sender exit %d: %s" % (tx.code, t["session"]["failure_detail"]))
    check(t["session"]["counters"]["connections"] == 2, "sender connections %d" % t["session"]["counters"]["connections"])
    check(t["session"]["counters"]["connection_losses"] == 1, "loss not detected")
    check(rx2.code == 0 and r["samples_altered"] == 0, "replacement receiver exit %d" % rx2.code)
    check("initial_join" in r["reasons"] and r["first_index"] > 0, "replacement claims the start")
    check(r["end_seen"] and r["end_index"] == n, "END index")
    check(r["session"]["end"] == "incomplete", "end is " + r["session"]["end"])
    return summary(tx, rx2)


def strict_sender_fails_when_receiver_is_killed(suite):
    port = free_udp_port()
    rx = suite.receiver(None, role="listener", port=port, rate="200000", timeout_ms=40000, **FAST)
    time.sleep(0.2)
    tx = suite.sender(None, role="caller", port=port, rate="200000", samples=4000000, **FAST)
    time.sleep(1.5)
    t0 = time.monotonic()
    rx.proc.kill()
    rx.proc.communicate()
    tx.finish()
    detected = time.monotonic() - t0
    s = tx.json["session"]
    check(tx.code == 2 and s["failure"] in ("connection_lost", "continuity"),
          "exit %d failure %s" % (tx.code, s["failure"]))
    check(not s["completed"] and s["counters"]["end_sent"] == 0, "sender reported completion")
    check(detected < 3.5, "took %.1fs to notice a dead peer with a 1 s idle timeout" % detected)
    out = summary(tx)
    out["detected_after_s"] = round(detected, 2)
    return out


def lost_end_is_not_completion(suite):
    port = free_udp_port()
    proxy = FaultProxy(port, "--drop-end")
    rx = suite.receiver(None, role="listener", port=port, rate="200000", timeout_ms=40000, **FAST)
    time.sleep(0.2)
    tx = suite.sender(None, role="caller", port=proxy.port, rate="200000", samples=100000, **FAST)
    tx.finish()
    rx.finish()
    counters = proxy.close()
    r = rx.json
    check(counters["dropped_end"] >= 1, "END was not dropped")
    check(r["samples_received"] == 100000 and r["samples_altered"] == 0, "data incomplete")
    check(not r["end_seen"] and r["session"]["termination_unknown"], "END or completion reported")
    check(rx.code == 2 and r["session"]["failure"] == "connection_lost", r["session"]["failure"])
    check(tx.json["session"]["completed"] and not tx.json["session"]["end_acknowledged_by_transport"],
          "sender claims an acknowledged END")
    return summary(tx, rx)


CASES = [
    cf32_ctr_sink_caller,
    f32_plaintext_sink_listener,
    cf32_plaintext_sink_caller,
    f32_ctr_sink_listener,
    rational_rate_segmented,
    empty_run,
    single_sample_run,
    small_record_limit,
    wrong_key_delivers_nothing,
    missing_key_delivers_nothing,
    unexpected_key_is_not_downgraded,
    no_peer_caller_fails_at_startup_deadline,
    no_peer_listener_fails_at_startup_deadline,
    rate_mismatch_is_a_terminal_configuration_error,
    format_mismatch_is_a_terminal_configuration_error,
    stream_id_mismatch_is_rejected,
    mid_run_join_segmented_and_strict,
    cancellation_is_not_completion,
    segmented_sender_without_peer_stays_bounded,
    strict_receiver_stall_is_terminal_not_silent,
    segmented_receiver_stall_reports_the_drop,
    invalid_configuration_is_rejected_before_any_socket,
    outage_reconnects_within_one_run,
    receiver_killed_then_replaced,
    strict_sender_fails_when_receiver_is_killed,
    lost_end_is_not_completion,
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sender", required=True)
    ap.add_argument("--receiver", required=True)
    ap.add_argument("--evidence")
    ap.add_argument("--only", action="append")
    args = ap.parse_args()
    suite = Suite(args.sender, args.receiver, args.evidence)
    for case in CASES:
        if not args.only or case.__name__ in args.only:
            suite.run(case)
    print("end-to-end: %d failed" % len(suite.failures))
    return 1 if suite.failures else 0


if __name__ == "__main__":
    sys.exit(main())
