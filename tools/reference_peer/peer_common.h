// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shared pieces of the headless reference peers: option parsing and the
// deterministic test signal. No GNU Radio dependency.

#ifndef RWSRT_PEER_COMMON_H
#define RWSRT_PEER_COMMON_H

#include "session_config.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>

namespace rwsrt_peer {

/*!
 * Test signal: every sample value is a function of its coordinate alone, so
 * a receiver can verify any sample it gets, with or without gaps.
 * f32:  value = index mod 2^24
 * cf32: I = index mod 2^24, Q = (index / 2^24) mod 2^24
 * Both are exactly representable in binary32. Unique for index < 2^48 (cf32).
 */
inline void test_signal(rwsrt::sample_format format, std::uint64_t index, float* out)
{
    out[0] = static_cast<float>(index & 0xffffffu);
    if (format == rwsrt::sample_format::cf32)
        out[1] = static_cast<float>((index >> 24) & 0xffffffu);
}

class options
{
public:
    options(int argc, char** argv)
    {
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            if (a.rfind("--", 0) != 0)
                throw std::invalid_argument("unexpected argument: " + a);
            const std::size_t eq = a.find('=');
            if (eq == std::string::npos)
                d_values[a.substr(2)] = "1";
            else
                d_values[a.substr(2, eq - 2)] = a.substr(eq + 1);
        }
    }
    std::string str(const std::string& key, const std::string& fallback)
    {
        d_seen[key] = true;
        const auto it = d_values.find(key);
        return it == d_values.end() ? fallback : it->second;
    }
    long long num(const std::string& key, long long fallback)
    {
        const std::string v = str(key, "");
        return v.empty() ? fallback : std::stoll(v);
    }
    bool flag(const std::string& key) { return str(key, "0") == "1"; }
    //! Call after reading every option: an unknown one is an error.
    void finish() const
    {
        for (const auto& kv : d_values)
            if (!d_seen.count(kv.first))
                throw std::invalid_argument("unknown option: --" + kv.first);
    }

private:
    std::map<std::string, std::string> d_values;
    std::map<std::string, bool> d_seen;
};

//! Options common to both peers.
inline void read_common(options& o, rwsrt::transport_config& t, rwsrt::stream_config& s)
{
    using namespace rwsrt;
    const std::string role = o.str("role", "");
    if (role == "caller")
        t.role = connection_role::caller;
    else if (role == "listener")
        t.role = connection_role::listener;
    else
        throw std::invalid_argument("--role=caller|listener is required");
    t.host = o.str("host", "127.0.0.1");
    t.bind_address = o.str("bind", "127.0.0.1");
    t.port = static_cast<int>(o.num("port", 9000));
    t.stream_id = o.str("stream-id", t.stream_id);
    t.latency_ms = static_cast<int>(o.num("latency-ms", 250));
    t.connect_timeout_ms = static_cast<int>(o.num("connect-timeout-ms", 3000));
    t.startup_timeout = std::chrono::milliseconds(o.num("startup-timeout-ms", 30000));
    t.retry.total_budget = std::chrono::milliseconds(o.num("reconnect-total-timeout-ms", 60000));
    t.reconnect = !o.flag("no-reconnect");

    // The secret comes from the environment, never from the command line.
    const std::string env = o.str("passphrase-env", "");
    if (o.flag("plaintext")) {
        if (!env.empty())
            throw std::invalid_argument("--plaintext and --passphrase-env are exclusive");
        t.security = security_mode::none;
    } else {
        if (env.empty())
            throw std::invalid_argument("--passphrase-env=NAME or --plaintext is required");
        const char* secret = std::getenv(env.c_str());
        if (!secret)
            throw std::invalid_argument("environment variable " + env + " is not set");
        t.security = security_mode::ctr;
        t.passphrase = secret;
    }

    const std::string format = o.str("format", "cf32");
    if (format == "cf32")
        s.format = sample_format::cf32;
    else if (format == "f32")
        s.format = sample_format::f32;
    else
        throw std::invalid_argument("--format=f32|cf32");
    const std::string rate = o.str("rate", "100000");
    const std::size_t slash = rate.find('/');
    s.rate_num = static_cast<std::uint32_t>(std::stoul(rate.substr(0, slash)));
    s.rate_den = slash == std::string::npos
                     ? 1u
                     : static_cast<std::uint32_t>(std::stoul(rate.substr(slash + 1)));
    s.record_limit_bytes = static_cast<std::size_t>(o.num("record-limit", 1200));
    s.flush_interval = std::chrono::milliseconds(o.num("flush-interval-ms", 2));
    s.queue.max_bytes = static_cast<std::size_t>(o.num("queue-max-bytes", 4 * 1024 * 1024));
    s.queue.max_records = static_cast<std::size_t>(o.num("queue-max-records", 8192));
    s.queue.max_age = std::chrono::milliseconds(o.num("queue-max-age-ms", 100));
    const std::string policy = o.str("policy", "strict");
    if (policy == "strict")
        s.policy = continuity_policy::strict_stop;
    else if (policy == "segmented")
        s.policy = continuity_policy::segmented_live;
    else
        throw std::invalid_argument("--policy=strict|segmented");
}

inline std::string queue_json(const rwsrt::queue_stats& q)
{
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "{\"records\":%zu,\"bytes\":%zu,\"high_water_records\":%zu,"
                  "\"high_water_bytes\":%zu,\"dropped_overflow_records\":%llu,"
                  "\"dropped_overflow_samples\":%llu,\"dropped_expired_records\":%llu,"
                  "\"dropped_expired_samples\":%llu,\"pushed\":%llu,\"popped\":%llu}",
                  q.records, q.bytes, q.high_water_records, q.high_water_bytes,
                  static_cast<unsigned long long>(q.dropped_overflow.records),
                  static_cast<unsigned long long>(q.dropped_overflow.samples),
                  static_cast<unsigned long long>(q.dropped_expired.records),
                  static_cast<unsigned long long>(q.dropped_expired.samples),
                  static_cast<unsigned long long>(q.pushed),
                  static_cast<unsigned long long>(q.popped));
    return buf;
}

} // namespace rwsrt_peer

#endif
