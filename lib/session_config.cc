/* -*- c++ -*- */
/*
 * Copyright 2026 Robotweax.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "session_config.h"

#include <arpa/inet.h>

namespace rwsrt {

namespace {
bool numeric_ipv4(const std::string& s)
{
    in_addr a{};
    return inet_pton(AF_INET, s.c_str(), &a) == 1;
}
} // namespace

std::string validate(const stream_config& s)
{
    if (s.format != sample_format::f32 && s.format != sample_format::cf32)
        return "sample_format must be f32 or cf32";
    if (s.rate_num == 0 || s.rate_den == 0)
        return "sample rate numerator and denominator must be nonzero";
    if (s.record_limit_bytes < header_size + sample_size(s.format))
        return "record_limit_bytes holds no sample";
    if (s.record_limit_bytes > qualified_payload_ceiling)
        return "record_limit_bytes exceeds the qualified single-packet ceiling of 1456";
    if (s.flush_interval.count() < 1)
        return "flush_interval_ms must be at least 1";
    if (s.queue.max_records == 0)
        return "queue_max_records must be positive";
    if (s.queue.max_bytes < s.record_limit_bytes)
        return "queue_max_bytes is smaller than one record";
    if (s.queue.max_age <= std::chrono::nanoseconds::zero())
        return "queue_max_age_ms must be positive";
    return {};
}

std::string validate(const transport_config& t)
{
    if (t.port < 1 || t.port > 65535)
        return "port must be 1-65535";
    if (t.role == connection_role::caller) {
        if (!numeric_ipv4(t.host))
            return "host must be a numeric IPv4 address for a caller";
    } else if (!numeric_ipv4(t.bind_address)) {
        return "bind_address must be a numeric IPv4 address";
    }
    if (t.stream_id.empty() || t.stream_id.size() > 512)
        return "stream_id must be 1-512 bytes";
    if (t.latency_ms < 20 || t.latency_ms > 60000)
        return "latency_ms must be 20-60000";
    if (t.connect_timeout_ms < 100)
        return "connect_timeout_ms must be at least 100";
    if (t.peer_idle_timeout_ms < 1000)
        return "peer_idle_timeout_ms must be at least 1000";
    if (t.startup_timeout.count() < 1)
        return "startup_timeout_ms must be positive";
    if (t.stop_timeout.count() < 100)
        return "stop_timeout_ms must be at least 100";
    if (t.max_lateness.count() < 0 || t.max_lateness.count() > 3600000)
        return "max_lateness_ms must be 0 (off) to 3600000";
    if (t.retry.initial.count() < 1 || t.retry.maximum < t.retry.initial)
        return "reconnect delays must satisfy 1 <= initial <= max";
    if (t.retry.jitter < 0.0 || t.retry.jitter > 1.0)
        return "reconnect jitter must be within 0-1";
    if (t.security == security_mode::ctr) {
        // 10-79 bytes is the range every SRT implementation accepts.
        if (t.passphrase.size() < 10 || t.passphrase.size() > 79)
            return "CTR mode needs a passphrase of 10-79 bytes";
        if (t.key_length != 16 && t.key_length != 24 && t.key_length != 32)
            return "key_length must be 16, 24 or 32";
    } else if (!t.passphrase.empty()) {
        return "a passphrase is set but security_mode is none";
    }
    if (t.srt_buffer_bytes < 64 * 1024)
        return "srt_buffer_bytes must be at least 65536";
    return {};
}

std::int64_t offered_bytes_per_second(const stream_config& s)
{
    const std::uint64_t bytes = sample_size(s.format);
    const std::uint64_t per_record = (s.record_limit_bytes - header_size) / bytes;
    // ceil(rate): samples per second.
    const std::uint64_t rate = (s.rate_num + s.rate_den - 1u) / s.rate_den;
    const std::uint64_t full_records = (rate + per_record - 1u) / per_record;
    const std::uint64_t flush_records =
        1000u / static_cast<std::uint64_t>(s.flush_interval.count()) + 1u;
    return static_cast<std::int64_t>(rate * bytes + (full_records + flush_records) * header_size);
}

} // namespace rwsrt
