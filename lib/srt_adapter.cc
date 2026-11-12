/* -*- c++ -*- */
/*
 * Copyright 2026 Robotweax.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "srt_adapter.h"
#include "session_worker.h"

#include <srt/access_control.h>
#include <srt/srt.h>
#include <srt/version.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace rwsrt {

namespace {

template <typename T>
bool set_opt(SRTSOCKET s, SRT_SOCKOPT opt, const T& value)
{
    return srt_setsockflag(s, opt, &value, sizeof(value)) != SRT_ERROR;
}

template <typename T>
void get_opt(SRTSOCKET s, SRT_SOCKOPT opt, T& value)
{
    T v{};
    int len = sizeof(v);
    if (srt_getsockflag(s, opt, &v, &len) != SRT_ERROR)
        value = v;
}

sockaddr_in address(const std::string& ip, int port)
{
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(static_cast<std::uint16_t>(port));
    inet_pton(AF_INET, ip.c_str(), &sa.sin_addr);
    return sa;
}

connect_failure classify_reject(int reason)
{
    switch (reason) {
    case SRT_REJ_BADSECRET:
    case SRT_REJ_UNSECURE:
        return connect_failure::credentials;
    case SRT_REJ_MESSAGEAPI:
    case SRT_REJ_VERSION:
    case SRT_REJ_FILTER:
    case SRT_REJ_GROUP:
    case SRT_REJ_ROGUE:
        return connect_failure::configuration;
    default:
        break;
    }
    // Application rejections from a peer's admission check.
    switch (reason) {
    case SRT_REJX_BAD_REQUEST:
    case SRT_REJX_UNAUTHORIZED:
    case SRT_REJX_FORBIDDEN:
    case SRT_REJX_NOTFOUND: // this module: unexpected stream ID
    case SRT_REJX_BAD_MODE:
    case SRT_REJX_UNACCEPTABLE:
    case SRT_REJX_NOTSUP_MEDIA:
        return connect_failure::configuration;
    default:
        // Timeout, backlog, resource, close, a busy peer (conflict) and
        // unknown reasons can clear up without any local change.
        return connect_failure::transient;
    }
}

int listen_hook(void* opaque, SRTSOCKET socket, int /*handshake_version*/,
                const sockaddr* /*peer*/, const char* stream_id)
{
    return static_cast<srt_endpoint*>(opaque)->admit(socket, stream_id);
}

// The largest UDP receive buffer the kernel grants an unprivileged socket.
int kernel_udp_receive_limit()
{
    int value = -1;
    if (std::FILE* f = std::fopen("/proc/sys/net/core/rmem_max", "r")) {
        if (std::fscanf(f, "%d", &value) != 1)
            value = -1;
        std::fclose(f);
    }
    return value;
}

} // namespace

srt_endpoint::srt_endpoint(const transport_config& config, std::size_t record_limit_bytes,
                           std::int64_t offered_bw)
    : d_config(config),
      d_record_limit(record_limit_bytes),
      d_offered_bw(offered_bw),
      d_listener(SRT_INVALID_SOCK),
      d_connection(SRT_INVALID_SOCK),
      d_pending(SRT_INVALID_SOCK),
      d_epoll(SRT_ERROR)
{
}

srt_endpoint::~srt_endpoint() { close(); }

int srt_endpoint::admit(int socket, const char* stream_id)
{
    // Rejected inside the handshake, so the caller never sees a connection.
    if (d_config.stream_id != (stream_id ? stream_id : "")) {
        (void)srt_setrejectreason(socket, SRT_REJX_NOTFOUND);
        ++d_rejected;
        return -1;
    }
    if (d_busy.load()) {
        // One active peer per endpoint.
        (void)srt_setrejectreason(socket, SRT_REJX_CONFLICT);
        ++d_rejected;
        return -1;
    }
    return 0;
}

void srt_endpoint::set_error(const char* what)
{
    int sys = 0;
    d_error_code = srt_getlasterror(&sys);
    d_error = std::string(what) + ": " + srt_strerror(d_error_code, sys);
}

bool srt_endpoint::connected() const noexcept { return d_connection != SRT_INVALID_SOCK; }

// The socket setup contract of plan section 8. Every setter is checked.
bool srt_endpoint::configure(int s)
{
    const SRT_TRANSTYPE live = SRTT_LIVE;
    const linger no_linger{ 0, 0 };
    struct step {
        const char* name;
        bool ok;
    };
    const step steps[] = {
        { "SRTO_TRANSTYPE", set_opt(s, SRTO_TRANSTYPE, live) },
        { "SRTO_MESSAGEAPI", set_opt(s, SRTO_MESSAGEAPI, true) },
        { "SRTO_SNDSYN", set_opt(s, SRTO_SNDSYN, false) },
        { "SRTO_RCVSYN", set_opt(s, SRTO_RCVSYN, false) },
        { "SRTO_TSBPDMODE", set_opt(s, SRTO_TSBPDMODE, true) },
        { "SRTO_TLPKTDROP", set_opt(s, SRTO_TLPKTDROP, true) },
        { "SRTO_RCVLATENCY", set_opt(s, SRTO_RCVLATENCY, d_config.latency_ms) },
        { "SRTO_PEERLATENCY", set_opt(s, SRTO_PEERLATENCY, d_config.latency_ms) },
        { "SRTO_MSS", set_opt(s, SRTO_MSS, 1500) },
        { "SRTO_PAYLOADSIZE", set_opt(s, SRTO_PAYLOADSIZE, static_cast<int>(d_record_limit)) },
        { "SRTO_SNDBUF", set_opt(s, SRTO_SNDBUF, d_config.srt_buffer_bytes) },
        { "SRTO_RCVBUF", set_opt(s, SRTO_RCVBUF, d_config.srt_buffer_bytes) },
        { "SRTO_LINGER", set_opt(s, SRTO_LINGER, no_linger) },
        { "SRTO_CONNTIMEO", set_opt(s, SRTO_CONNTIMEO, d_config.connect_timeout_ms) },
        { "SRTO_PEERIDLETIMEO", set_opt(s, SRTO_PEERIDLETIMEO, d_config.peer_idle_timeout_ms) },
        { "SRTO_ENFORCEDENCRYPTION", set_opt(s, SRTO_ENFORCEDENCRYPTION, true) },
        { "SRTO_STREAMID",
          srt_setsockflag(s, SRTO_STREAMID, d_config.stream_id.data(),
                          static_cast<int>(d_config.stream_id.size())) != SRT_ERROR },
    };
    for (const step& st : steps) {
        if (!st.ok) {
            d_error = std::string("setting ") + st.name + " failed";
            d_error_code = srt_getlasterror(nullptr);
            return false;
        }
    }
    // Request no more UDP receive buffer than the kernel grants; a larger
    // request is silently capped and only shows up as a library warning.
    d_udp_rcv_limit = kernel_udp_receive_limit();
    constexpr int wanted_udp_receive = 12288000; // the library's own default
    const int udp_receive =
        d_udp_rcv_limit > 0 ? std::min(wanted_udp_receive, d_udp_rcv_limit) : wanted_udp_receive;
    if (!set_opt(s, SRTO_UDP_RCVBUF, udp_receive)) {
        set_error("setting SRTO_UDP_RCVBUF");
        return false;
    }
    if (d_offered_bw > 0) {
        // Pace from the declared input rate plus the library's overhead
        // reserve (SRTO_OHEADBW, read back in effective()).
        const std::int64_t zero = 0;
        if (!set_opt(s, SRTO_MAXBW, zero) || !set_opt(s, SRTO_INPUTBW, d_offered_bw)) {
            set_error("setting SRTO_MAXBW/SRTO_INPUTBW");
            return false;
        }
    }
    if (d_config.security == security_mode::ctr) {
        if (!set_opt(s, SRTO_PBKEYLEN, d_config.key_length) ||
            srt_setsockflag(s, SRTO_PASSPHRASE, d_config.passphrase.data(),
                            static_cast<int>(d_config.passphrase.size())) == SRT_ERROR) {
            d_error = "setting the encryption key failed"; // never echo the secret
            d_error_code = srt_getlasterror(nullptr);
            return false;
        }
    }
    return true;
}

bool srt_endpoint::open()
{
    d_failure = connect_failure::none;
    if (d_epoll == SRT_ERROR) {
        d_epoll = srt_epoll_create();
        if (d_epoll == SRT_ERROR) {
            set_error("srt_epoll_create");
            return false;
        }
    }
    if (d_config.role == connection_role::listener) {
        if (d_listener != SRT_INVALID_SOCK)
            return true; // still listening from before
        const SRTSOCKET s = srt_create_socket();
        if (s == SRT_INVALID_SOCK) {
            set_error("srt_create_socket");
            return false;
        }
        sockaddr_in sa = address(d_config.bind_address, d_config.port);
        const int mask = SRT_EPOLL_IN | SRT_EPOLL_ERR;
        if (!configure(s)) {
            (void)srt_close(s);
            return false;
        }
        if (srt_listen_callback(s, &listen_hook, this) == SRT_ERROR ||
            srt_bind(s, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == SRT_ERROR ||
            srt_listen(s, 2) == SRT_ERROR ||
            srt_epoll_add_usock(d_epoll, s, &mask) == SRT_ERROR) {
            set_error("bind/listen");
            (void)srt_close(s);
            return false;
        }
        d_listener = s;
        return true;
    }

    if (d_pending != SRT_INVALID_SOCK) {
        (void)srt_epoll_remove_usock(d_epoll, d_pending);
        (void)srt_close(d_pending);
        d_pending = SRT_INVALID_SOCK;
    }
    const SRTSOCKET s = srt_create_socket();
    if (s == SRT_INVALID_SOCK) {
        set_error("srt_create_socket");
        return false;
    }
    sockaddr_in sa = address(d_config.host, d_config.port);
    const int mask = SRT_EPOLL_OUT | SRT_EPOLL_ERR;
    if (!configure(s)) {
        (void)srt_close(s);
        return false;
    }
    if (srt_epoll_add_usock(d_epoll, s, &mask) == SRT_ERROR) {
        set_error("srt_epoll_add_usock");
        (void)srt_close(s);
        return false;
    }
    // Nonblocking: completion or failure arrives as an epoll event.
    (void)srt_connect(s, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
    d_pending = s;
    return true;
}

void srt_endpoint::reject(int socket)
{
    // Fallback: admission normally happens in admit(), during the handshake.
    ++d_rejected;
    (void)srt_close(socket);
}

connect_progress srt_endpoint::poll(int slice_ms)
{
    if (connected())
        return connect_progress::connected;
    SRT_EPOLL_EVENT events[4];
    const int n = srt_epoll_uwait(d_epoll, events, 4, slice_ms);
    if (n == SRT_ERROR) {
        set_error("srt_epoll_uwait");
        d_failure = connect_failure::transient;
        return connect_progress::failed;
    }
    for (int i = 0; i < n; ++i) {
        const SRTSOCKET s = events[i].fd;
        if (s == d_listener && d_listener != SRT_INVALID_SOCK) {
            if (events[i].events & SRT_EPOLL_ERR) {
                set_error("listener failed");
                d_failure = connect_failure::transient;
                (void)srt_epoll_remove_usock(d_epoll, d_listener);
                (void)srt_close(d_listener);
                d_listener = SRT_INVALID_SOCK;
                return connect_progress::failed;
            }
            const SRTSOCKET peer = srt_accept(d_listener, nullptr, nullptr);
            if (peer == SRT_INVALID_SOCK)
                continue; // spurious readiness: retry on the next slice
            char sid[513] = {};
            int sid_len = sizeof(sid) - 1;
            (void)srt_getsockflag(peer, SRTO_STREAMID, sid, &sid_len);
            if (d_config.stream_id != sid) {
                reject(peer); // unexpected identity
                continue;
            }
            const int mask = SRT_EPOLL_IN | SRT_EPOLL_ERR;
            if (srt_epoll_add_usock(d_epoll, peer, &mask) == SRT_ERROR) {
                reject(peer);
                continue;
            }
            d_connection = peer;
            d_busy.store(true);
            return connect_progress::connected;
        }
        if (s == d_pending && d_pending != SRT_INVALID_SOCK) {
            const bool ok = (events[i].events & SRT_EPOLL_OUT) &&
                            !(events[i].events & SRT_EPOLL_ERR) &&
                            srt_getsockstate(s) == SRTS_CONNECTED;
            if (ok) {
                const int mask = SRT_EPOLL_IN | SRT_EPOLL_ERR;
                (void)srt_epoll_update_usock(d_epoll, s, &mask);
                d_connection = s;
                d_busy.store(true);
                d_pending = SRT_INVALID_SOCK;
                return connect_progress::connected;
            }
            const int reason = srt_getrejectreason(s);
            d_failure = classify_reject(reason);
            // Name this module's own admission reasons; the library only
            // knows them as "application-defined".
            const char* why = reason == SRT_REJX_NOTFOUND
                                  ? "the peer does not serve this stream_id"
                                  : reason == SRT_REJX_CONFLICT
                                        ? "the peer already has a connection"
                                        : srt_rejectreason_str(reason);
            d_error = std::string("connection attempt failed: ") + why;
            d_error_code = reason;
            (void)srt_epoll_remove_usock(d_epoll, s);
            (void)srt_close(s);
            d_pending = SRT_INVALID_SOCK;
            return connect_progress::failed;
        }
    }
    return connect_progress::pending;
}

unsigned srt_endpoint::wait(int slice_ms, bool want_read, bool want_write)
{
    if (!connected())
        return wait_broken;
    int mask = SRT_EPOLL_ERR | (want_read ? SRT_EPOLL_IN : 0) | (want_write ? SRT_EPOLL_OUT : 0);
    (void)srt_epoll_update_usock(d_epoll, d_connection, &mask);
    SRT_EPOLL_EVENT events[4];
    const int n = srt_epoll_uwait(d_epoll, events, 4, slice_ms);
    if (n == SRT_ERROR)
        return wait_broken;
    unsigned result = wait_timeout;
    for (int i = 0; i < n; ++i) {
        if (events[i].fd == d_connection) {
            if (events[i].events & SRT_EPOLL_IN)
                result |= wait_readable; // buffered data is read before a break is acted on
            if (events[i].events & SRT_EPOLL_OUT)
                result |= wait_writable;
            if (events[i].events & SRT_EPOLL_ERR)
                result |= wait_broken;
        } else if (events[i].fd == d_listener && d_listener != SRT_INVALID_SOCK) {
            // One active peer per endpoint: anything else is closed at once.
            const SRTSOCKET extra = srt_accept(d_listener, nullptr, nullptr);
            if (extra != SRT_INVALID_SOCK)
                reject(extra);
        }
    }
    return result;
}

io_result srt_endpoint::send(const std::uint8_t* data, std::size_t size)
{
    if (!connected())
        return io_result::lost;
    SRT_MSGCTRL mc;
    srt_msgctrl_init(&mc);
    mc.srctime = 0; // SRT stamps submission time; capture time is in the record
    const int n = srt_sendmsg2(d_connection, reinterpret_cast<const char*>(data),
                               static_cast<int>(size), &mc);
    if (n == static_cast<int>(size))
        return io_result::ok;
    const int code = srt_getlasterror(nullptr);
    if (n == SRT_ERROR && code == SRT_EASYNCSND)
        return io_result::would_block;
    set_error("srt_sendmsg2");
    if (code == SRT_ECONNLOST || code == SRT_ENOCONN || code == SRT_EINVSOCK)
        return io_result::lost;
    return io_result::error;
}

io_result srt_endpoint::receive(std::uint8_t* buffer, std::size_t capacity, std::size_t& size)
{
    size = 0;
    if (!connected())
        return io_result::lost;
    SRT_MSGCTRL mc;
    srt_msgctrl_init(&mc);
    const int n = srt_recvmsg2(d_connection, reinterpret_cast<char*>(buffer),
                               static_cast<int>(capacity), &mc);
    if (n >= 0) {
        size = static_cast<std::size_t>(n);
        return io_result::ok;
    }
    const int code = srt_getlasterror(nullptr);
    if (code == SRT_EASYNCRCV)
        return io_result::would_block;
    set_error("srt_recvmsg2");
    if (code == SRT_ECONNLOST || code == SRT_ENOCONN || code == SRT_EINVSOCK)
        return io_result::lost;
    return io_result::error;
}

int srt_endpoint::unacknowledged_packets()
{
    if (!connected())
        return -1;
    std::int32_t v = -1;
    get_opt(d_connection, SRTO_SNDDATA, v);
    return v;
}

void srt_endpoint::close_connection()
{
    if (d_connection != SRT_INVALID_SOCK) {
        if (d_epoll != SRT_ERROR)
            (void)srt_epoll_remove_usock(d_epoll, d_connection);
        (void)srt_close(d_connection);
        d_connection = SRT_INVALID_SOCK;
        d_busy.store(false);
    }
    if (d_pending != SRT_INVALID_SOCK) {
        if (d_epoll != SRT_ERROR)
            (void)srt_epoll_remove_usock(d_epoll, d_pending);
        (void)srt_close(d_pending);
        d_pending = SRT_INVALID_SOCK;
    }
}

void srt_endpoint::close()
{
    close_connection();
    if (d_listener != SRT_INVALID_SOCK) {
        if (d_epoll != SRT_ERROR)
            (void)srt_epoll_remove_usock(d_epoll, d_listener);
        (void)srt_close(d_listener);
        d_listener = SRT_INVALID_SOCK;
    }
    if (d_epoll != SRT_ERROR) {
        (void)srt_epoll_release(d_epoll);
        d_epoll = SRT_ERROR;
    }
}

srt_effective srt_endpoint::effective()
{
    srt_effective e;
    const SRTSOCKET s = connected() ? d_connection
                                    : (d_pending != SRT_INVALID_SOCK ? d_pending : d_listener);
    if (s == SRT_INVALID_SOCK)
        return e;
    get_opt(s, SRTO_RCVLATENCY, e.rcv_latency_ms);
    get_opt(s, SRTO_PEERLATENCY, e.peer_latency_ms);
    get_opt(s, SRTO_PAYLOADSIZE, e.payload_size);
    get_opt(s, SRTO_MSS, e.mss);
    get_opt(s, SRTO_SNDBUF, e.snd_buf_bytes);
    get_opt(s, SRTO_RCVBUF, e.rcv_buf_bytes);
    get_opt(s, SRTO_FC, e.flow_window_packets);
    get_opt(s, SRTO_UDP_SNDBUF, e.udp_snd_buf_bytes);
    get_opt(s, SRTO_UDP_RCVBUF, e.udp_rcv_buf_bytes);
    e.udp_rcv_buf_limit_bytes = d_udp_rcv_limit;
    get_opt(s, SRTO_MAXBW, e.max_bw);
    get_opt(s, SRTO_INPUTBW, e.input_bw);
    get_opt(s, SRTO_OHEADBW, e.overhead_bw_percent);
    get_opt(s, SRTO_PBKEYLEN, e.key_length);
    get_opt(s, SRTO_KMSTATE, e.km_state);
    char sid[513] = {};
    int sid_len = sizeof(sid) - 1;
    if (srt_getsockflag(s, SRTO_STREAMID, sid, &sid_len) != SRT_ERROR)
        e.peer_stream_id = sid;
    return e;
}

srt_link_stats srt_endpoint::stats()
{
    srt_link_stats out;
    if (!connected())
        return out;
    SRT_TRACEBSTATS st{};
    if (srt_bstats(d_connection, &st, 0) == SRT_ERROR)
        return out;
    out.valid = true;
    out.rtt_ms = st.msRTT;
    out.packets_sent = st.pktSentTotal;
    out.packets_received = st.pktRecvTotal;
    out.send_loss = st.pktSndLossTotal;
    out.receive_loss = st.pktRcvLossTotal;
    out.retransmitted = st.pktRetransTotal;
    out.send_dropped = st.pktSndDropTotal;
    out.receive_dropped = st.pktRcvDropTotal;
    out.undecrypted = st.pktRcvUndecryptTotal;
    out.send_buffer_packets = st.pktSndBuf;
    out.receive_buffer_packets = st.pktRcvBuf;
    return out;
}

std::string srt_library_version() { return ROBOTWEAX_SRT_VERSION_STRING; }

} // namespace rwsrt
