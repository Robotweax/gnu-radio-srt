#!/usr/bin/env python
#
# Copyright 2026 Robotweax.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
"""Configuration validation and construction of both blocks."""

import os

from gnuradio import blocks, gr, gr_unittest

from qa_common import (KEY_ENV, configs, evidence, free_udp_port, no_secret, robotweax_srt,
                       thread_count, wait_done)


class qa_config(gr_unittest.TestCase):

    def test_keyword_constructors_reject_unknown_names_and_values(self):
        with self.assertRaises(TypeError):
            robotweax_srt.make_transport_config(prot=9000)
        with self.assertRaises(TypeError):
            robotweax_srt.make_stream_config(sample_rate=48000)
        with self.assertRaises(ValueError):
            robotweax_srt.make_stream_config(format="ci16")
        with self.assertRaises(ValueError):
            robotweax_srt.make_transport_config(security="gcm")
        c = robotweax_srt.make_stream_config(format="f32", continuity="segmented_live")
        self.assertEqual(c.format, robotweax_srt.sample_format.f32)
        self.assertEqual(c.continuity, robotweax_srt.continuity_policy.segmented_live)

    def test_defaults_match_the_plan(self):
        t, s = robotweax_srt.transport_config(), robotweax_srt.stream_config()
        self.assertEqual((t.port, t.latency_ms, t.connect_timeout_ms, t.startup_timeout_ms), (9000, 250, 3000, 30000))
        self.assertEqual((t.reconnect_initial_ms, t.reconnect_max_ms, t.reconnect_total_timeout_ms), (250, 4000, 60000))
        self.assertEqual((t.stop_timeout_ms, t.status_interval_ms, t.bind_address), (1000, 1000, "127.0.0.1"))
        self.assertEqual(t.security, robotweax_srt.security_mode.ctr)  # never plaintext by default
        self.assertEqual(t.stream_id, "rwsrt/v1/default")
        self.assertEqual((s.sample_rate_num, s.sample_rate_den, s.record_limit_bytes, s.flush_interval_ms),
                         (100000, 1, 1200, 2))
        self.assertEqual((s.queue_max_bytes, s.queue_max_records, s.queue_max_age_ms), (4 * 1024 * 1024, 8192, 100))
        self.assertEqual(s.format, robotweax_srt.sample_format.cf32)
        self.assertEqual(s.continuity, robotweax_srt.continuity_policy.strict_stop)

    def test_invalid_values_are_rejected_at_construction(self):
        bad_transport = [
            dict(port=0), dict(port=65536), dict(host="example.invalid"), dict(latency_ms=5),
            dict(stream_id=""), dict(stream_id="x" * 513), dict(status_interval_ms=1),
            dict(passphrase_env=""),                      # CTR without a variable name
            dict(security="none"),                        # plaintext with a variable name set
            dict(reconnect_initial_ms=0), dict(stop_timeout_ms=1), dict(startup_timeout_ms=0),
        ]
        bad_stream = [
            dict(sample_rate_num=0), dict(sample_rate_den=0), dict(record_limit_bytes=1457),
            dict(record_limit_bytes=96), dict(record_limit_bytes=-1), dict(flush_interval_ms=0),
            dict(queue_max_records=0), dict(queue_max_bytes=100), dict(queue_max_age_ms=0),
        ]
        threads = thread_count()
        for block in (robotweax_srt.srt_sink, robotweax_srt.srt_source):
            for extra in bad_transport:
                values = dict(role="caller", host="127.0.0.1", port=9000, passphrase_env=KEY_ENV)
                values.update(extra)
                with self.assertRaises(Exception, msg=str(extra)):
                    block(robotweax_srt.make_transport_config(**values), robotweax_srt.stream_config())
            for extra in bad_stream:
                good = robotweax_srt.make_transport_config(role="caller", host="127.0.0.1", passphrase_env=KEY_ENV)
                with self.assertRaises(Exception, msg=str(extra)):
                    block(good, robotweax_srt.make_stream_config(**extra))
        self.assertEqual(thread_count(), threads)  # nothing was started

    def test_listener_needs_no_host_and_caller_needs_one(self):
        ok = robotweax_srt.make_transport_config(role="listener", host="", passphrase_env=KEY_ENV)
        robotweax_srt.srt_source(ok, robotweax_srt.stream_config())
        with self.assertRaises(Exception):
            robotweax_srt.srt_sink(robotweax_srt.make_transport_config(role="caller", host="", passphrase_env=KEY_ENV),
                                   robotweax_srt.stream_config())

    def test_item_size_follows_the_sample_format(self):
        for fmt, size in (("cf32", gr.sizeof_gr_complex), ("f32", gr.sizeof_float)):
            ts, ss, tr, sr = configs(free_udp_port(), fmt=fmt)
            self.assertEqual(robotweax_srt.srt_sink(ts, ss).input_signature().sizeof_stream_item(0), size)
            self.assertEqual(robotweax_srt.srt_source(tr, sr).output_signature().sizeof_stream_item(0), size)

    def test_status_before_start(self):
        ts, ss, tr, sr = configs(free_udp_port())
        for block, direction in ((robotweax_srt.srt_sink(ts, ss), "send"),
                                 (robotweax_srt.srt_source(tr, sr), "receive")):
            st = block.status()
            self.assertEqual(st["direction"], direction)
            self.assertFalse(st["running"])
            self.assertEqual(st["session"]["state"], "stopped")
            self.assertEqual(st["config"]["security_mode"], "ctr")
            self.assertEqual(st["config"]["sample_rate_num"], 200000)
            self.assertEqual(st["srt_runtime_leases"], 0)
            self.assertEqual(st["versions"]["robotweax_srt"], "0.3.2")
            self.assertTrue(st["versions"]["gnuradio"].startswith("3.10"))
            no_secret(self, st, block.status_json())
            self.assertEqual(st, __import__("json").loads(block.status_json()))

    def test_missing_or_bad_secret_is_a_visible_terminal_failure(self):
        """The secret is read at start. start() runs on the scheduler thread,
        where an exception would hang the flowgraph, so the block finishes at
        once and reports why."""
        port = free_udp_port()
        cases = {"RWSRT_QA_UNSET_VARIABLE": None, "RWSRT_QA_SHORT": "short"}
        for env, value in cases.items():
            os.environ.pop(env, None)
            if value is not None:
                os.environ[env] = value
            for make in ("sink", "source"):
                tb = gr.top_block()
                t = robotweax_srt.make_transport_config(role="listener", port=port, passphrase_env=env)
                if make == "sink":
                    blk = robotweax_srt.srt_sink(t, robotweax_srt.stream_config())
                    tb.connect(blocks.null_source(gr.sizeof_gr_complex),
                               blocks.throttle(gr.sizeof_gr_complex, 100000, False), blk)
                else:
                    blk = robotweax_srt.srt_source(t, robotweax_srt.stream_config())
                    tb.connect(blk, blocks.null_sink(gr.sizeof_gr_complex))
                threads = thread_count()
                tb.start()
                waited = wait_done(tb, 5.0)
                st = blk.status()
                evidence("start_without_secret", block=make, env=env, waited_s=waited,
                         failure=st["session"]["failure"], detail=st["session"]["failure_detail"])
                self.assertIsNotNone(waited, "flowgraph did not end by itself")
                self.assertEqual(st["session"]["state"], "failed")
                self.assertEqual(st["session"]["failure"], "configuration")
                self.assertIn(env, st["session"]["failure_detail"])  # names the variable ...
                if value is not None:
                    self.assertNotIn(value, blk.status_json())  # ... never its content
                self.assertFalse(st["running"])
                self.assertEqual(st["srt_runtime_leases"], 0)
                self.assertEqual(thread_count(), threads)

if __name__ == '__main__':
    gr_unittest.run(qa_config)
