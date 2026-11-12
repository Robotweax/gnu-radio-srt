// SPDX-License-Identifier: GPL-3.0-or-later
#include "baseline_link.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <map>
#include <vector>

namespace rwsrt_peer {

using namespace rwsrt;
using clock_type = std::chrono::steady_clock;

namespace {

constexpr int slice_ms = 20;

sockaddr_in address(const std::string& ip, int port)
{
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(static_cast<std::uint16_t>(port));
    inet_pton(AF_INET, ip.c_str(), &sa.sin_addr);
    return sa;
}

void set_buffers(int fd)
{
    const int size = 4 * 1024 * 1024;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
}

bool readable(int fd, int timeout_ms)
{
    pollfd p{ fd, POLLIN, 0 };
    return poll(&p, 1, timeout_ms) > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR));
}

} // namespace

baseline_link::baseline_link(link_kind kind, stream_direction direction,
                             const transport_config& transport, const stream_config& stream,
                             bounded_record_queue& queue)
    : d_kind(kind),
      d_direction(direction),
      d_transport(transport),
      d_stream([&stream] {
          stream_config s = stream;
          normalize_rate(s.rate_num, s.rate_den);
          return s;
      }()),
      d_queue(queue),
      d_ledger(stream.policy, d_stream.format, d_stream.rate_num, d_stream.rate_den)
{
}

baseline_link::~baseline_link() { stop(); }

void baseline_link::start()
{
    d_cancel.store(false);
    d_thread = std::thread([this] {
        if (d_direction == stream_direction::send)
            run_send();
        else if (d_kind == link_kind::udp)
            run_receive_udp();
        else
            run_receive_tcp();
    });
}

void baseline_link::stop()
{
    d_cancel.store(true);
    if (d_thread.joinable())
        d_thread.join();
}

void baseline_link::set_failure(const std::string& failure, const std::string& detail)
{
    std::lock_guard<std::mutex> guard(d_mutex);
    if (d_failure == "none" && !d_completed) {
        d_failure = failure;
        d_failure_detail = detail;
    }
}

void baseline_link::fail(const std::string& reason) { set_failure(reason, "reported by the peer program"); }

bool baseline_link::connected() const
{
    std::lock_guard<std::mutex> guard(d_mutex);
    return d_counters.connected;
}

bool baseline_link::failed() const
{
    std::lock_guard<std::mutex> guard(d_mutex);
    return d_failure != "none";
}

bool baseline_link::completed() const
{
    std::lock_guard<std::mutex> guard(d_mutex);
    return d_completed;
}

status_doc baseline_link::describe() const
{
    std::lock_guard<std::mutex> guard(d_mutex);
    status_doc d;
    d.set_str("state", d_failure != "none" ? "failed" : d_completed ? "completed"
                                                 : d_counters.connected ? "connected" : "connecting")
        .set_str("failure", d_failure)
        .set_str("failure_detail", d_failure_detail)
        .set_str("end", d_end_received ? (d_end_complete ? "complete" : "incomplete") : "none")
        .set_bool("termination_unknown", d_ledger.run_open() && d_failure != "none")
        .set_bool("completed", d_completed);
    d.child("counters")
        .set_uint("connect_attempts", d_counters.connect_attempts)
        .set_uint("records_send_accepted", d_counters.records_sent)
        .set_uint("samples_send_accepted", d_counters.samples_sent)
        .set_uint("bytes_send_accepted", d_counters.bytes_sent)
        .set_uint("send_errors", d_counters.send_errors)
        .set_uint("records_received", d_counters.records_received)
        .set_uint("bytes_received", d_counters.bytes_received)
        .set_uint("malformed_records", d_counters.malformed_records)
        .set_uint("too_late_dropped", d_counters.too_late_dropped)
        .set_uint("duplicates_dropped", d_counters.duplicates_dropped)
        .set_uint("playout_high_water", d_counters.playout_high_water)
        .set_uint("end_sent", d_counters.end_sent ? 1u : 0u);
    const ledger_counters& c = d_ledger.counters();
    d.child("continuity")
        .set_uint("records_accepted", c.records_accepted)
        .set_uint("samples_accepted", c.samples_accepted)
        .set_uint("known_missing_samples", c.known_missing_samples)
        .set_uint("unknown_gaps", c.unknown_gaps)
        .set_uint("sequence_gaps", c.sequence_gaps)
        .set_uint("discontinuities", c.discontinuities)
        .set_uint("runs", c.runs);
    return d;
}

// ------------------------------------------------------------------ sender

void baseline_link::run_send()
{
    const sockaddr_in peer = address(d_transport.host, d_transport.port);
    const auto started = clock_type::now();
    int fd = -1;
    if (d_kind == link_kind::udp) {
        fd = socket(AF_INET, SOCK_DGRAM, 0);
        set_buffers(fd);
        (void)connect(fd, reinterpret_cast<const sockaddr*>(&peer), sizeof(peer));
        std::lock_guard<std::mutex> guard(d_mutex);
        d_counters.connected = true; // nothing to establish
    } else {
        while (!d_cancel.load()) {
            fd = socket(AF_INET, SOCK_STREAM, 0);
            // The kernel's default send buffer holds seconds of this stream.
            // Data queued there cannot be dropped when it goes stale, so TCP
            // would deliver old samples long after a stall. The buffer is
            // sized to the latency budget instead; beyond it, the same
            // age-limited queue as for the other transports discards.
            const int budget = static_cast<int>(std::max<std::int64_t>(
                16 * 1024, offered_bytes_per_second(d_stream) * d_transport.latency_ms / 1000));
            (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &budget, sizeof(budget));
            const int one = 1;
            (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            {
                std::lock_guard<std::mutex> guard(d_mutex);
                ++d_counters.connect_attempts;
            }
            if (connect(fd, reinterpret_cast<const sockaddr*>(&peer), sizeof(peer)) == 0) {
                std::lock_guard<std::mutex> guard(d_mutex);
                d_counters.connected = true;
                break;
            }
            close(fd);
            fd = -1;
            if (clock_type::now() - started >= d_transport.startup_timeout) {
                set_failure("startup_timeout", "no TCP peer within the startup timeout");
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (fd < 0)
            return;
        // A write may block while TCP recovers; never longer than one slice
        // at a time, so stop stays prompt.
        timeval tv{ 0, slice_ms * 1000 };
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    while (!d_cancel.load()) {
        queue_entry e;
        const pop_status st = d_queue.wait_pop(e, std::chrono::milliseconds(slice_ms));
        if (st == pop_status::expired) {
            set_failure("queue_overflow", "queued record outlived queue_max_age");
            break;
        }
        if (st != pop_status::record)
            continue;
        bool ok = true;
        if (d_kind == link_kind::udp) {
            ok = send(fd, e.message.data(), e.message.size(), 0) ==
                 static_cast<ssize_t>(e.message.size());
        } else {
            std::size_t done = 0;
            while (done < e.message.size() && !d_cancel.load()) {
                const ssize_t n = send(fd, e.message.data() + done, e.message.size() - done, MSG_NOSIGNAL);
                if (n > 0) {
                    done += static_cast<std::size_t>(n);
                } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    set_failure("connection_lost", std::strerror(errno));
                    ok = false;
                    break;
                }
            }
            if (done < e.message.size())
                ok = false;
        }
        std::lock_guard<std::mutex> guard(d_mutex);
        if (!ok) {
            ++d_counters.send_errors;
            if (d_kind == link_kind::tcp)
                break; // a TCP stream with a hole in it is useless
            continue;
        }
        ++d_counters.records_sent;
        d_counters.samples_sent += e.header.sample_count;
        d_counters.bytes_sent += e.message.size();
        if (e.header.kind == record_kind::end) {
            d_counters.end_sent = true;
            d_completed = true;
            break;
        }
    }
    if (fd >= 0) {
        if (d_kind == link_kind::tcp)
            (void)shutdown(fd, SHUT_WR); // let the kernel deliver what is queued
        close(fd);
    }
}

// ---------------------------------------------------------------- receiver

// Validates one record and passes it on. Returns false on a terminal error.
bool baseline_link::accept_record(const std::uint8_t* data, std::size_t size)
{
    decoded_record rec;
    if (decode(std::span<const std::uint8_t>(data, size), d_stream.record_limit_bytes, rec) !=
        wire_error::none) {
        std::lock_guard<std::mutex> guard(d_mutex);
        ++d_counters.malformed_records;
        return d_kind == link_kind::udp; // a bad datagram is skipped; a bad TCP stream is lost
    }
    ledger_result lr;
    {
        std::lock_guard<std::mutex> guard(d_mutex);
        lr = d_ledger.on_record(rec.header);
        ++d_counters.records_received;
        d_counters.bytes_received += size;
    }
    if (!lr.accepted) {
        set_failure(lr.failure == ledger_failure::config_mismatch ? "configuration"
                        : lr.failure == ledger_failure::protocol_violation ? "protocol" : "continuity",
                    lr.detail);
        return false;
    }
    queue_entry e;
    e.header = rec.header;
    e.message.assign(data, data + size);
    e.brk = lr.brk;
    const push_status ps = d_queue.push(std::move(e), clock_type::now());
    if (ps != push_status::ok) {
        set_failure("queue_overflow", "receive queue full");
        return false;
    }
    if (lr.is_end) {
        std::lock_guard<std::mutex> guard(d_mutex);
        d_end_received = true;
        d_end_complete = lr.end_complete;
    }
    return true;
}

void baseline_link::run_receive_udp()
{
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    set_buffers(fd);
    const sockaddr_in local = address(d_transport.bind_address, d_transport.port);
    if (bind(fd, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
        set_failure("configuration", std::string("bind: ") + std::strerror(errno));
        close(fd);
        return;
    }
    d_ledger.on_connection();

    struct held {
        std::vector<std::uint8_t> bytes;
        clock_type::time_point due;
    };
    // Playout buffer keyed by record sequence. Fixed delay: a record is due
    // at (first arrival + latency) plus its nominal sample offset from the
    // first record, exactly the budget SRT's receiver latency provides.
    std::map<std::uint64_t, held> buffer;
    bool have_base = false, any_released = false, end_released = false;
    clock_type::time_point base;
    std::uint64_t base_index = 0, last_released = 0;
    const auto latency = std::chrono::milliseconds(d_transport.latency_ms);
    const auto started = clock_type::now();
    std::vector<std::uint8_t> datagram(2048);

    while (!d_cancel.load()) {
        if (end_released) {
            if (d_consumer_drained.load()) {
                std::lock_guard<std::mutex> guard(d_mutex);
                d_completed = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(slice_ms));
            continue;
        }
        if (!have_base && clock_type::now() - started >= d_transport.startup_timeout) {
            set_failure("startup_timeout", "no datagram within the startup timeout");
            break;
        }
        int wait = slice_ms;
        if (!buffer.empty()) {
            // Rounded up to a whole millisecond: polling with zero timeout
            // until the record is due would spin a core.
            const auto until = std::chrono::duration_cast<std::chrono::microseconds>(
                                   buffer.begin()->second.due - clock_type::now()).count();
            wait = static_cast<int>(std::clamp<long long>((until + 999) / 1000, 0, slice_ms));
        }
        while (readable(fd, wait)) {
            wait = 0;
            const ssize_t n = recv(fd, datagram.data(), datagram.size(), 0);
            if (n <= 0)
                break;
            decoded_record rec;
            if (decode(std::span<const std::uint8_t>(datagram.data(), static_cast<std::size_t>(n)),
                       d_stream.record_limit_bytes, rec) != wire_error::none) {
                std::lock_guard<std::mutex> guard(d_mutex);
                ++d_counters.malformed_records;
                continue;
            }
            const auto now = clock_type::now();
            if (!have_base) {
                have_base = true;
                base = now + latency;
                base_index = rec.header.first_sample_index;
                std::lock_guard<std::mutex> guard(d_mutex);
                d_counters.connected = true;
            }
            const std::uint64_t seq = rec.header.record_sequence;
            if (any_released && seq <= last_released) {
                std::lock_guard<std::mutex> guard(d_mutex);
                ++d_counters.too_late_dropped; // its place in the output has passed
                continue;
            }
            if (buffer.count(seq)) {
                std::lock_guard<std::mutex> guard(d_mutex);
                ++d_counters.duplicates_dropped;
                continue;
            }
            // Nominal offset of this record from the first one, in the
            // sender's sample clock. Records before the first one are due at once.
            std::int64_t offset_ns = 0;
            bool exact = false;
            const std::uint64_t idx = rec.header.first_sample_index;
            if (idx > base_index)
                (void)sample_offset_ns(idx - base_index, d_stream.rate_num, d_stream.rate_den,
                                       offset_ns, exact);
            held h;
            h.bytes.assign(datagram.begin(), datagram.begin() + n);
            h.due = idx >= base_index ? base + std::chrono::nanoseconds(offset_ns) : now;
            buffer.emplace(seq, std::move(h));
            std::lock_guard<std::mutex> guard(d_mutex);
            d_counters.playout_high_water =
                std::max<std::uint64_t>(d_counters.playout_high_water, buffer.size());
        }
        // Release everything that is due, in sequence order.
        bool alive = true;
        while (alive && !buffer.empty() && buffer.begin()->second.due <= clock_type::now()) {
            auto it = buffer.begin();
            const bool is_end = it->second.bytes.size() == header_size && it->second.bytes[5] == 2;
            last_released = it->first;
            any_released = true;
            alive = accept_record(it->second.bytes.data(), it->second.bytes.size());
            buffer.erase(it);
            if (is_end && alive)
                end_released = true;
        }
        if (!alive)
            break;
    }
    close(fd);
}

void baseline_link::run_receive_tcp()
{
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    const int one = 1;
    (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    set_buffers(listener);
    const sockaddr_in local = address(d_transport.bind_address, d_transport.port);
    if (bind(listener, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0 ||
        listen(listener, 1) != 0) {
        set_failure("configuration", std::string("bind/listen: ") + std::strerror(errno));
        close(listener);
        return;
    }
    const auto started = clock_type::now();
    int fd = -1;
    while (!d_cancel.load() && fd < 0) {
        if (readable(listener, slice_ms))
            fd = accept(listener, nullptr, nullptr);
        else if (clock_type::now() - started >= d_transport.startup_timeout) {
            set_failure("startup_timeout", "no TCP peer within the startup timeout");
            break;
        }
    }
    close(listener);
    if (fd < 0)
        return;
    {
        std::lock_guard<std::mutex> guard(d_mutex);
        d_counters.connected = true;
        d_ledger.on_connection();
    }
    std::vector<std::uint8_t> stream;
    std::vector<std::uint8_t> chunk(65536);
    bool end_seen = false;
    while (!d_cancel.load()) {
        if (end_seen) {
            if (d_consumer_drained.load()) {
                std::lock_guard<std::mutex> guard(d_mutex);
                d_completed = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(slice_ms));
            continue;
        }
        if (!readable(fd, slice_ms))
            continue;
        const ssize_t n = recv(fd, chunk.data(), chunk.size(), 0);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
            set_failure("connection_lost", n == 0 ? "connection closed without END"
                                                  : std::strerror(errno));
            break;
        }
        if (n < 0)
            continue;
        stream.insert(stream.end(), chunk.begin(), chunk.begin() + n);
        // Records are self-delimiting: 96 bytes of header carry the payload length.
        std::size_t pos = 0;
        bool alive = true;
        while (alive && stream.size() - pos >= header_size) {
            const std::uint8_t* p = stream.data() + pos;
            const std::size_t payload = (static_cast<std::size_t>(p[12]) << 24) |
                                        (static_cast<std::size_t>(p[13]) << 16) |
                                        (static_cast<std::size_t>(p[14]) << 8) | p[15];
            if (std::memcmp(p, "RWSR", 4) != 0 ||
                header_size + payload > d_stream.record_limit_bytes) {
                set_failure("protocol", "TCP stream lost record framing");
                alive = false;
                break;
            }
            if (stream.size() - pos < header_size + payload)
                break;
            const bool is_end = p[5] == 2;
            alive = accept_record(p, header_size + payload);
            pos += header_size + payload;
            if (is_end && alive) {
                end_seen = true;
                break;
            }
        }
        if (!alive)
            break;
        stream.erase(stream.begin(), stream.begin() + static_cast<std::ptrdiff_t>(pos));
    }
    close(fd);
}

} // namespace rwsrt_peer
