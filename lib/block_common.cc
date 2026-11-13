/* -*- c++ -*- */
/*
 * Copyright 2026 Robotweax.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "block_common.h"

#include <gnuradio/gr_complex.h>

#include <cstdlib>
#include <stdexcept>

namespace gr {
namespace robotweax_srt {
namespace blk {

const pmt::pmt_t key_capture_time = pmt::intern("rwsrt.capture_time");
const pmt::pmt_t key_input_discontinuity = pmt::intern("rwsrt.input_discontinuity");
const pmt::pmt_t key_record = pmt::intern("rwsrt.record");
const pmt::pmt_t key_discontinuity = pmt::intern("rwsrt.discontinuity");
const pmt::pmt_t key_rx_rate = pmt::intern("rx_rate");
const pmt::pmt_t port_status = pmt::intern("status");

std::size_t item_size(sample_format f)
{
    return f == sample_format::f32 ? sizeof(float) : sizeof(gr_complex);
}

rwsrt::stream_config to_internal(const stream_config& s)
{
    if (s.format != sample_format::f32 && s.format != sample_format::cf32)
        throw std::invalid_argument("stream configuration: sample_format must be f32 or cf32");
    if (s.record_limit_bytes < 0 || s.flush_interval_ms < 0 || s.queue_max_records < 0 ||
        s.queue_max_age_ms < 0)
        throw std::invalid_argument("stream configuration: sizes and intervals must not be negative");
    rwsrt::stream_config out;
    out.format = s.format == sample_format::f32 ? rwsrt::sample_format::f32
                                                : rwsrt::sample_format::cf32;
    out.rate_num = s.sample_rate_num;
    out.rate_den = s.sample_rate_den;
    out.record_limit_bytes = static_cast<std::size_t>(s.record_limit_bytes);
    out.flush_interval = std::chrono::milliseconds(s.flush_interval_ms);
    out.queue.max_bytes = static_cast<std::size_t>(s.queue_max_bytes);
    out.queue.max_records = static_cast<std::size_t>(s.queue_max_records);
    out.queue.max_age = std::chrono::milliseconds(s.queue_max_age_ms);
    out.policy = s.continuity == continuity_policy::strict_stop
                     ? rwsrt::continuity_policy::strict_stop
                     : rwsrt::continuity_policy::segmented_live;
    if (const std::string e = rwsrt::validate(out); !e.empty())
        throw std::invalid_argument("stream configuration: " + e);
    rwsrt::normalize_rate(out.rate_num, out.rate_den);
    return out;
}

rwsrt::transport_config to_internal(const transport_config& t)
{
    rwsrt::transport_config out;
    out.role = t.role == connection_role::caller ? rwsrt::connection_role::caller
                                                 : rwsrt::connection_role::listener;
    out.host = t.host;
    out.bind_address = t.bind_address;
    out.port = t.port;
    out.stream_id = t.stream_id;
    out.latency_ms = t.latency_ms;
    out.connect_timeout_ms = t.connect_timeout_ms;
    out.startup_timeout = std::chrono::milliseconds(t.startup_timeout_ms);
    out.peer_idle_timeout_ms = t.peer_idle_timeout_ms;
    out.reconnect = t.reconnect;
    out.retry.initial = std::chrono::milliseconds(t.reconnect_initial_ms);
    out.retry.maximum = std::chrono::milliseconds(t.reconnect_max_ms);
    out.retry.total_budget = std::chrono::milliseconds(t.reconnect_total_timeout_ms);
    out.stop_timeout = std::chrono::milliseconds(t.stop_timeout_ms);
    out.max_lateness = std::chrono::milliseconds(t.max_lateness_ms);
    if (t.status_interval_ms < 10)
        throw std::invalid_argument("transport configuration: status_interval_ms must be at least 10");
    if (t.security == security_mode::ctr) {
        if (t.passphrase_env.empty())
            throw std::invalid_argument(
                "transport configuration: CTR mode needs passphrase_env, the name of the "
                "environment variable that holds the passphrase");
        out.security = rwsrt::security_mode::ctr;
        // The secret is read at start(). A placeholder of valid length lets
        // every other value be checked now, before any worker exists.
        out.passphrase = "placeholder-until-start";
    } else {
        if (!t.passphrase_env.empty())
            throw std::invalid_argument(
                "transport configuration: passphrase_env is set but security is none");
        out.security = rwsrt::security_mode::none;
    }
    if (const std::string e = rwsrt::validate(out); !e.empty())
        throw std::invalid_argument("transport configuration: " + e);
    return out;
}

void resolve_secret(const transport_config& pub, rwsrt::transport_config& internal)
{
    if (pub.security != security_mode::ctr) {
        internal.passphrase.clear();
        return;
    }
    const char* secret = std::getenv(pub.passphrase_env.c_str());
    if (secret == nullptr)
        throw std::runtime_error("environment variable " + pub.passphrase_env +
                                 " (passphrase_env) is not set");
    internal.passphrase = secret;
    if (const std::string e = rwsrt::validate(internal); !e.empty())
        throw std::runtime_error("passphrase from " + pub.passphrase_env + ": " + e);
}

rwsrt::status_doc describe(const transport_config& t, const stream_config& s)
{
    rwsrt::status_doc d;
    d.set_str("role", t.role == connection_role::caller ? "caller" : "listener")
        .set_str("host", t.host)
        .set_str("bind_address", t.bind_address)
        .set_int("port", t.port)
        .set_str("stream_id", t.stream_id)
        .set_int("latency_ms", t.latency_ms)
        .set_int("peer_idle_timeout_ms", t.peer_idle_timeout_ms)
        .set_int("max_lateness_ms", t.max_lateness_ms)
        .set_str("security_mode", t.security == security_mode::ctr ? "ctr" : "none")
        .set_str("sample_format", s.format == sample_format::f32 ? "f32" : "cf32")
        .set_uint("sample_rate_num", s.sample_rate_num)
        .set_uint("sample_rate_den", s.sample_rate_den)
        .set_int("record_limit_bytes", s.record_limit_bytes)
        .set_int("flush_interval_ms", s.flush_interval_ms)
        .set_uint("queue_max_bytes", s.queue_max_bytes)
        .set_int("queue_max_records", s.queue_max_records)
        .set_int("queue_max_age_ms", s.queue_max_age_ms)
        .set_str("continuity_policy",
                 s.continuity == continuity_policy::strict_stop ? "strict_stop" : "segmented_live");
    return d;
}

pmt::pmt_t to_pmt(const rwsrt::status_doc& doc)
{
    pmt::pmt_t dict = pmt::make_dict();
    for (const auto& item : doc.items()) {
        pmt::pmt_t v;
        if (const auto* i = std::get_if<std::int64_t>(&item.second))
            v = pmt::from_long(static_cast<long>(*i));
        else if (const auto* u = std::get_if<std::uint64_t>(&item.second))
            v = pmt::from_uint64(*u);
        else if (const auto* d = std::get_if<double>(&item.second))
            v = pmt::from_double(*d);
        else if (const auto* b = std::get_if<bool>(&item.second))
            v = pmt::from_bool(*b);
        else if (const auto* s = std::get_if<std::string>(&item.second))
            v = pmt::intern(*s);
        else
            v = to_pmt(*std::get<std::shared_ptr<rwsrt::status_doc>>(item.second));
        dict = pmt::dict_add(dict, pmt::intern(item.first), v);
    }
    return dict;
}

std::string run_id_hex(const rwsrt::run_id_t& run)
{
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (const std::uint8_t b : run) {
        out += digits[b >> 4];
        out += digits[b & 0x0f];
    }
    return out;
}

pmt::pmt_t coordinate_pmt(const rwsrt::coordinate& c)
{
    pmt::pmt_t d = pmt::make_dict();
    d = pmt::dict_add(d, pmt::intern("run_id"), pmt::intern(run_id_hex(c.run)));
    d = pmt::dict_add(d, pmt::intern("segment_id"), pmt::from_uint64(c.segment));
    d = pmt::dict_add(d, pmt::intern("sample_index"), pmt::from_uint64(c.sample_index));
    return d;
}

pmt::pmt_t discontinuity_pmt(const rwsrt::discontinuity& brk)
{
    pmt::pmt_t d = pmt::make_dict();
    d = pmt::dict_add(d, pmt::intern("reasons"),
                      pmt::intern(rwsrt::reasons_to_string(brk.reasons)));
    d = pmt::dict_add(d, pmt::intern("has_previous"), pmt::from_bool(brk.has_previous));
    if (brk.has_previous)
        d = pmt::dict_add(d, pmt::intern("previous"), coordinate_pmt(brk.previous));
    d = pmt::dict_add(d, pmt::intern("next"), coordinate_pmt(brk.next));
    d = pmt::dict_add(d, pmt::intern("missing_known"), pmt::from_bool(brk.missing_known));
    // An unknown count is stated as unknown, never as zero.
    if (brk.missing_known)
        d = pmt::dict_add(d, pmt::intern("missing_samples"),
                          pmt::from_uint64(brk.missing_samples));
    d = pmt::dict_add(d, pmt::intern("connection_generation"),
                      pmt::from_uint64(brk.connection_generation));
    return d;
}

pmt::pmt_t capture_time_pmt(std::int64_t time_ns, std::uint64_t uncertainty_ns)
{
    pmt::pmt_t d = pmt::make_dict();
    d = pmt::dict_add(d, pmt::intern("time_ns"), pmt::from_long(static_cast<long>(time_ns)));
    d = pmt::dict_add(d, pmt::intern("uncertainty_ns"), pmt::from_uint64(uncertainty_ns));
    return d;
}

} // namespace blk
} // namespace robotweax_srt
} // namespace gr
