#
# Copyright 2026 Robotweax.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
"""Shared helpers of the flowgraph QA suites."""

import json
import os
import socket
import threading
import time

import pmt
from gnuradio import blocks, gr

try:
    from gnuradio import robotweax_srt
except ImportError:
    import sys
    dirname, filename = os.path.split(os.path.abspath(__file__))
    sys.path.append(os.path.join(dirname, "bindings"))
    from gnuradio import robotweax_srt

KEY_ENV = "RWSRT_QA_PASSPHRASE"
OTHER_KEY_ENV = "RWSRT_QA_PASSPHRASE_OTHER"
os.environ[KEY_ENV] = "qa-suite-passphrase-A"
os.environ[OTHER_KEY_ENV] = "qa-suite-passphrase-B"
SECRETS = (os.environ[KEY_ENV], os.environ[OTHER_KEY_ENV])

RATE = 200000
STOP_BOUND_MS = 1000.0
PER_RECORD = {"cf32": 138, "f32": 276}  # samples in a 1200-byte record


def free_udp_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def evidence(name, **fields):
    print("M2-EVIDENCE " + json.dumps({"test": name, **fields}, sort_keys=True, default=str), flush=True)


def configs(port, fmt="cf32", sink_role="caller", policy="strict_stop", key=KEY_ENV,
            sink_policy=None, source_policy=None, sink_key="same", source_key="same",
            transport=None, stream=None, sink_stream=None, source_stream=None):
    """(sink transport, sink stream, source transport, source stream)."""
    source_role = "listener" if sink_role == "caller" else "caller"

    def tcfg(role, k):
        k = key if k == "same" else k
        values = dict(role=role, host="127.0.0.1", bind_address="127.0.0.1", port=port,
                      security="ctr" if k else "none", passphrase_env=k or "",
                      status_interval_ms=100)
        values.update(transport or {})
        return robotweax_srt.make_transport_config(**values)

    def scfg(pol, extra):
        values = dict(format=fmt, sample_rate_num=RATE, continuity=pol)
        values.update(stream or {})
        values.update(extra or {})
        return robotweax_srt.make_stream_config(**values)

    return (tcfg(sink_role, sink_key), scfg(sink_policy or policy, sink_stream),
            tcfg(source_role, source_key), scfg(source_policy or policy, source_stream))


def itemsize(fmt):
    return gr.sizeof_gr_complex if fmt == "cf32" else gr.sizeof_float


def vector_source(fmt, data, tags=()):
    cls = blocks.vector_source_c if fmt == "cf32" else blocks.vector_source_f
    return cls(list(data), False, 1, list(tags))


def vector_sink(fmt):
    return blocks.vector_sink_c() if fmt == "cf32" else blocks.vector_sink_f()


def test_signal(fmt, n, start=0):
    """Values that identify their own index (exact in float32)."""
    if fmt == "cf32":
        return [complex((start + i) & 0xffffff, ((start + i) >> 24) + 1) for i in range(n)]
    return [float((start + i) & 0xffffff) for i in range(n)]


def make_tag(offset, key, value):
    t = gr.tag_t()
    t.offset = offset
    t.key = pmt.intern(key)
    t.value = value
    t.srcid = pmt.intern("qa")
    return t


def pdict(**values):
    d = pmt.make_dict()
    for k, v in values.items():
        if isinstance(v, bool):
            pv = pmt.from_bool(v)
        elif isinstance(v, int):
            pv = pmt.from_uint64(v)
        else:
            pv = pmt.to_pmt(v)
        d = pmt.dict_add(d, pmt.intern(k), pv)
    return d


class Sender:
    """vector source -> throttle -> srt_sink, in its own flowgraph."""

    def __init__(self, tcfg, scfg, fmt, data, tags=(), rate=RATE, endless=False, chunk=0):
        self.tb = gr.top_block()
        self.sink = robotweax_srt.srt_sink(tcfg, scfg)
        self.msgs = blocks.message_debug()
        if endless:
            src = blocks.null_source(itemsize(fmt))
        else:
            src = vector_source(fmt, data, tags)
        # Paced like a sample clock: an unpaced burst is not a real-time stream.
        # chunk > 0 limits how many items the throttle forwards at once.
        self.throttle = blocks.throttle(itemsize(fmt), rate, False, chunk)
        self.tb.connect(src, self.throttle, self.sink)
        self.tb.msg_connect(self.sink, "status", self.msgs, "store")

    def status(self):
        return self.sink.status()


class Receiver:
    """srt_source -> vector sink, in its own flowgraph."""

    def __init__(self, tcfg, scfg, fmt, max_noutput=None, slow_rate=None):
        self.tb = gr.top_block()
        self.source = robotweax_srt.srt_source(tcfg, scfg)
        self.out = vector_sink(fmt)
        self.msgs = blocks.message_debug()
        if max_noutput:
            self.source.set_max_noutput_items(max_noutput)
        if slow_rate:
            # A consumer slower than the stream: backpressure reaches the source.
            self.tb.connect(self.source, blocks.throttle(itemsize(fmt), slow_rate, False), self.out)
        else:
            self.tb.connect(self.source, self.out)
        self.tb.msg_connect(self.source, "status", self.msgs, "store")

    def status(self):
        return self.source.status()

    def data(self):
        return list(self.out.data())

    def tags(self, key):
        return sorted(((t.offset, pmt.to_python(t.value)) for t in self.out.tags()
                       if pmt.symbol_to_string(t.key) == key), key=lambda x: x[0])


def wait_done(tb, timeout):
    """tb.wait() with a deadline. Returns seconds waited, or None on timeout
    (the flowgraph is then stopped so the test can fail cleanly)."""
    t0 = time.monotonic()
    th = threading.Thread(target=tb.wait)
    th.start()
    th.join(timeout)
    if th.is_alive():
        tb.stop()
        th.join(10)
        return None
    return time.monotonic() - t0


def stop_and_time(tb):
    t0 = time.monotonic()
    tb.stop()
    tb.wait()
    return (time.monotonic() - t0) * 1000.0


def wait_for(predicate, timeout, step=0.02):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(step)
    return predicate()


def thread_count():
    with open("/proc/self/status") as f:
        for line in f:
            if line.startswith("Threads:"):
                return int(line.split()[1])
    return -1


def no_secret(testcase, *objects):
    text = json.dumps(objects, default=str)
    for secret in SECRETS:
        testcase.assertNotIn(secret, text)
