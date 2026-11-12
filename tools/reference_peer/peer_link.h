// SPDX-License-Identifier: GPL-3.0-or-later
//
// One interface over the SRT session worker and the baseline transports, so
// the sender and receiver programs are identical for every transport.

#ifndef RWSRT_PEER_LINK_H
#define RWSRT_PEER_LINK_H

#include "baseline_link.h"
#include "session_worker.h"

#include <sys/resource.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

namespace rwsrt_peer {

class peer_link
{
public:
    peer_link(link_kind kind, rwsrt::stream_direction direction,
              const rwsrt::transport_config& transport, const rwsrt::stream_config& stream,
              rwsrt::bounded_record_queue& queue)
    {
        if (kind == link_kind::srt)
            d_srt = std::make_unique<rwsrt::session_worker>(direction, transport, stream, queue);
        else
            d_base = std::make_unique<baseline_link>(kind, direction, transport, stream, queue);
    }
    void start() { d_srt ? d_srt->start() : d_base->start(); }
    void stop() { d_srt ? d_srt->stop() : d_base->stop(); }
    void fail_overflow(const std::string& detail)
    {
        if (d_srt)
            d_srt->fail(rwsrt::session_failure::queue_overflow, detail);
        else
            d_base->fail("queue_overflow");
    }
    void consumer_drained() { d_srt ? d_srt->consumer_drained() : d_base->consumer_drained(); }
    bool failed() const
    {
        if (!d_srt)
            return d_base->failed();
        const rwsrt::session_state s = d_srt->snapshot().state;
        return s == rwsrt::session_state::failed || s == rwsrt::session_state::stopped;
    }
    bool completed() const { return d_srt ? d_srt->snapshot().completed : d_base->completed(); }
    bool wait_connected(std::chrono::milliseconds timeout) const
    {
        if (d_srt)
            return d_srt->wait_connected(timeout);
        const auto until = std::chrono::steady_clock::now() + timeout;
        while (!d_base->connected() && !d_base->failed() && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return d_base->connected();
    }
    bool wait_finished(std::chrono::milliseconds timeout) const
    {
        if (d_srt)
            return d_srt->wait_finished(timeout);
        const auto until = std::chrono::steady_clock::now() + timeout;
        while (!d_base->finished() && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return d_base->finished();
    }
    std::string failure() const
    {
        return d_srt ? rwsrt::to_string(d_srt->snapshot().failure)
                     : d_base->describe().to_json().find("\"failure\":\"none\"") != std::string::npos
                           ? "none" : "failed";
    }
    std::string status_json() const
    {
        return d_srt ? rwsrt::to_json(d_srt->snapshot()) : d_base->describe().to_json();
    }

private:
    std::unique_ptr<rwsrt::session_worker> d_srt;
    std::unique_ptr<baseline_link> d_base;
};

inline link_kind parse_transport(const std::string& name)
{
    if (name == "srt")
        return link_kind::srt;
    if (name == "udp")
        return link_kind::udp;
    if (name == "tcp")
        return link_kind::tcp;
    throw std::invalid_argument("--transport=srt|udp|tcp");
}

//! CPU seconds (user + system) and peak resident memory of this process.
inline std::string resource_json()
{
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    const double cpu = static_cast<double>(u.ru_utime.tv_sec + u.ru_stime.tv_sec) +
                       static_cast<double>(u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1e6;
    char buf[96];
    std::snprintf(buf, sizeof(buf), "{\"cpu_seconds\":%.3f,\"max_rss_kib\":%ld}", cpu, u.ru_maxrss);
    return buf;
}

inline long long monotonic_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace rwsrt_peer

#endif
