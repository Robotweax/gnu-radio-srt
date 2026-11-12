/* -*- c++ -*- */
/*
 * Copyright 2026 Robotweax.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "m0_probe_sink_impl.h"

#include <gnuradio/block_detail.h>
#include <gnuradio/buffer_reader.h>
#include <gnuradio/io_signature.h>

#include <srt/srt.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <chrono>
#include <sstream>
#include <stdexcept>

namespace gr {
namespace robotweax_srt {

namespace {

using steady = std::chrono::steady_clock;

// Upper bound for any single wait in the worker (plan section 9).
constexpr int wait_slice_ms = 20;
constexpr int retry_delay_ms = 250;

template <typename T>
bool set_opt(SRTSOCKET s, SRT_SOCKOPT opt, const T& value)
{
    return srt_setsockflag(s, opt, &value, sizeof(value)) != SRT_ERROR;
}

bool configure(SRTSOCKET s)
{
    const SRT_TRANSTYPE tt = SRTT_LIVE;
    const linger no_linger{ 0, 0 };
    return set_opt(s, SRTO_TRANSTYPE, tt) && set_opt(s, SRTO_MESSAGEAPI, true) &&
           set_opt(s, SRTO_SNDSYN, false) && set_opt(s, SRTO_RCVSYN, false) &&
           set_opt(s, SRTO_LINGER, no_linger) && set_opt(s, SRTO_CONNTIMEO, 3000);
}

} // namespace

m0_probe_sink::sptr m0_probe_sink::make(bool listener, const std::string& host, int port)
{
    return gnuradio::make_block_sptr<m0_probe_sink_impl>(listener, host, port);
}

m0_probe_sink_impl::m0_probe_sink_impl(bool listener, const std::string& host, int port)
    : gr::block("m0_probe_sink",
                gr::io_signature::make(1, 1, sizeof(gr_complex)),
                gr::io_signature::make(0, 0, 0)),
      d_listener(listener),
      d_host(host),
      d_port(port)
{
    // Reject invalid values before any worker exists.
    in_addr parsed{};
    if (inet_pton(AF_INET, host.c_str(), &parsed) != 1)
        throw std::invalid_argument("m0_probe_sink: host must be a numeric IPv4 address");
    if (port < 1 || port > 65535)
        throw std::invalid_argument("m0_probe_sink: port must be 1-65535");
}

m0_probe_sink_impl::~m0_probe_sink_impl() { shutdown_worker(); }

bool m0_probe_sink_impl::start()
{
    std::lock_guard<std::mutex> guard(d_mutex);
    ++d_start_calls;
    if (!d_running) {
        d_runtime = runtime_owner::acquire();
        d_cancel.store(false);
        d_worker_state.store(1);
        d_worker = std::thread([this] { worker_main(); });
        d_running = true;
    }
    return block::start();
}

bool m0_probe_sink_impl::stop()
{
    const auto t0 = steady::now();
    // What the runtime exposes at the moment stop() runs: is the input
    // marked done (upstream finished), and is anything left unconsumed?
    bool input_done = false;
    std::int64_t pending = -1;
    if (const block_detail_sptr d = detail()) {
        if (d->ninputs() > 0 && d->input(0)) {
            input_done = d->input(0)->done();
            pending = d->input(0)->items_available();
        }
    }
    shutdown_worker();
    const double ms = std::chrono::duration<double, std::milli>(steady::now() - t0).count();
    {
        std::lock_guard<std::mutex> guard(d_mutex);
        ++d_stop_calls;
        if (d_running) {
            ++d_effective_stops;
            d_running = false;
            d_stop_input_done = input_done;
            d_stop_items_pending = pending;
            d_stop_ms = ms;
            d_max_stop_ms = std::max(d_max_stop_ms, ms);
        }
    }
    return block::stop();
}

void m0_probe_sink_impl::shutdown_worker()
{
    d_cancel.store(true);
    if (d_worker.joinable())
        d_worker.join(); // the worker closes its own sockets before exiting
    d_runtime.reset();   // only after every handle of this block is closed
}

void m0_probe_sink_impl::worker_main()
{
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(d_port));
    inet_pton(AF_INET, d_host.c_str(), &addr.sin_addr);

    // Bounded wait that also records the longest slice actually observed.
    auto wait = [this](int eid, SRT_EPOLL_EVENT* ev) {
        const auto t0 = steady::now();
        const int n = srt_epoll_uwait(eid, ev, 1, wait_slice_ms);
        const auto us =
            std::chrono::duration_cast<std::chrono::microseconds>(steady::now() - t0).count();
        std::int64_t seen = d_max_wait_slice_us.load();
        while (us > seen && !d_max_wait_slice_us.compare_exchange_weak(seen, us)) {
        }
        return n;
    };
    auto pause = [this](int total_ms) {
        for (int waited = 0; waited < total_ms && !d_cancel.load(); waited += wait_slice_ms)
            std::this_thread::sleep_for(std::chrono::milliseconds(wait_slice_ms));
    };

    while (!d_cancel.load()) {
        d_worker_state.store(1);
        ++d_attempts;
        const SRTSOCKET s = srt_create_socket();
        const int eid = srt_epoll_create();
        SRTSOCKET conn = SRT_INVALID_SOCK;
        bool setup_ok = s != SRT_INVALID_SOCK && eid != SRT_ERROR && configure(s);
        SRT_EPOLL_EVENT ev{};

        if (setup_ok && d_listener) {
            setup_ok = srt_bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) !=
                           SRT_ERROR &&
                       srt_listen(s, 1) != SRT_ERROR;
            const int mask = SRT_EPOLL_IN | SRT_EPOLL_ERR;
            setup_ok = setup_ok && srt_epoll_add_usock(eid, s, &mask) != SRT_ERROR;
            while (setup_ok && !d_cancel.load()) {
                if (wait(eid, &ev) > 0) {
                    if (ev.events & SRT_EPOLL_IN)
                        conn = srt_accept(s, nullptr, nullptr);
                    break;
                }
            }
        } else if (setup_ok) {
            const int mask = SRT_EPOLL_OUT | SRT_EPOLL_ERR;
            setup_ok = srt_epoll_add_usock(eid, s, &mask) != SRT_ERROR;
            if (setup_ok)
                (void)srt_connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
            while (setup_ok && !d_cancel.load()) {
                if (wait(eid, &ev) > 0) {
                    if ((ev.events & SRT_EPOLL_OUT) && !(ev.events & SRT_EPOLL_ERR) &&
                        srt_getsockstate(s) == SRTS_CONNECTED)
                        conn = s;
                    break;
                }
            }
        }
        if (!setup_ok)
            ++d_setup_errors;

        if (conn != SRT_INVALID_SOCK) {
            ++d_connections;
            d_worker_state.store(2);
            // Hold the connection until it breaks or the block stops.
            const int mask = SRT_EPOLL_ERR;
            (void)srt_epoll_clear_usocks(eid);
            (void)srt_epoll_add_usock(eid, conn, &mask);
            while (!d_cancel.load()) {
                if (wait(eid, &ev) > 0 && (ev.events & SRT_EPOLL_ERR))
                    break;
            }
        }

        if (eid != SRT_ERROR)
            (void)srt_epoll_release(eid);
        if (conn != SRT_INVALID_SOCK && conn != s)
            (void)srt_close(conn);
        if (s != SRT_INVALID_SOCK)
            (void)srt_close(s);
        if (!d_cancel.load())
            pause(retry_delay_ms);
    }
    d_worker_state.store(3);
}

std::string m0_probe_sink_impl::report() const
{
    std::lock_guard<std::mutex> guard(d_mutex);
    std::ostringstream os;
    os << "{\"start_calls\":" << d_start_calls << ",\"stop_calls\":" << d_stop_calls
       << ",\"effective_stops\":" << d_effective_stops << ",\"work_calls\":"
       << d_work_calls.load() << ",\"work_calls_seeing_input_done\":"
       << d_work_calls_seeing_done.load() << ",\"items_consumed\":" << d_items_consumed.load()
       << ",\"stop_input_done\":" << (d_stop_input_done ? "true" : "false")
       << ",\"stop_items_pending\":" << d_stop_items_pending << ",\"stop_ms\":" << d_stop_ms
       << ",\"max_stop_ms\":" << d_max_stop_ms << ",\"worker_state\":" << d_worker_state.load()
       << ",\"attempts\":" << d_attempts.load() << ",\"connections\":" << d_connections.load()
       << ",\"setup_errors\":" << d_setup_errors.load() << ",\"max_wait_slice_ms\":"
       << d_max_wait_slice_us.load() / 1000.0 << ",\"runtime_leases\":"
       << runtime_owner::active_leases() << "}";
    return os.str();
}

void m0_probe_sink_impl::forecast(int /*noutput_items*/, gr_vector_int& ninput_items_required)
{
    ninput_items_required[0] = 1;
}

int m0_probe_sink_impl::general_work(int /*noutput_items*/,
                                     gr_vector_int& ninput_items,
                                     gr_vector_const_void_star& /*input_items*/,
                                     gr_vector_void_star& /*output_items*/)
{
    ++d_work_calls;
    if (const block_detail_sptr d = detail()) {
        if (d->input(0)->done())
            ++d_work_calls_seeing_done;
    }
    // Explicit consumption accounting: exactly what was offered, discarded.
    d_items_consumed += static_cast<std::uint64_t>(ninput_items[0]);
    consume(0, ninput_items[0]);
    return 0;
}

} /* namespace robotweax_srt */
} /* namespace gr */
