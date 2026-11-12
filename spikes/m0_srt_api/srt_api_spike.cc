// SPDX-License-Identifier: GPL-3.0-or-later
//
// M0 integration spike for gr-robotweax-srt.
//
// Exercises the installed Robotweax SRT public C API only. Each check prints
// one JSON line; the process exits nonzero if any check fails. Measured
// durations are evidence for docs/decisions.md, not performance claims.

#include <srt/srt.h>
#include <srt/version.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using clock_type = std::chrono::steady_clock;

double ms_since(clock_type::time_point t0)
{
    return std::chrono::duration<double, std::milli>(clock_type::now() - t0).count();
}

int g_failures = 0;

void report(const char* check, bool pass, const std::string& detail_json)
{
    if (!pass)
        ++g_failures;
    std::printf("{\"check\":\"%s\",\"pass\":%s,\"detail\":%s}\n",
                check,
                pass ? "true" : "false",
                detail_json.empty() ? "{}" : detail_json.c_str());
    std::fflush(stdout);
}

sockaddr_in loopback(std::uint16_t port)
{
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    return sa;
}

std::uint16_t bound_port(SRTSOCKET s)
{
    sockaddr_in sa{};
    int len = sizeof(sa);
    if (srt_getsockname(s, reinterpret_cast<sockaddr*>(&sa), &len) == SRT_ERROR)
        return 0;
    return ntohs(sa.sin_port);
}

template <typename T>
bool set_opt(SRTSOCKET s, SRT_SOCKOPT opt, const T& value)
{
    return srt_setsockflag(s, opt, &value, sizeof(value)) != SRT_ERROR;
}

template <typename T>
bool get_opt(SRTSOCKET s, SRT_SOCKOPT opt, T& value)
{
    int len = sizeof(value);
    return srt_getsockflag(s, opt, &value, &len) != SRT_ERROR;
}

struct session_options {
    int latency_ms = 250;
    int payload_size = 1200;
    std::string passphrase; // empty: explicit plaintext
    std::string stream_id = "rwsrt/v1/default";
};

// The socket setup contract of plan section 8, applied before bind/connect.
// Returns an empty string on success, else the name of the failed setter.
std::string configure(SRTSOCKET s, const session_options& o)
{
    const SRT_TRANSTYPE tt = SRTT_LIVE;
    if (!set_opt(s, SRTO_TRANSTYPE, tt)) return "SRTO_TRANSTYPE";
    if (!set_opt(s, SRTO_MESSAGEAPI, true)) return "SRTO_MESSAGEAPI";
    if (!set_opt(s, SRTO_SNDSYN, false)) return "SRTO_SNDSYN";
    if (!set_opt(s, SRTO_RCVSYN, false)) return "SRTO_RCVSYN";
    if (!set_opt(s, SRTO_RCVLATENCY, o.latency_ms)) return "SRTO_RCVLATENCY";
    if (!set_opt(s, SRTO_PEERLATENCY, o.latency_ms)) return "SRTO_PEERLATENCY";
    if (!set_opt(s, SRTO_TSBPDMODE, true)) return "SRTO_TSBPDMODE";
    if (!set_opt(s, SRTO_TLPKTDROP, true)) return "SRTO_TLPKTDROP";
    if (!set_opt(s, SRTO_MSS, 1500)) return "SRTO_MSS";
    if (!set_opt(s, SRTO_PAYLOADSIZE, o.payload_size)) return "SRTO_PAYLOADSIZE";
    const linger no_linger{0, 0};
    if (!set_opt(s, SRTO_LINGER, no_linger)) return "SRTO_LINGER";
    if (srt_setsockflag(s, SRTO_STREAMID, o.stream_id.data(),
                        static_cast<int>(o.stream_id.size())) == SRT_ERROR)
        return "SRTO_STREAMID";
    if (!set_opt(s, SRTO_CONNTIMEO, 3000)) return "SRTO_CONNTIMEO";
    if (!set_opt(s, SRTO_PEERIDLETIMEO, 5000)) return "SRTO_PEERIDLETIMEO";
    if (!set_opt(s, SRTO_ENFORCEDENCRYPTION, true)) return "SRTO_ENFORCEDENCRYPTION";
    if (!o.passphrase.empty()) {
        if (!set_opt(s, SRTO_PBKEYLEN, 16)) return "SRTO_PBKEYLEN";
        if (srt_setsockflag(s, SRTO_PASSPHRASE, o.passphrase.data(),
                            static_cast<int>(o.passphrase.size())) == SRT_ERROR)
            return "SRTO_PASSPHRASE";
    }
    return {};
}

// Wait for `mask` on one socket in slices of at most 20 ms (plan section 9),
// up to `budget_ms`. Returns the event bits seen, or 0 on timeout.
int wait_events(int eid, int budget_ms, int* slices = nullptr)
{
    const auto t0 = clock_type::now();
    int n_slices = 0;
    int result = 0;
    while (ms_since(t0) < budget_ms) {
        SRT_EPOLL_EVENT ev[4];
        ++n_slices;
        const int n = srt_epoll_uwait(eid, ev, 4, 20);
        if (n > 0) {
            for (int i = 0; i < n; ++i)
                result |= ev[i].events;
            break;
        }
        if (n == SRT_ERROR)
            break;
    }
    if (slices)
        *slices = n_slices;
    return result;
}

struct pair_result {
    bool connected = false;
    SRTSOCKET listener = SRT_INVALID_SOCK;
    SRTSOCKET accepted = SRT_INVALID_SOCK;
    SRTSOCKET caller = SRT_INVALID_SOCK;
    int caller_events = 0;
    int reject_reason = 0;
    double connect_ms = 0;
    std::string error;
};

// Nonblocking listener + caller on loopback, driven only by public epoll.
pair_result connect_pair(const session_options& listener_opts,
                         const session_options& caller_opts,
                         int budget_ms = 4000)
{
    pair_result r;
    r.listener = srt_create_socket();
    r.caller = srt_create_socket();
    if (auto f = configure(r.listener, listener_opts); !f.empty()) { r.error = "listener " + f; return r; }
    if (auto f = configure(r.caller, caller_opts); !f.empty()) { r.error = "caller " + f; return r; }

    sockaddr_in any = loopback(0);
    if (srt_bind(r.listener, reinterpret_cast<sockaddr*>(&any), sizeof(any)) == SRT_ERROR ||
        srt_listen(r.listener, 1) == SRT_ERROR) { r.error = "bind/listen"; return r; }
    sockaddr_in dst = loopback(bound_port(r.listener));

    const int leid = srt_epoll_create();
    const int ceid = srt_epoll_create();
    const int lmask = SRT_EPOLL_IN | SRT_EPOLL_ERR;
    const int cmask = SRT_EPOLL_OUT | SRT_EPOLL_ERR;
    srt_epoll_add_usock(leid, r.listener, &lmask);
    srt_epoll_add_usock(ceid, r.caller, &cmask);

    const auto t0 = clock_type::now();
    // Nonblocking connect returns immediately; completion arrives as OUT/ERR.
    (void)srt_connect(r.caller, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));

    r.caller_events = wait_events(ceid, budget_ms);
    r.connect_ms = ms_since(t0);
    const bool caller_ok = (r.caller_events & SRT_EPOLL_OUT) && !(r.caller_events & SRT_EPOLL_ERR) &&
                           srt_getsockstate(r.caller) == SRTS_CONNECTED;
    if (caller_ok && (wait_events(leid, 1000) & SRT_EPOLL_IN)) {
        sockaddr_storage peer{};
        int plen = sizeof(peer);
        r.accepted = srt_accept(r.listener, reinterpret_cast<sockaddr*>(&peer), &plen);
    }
    if (!caller_ok)
        r.reject_reason = srt_getrejectreason(r.caller);
    r.connected = caller_ok && r.accepted != SRT_INVALID_SOCK;
    srt_epoll_release(leid);
    srt_epoll_release(ceid);
    return r;
}

void close_pair(pair_result& r)
{
    for (SRTSOCKET* s : { &r.accepted, &r.caller, &r.listener }) {
        if (*s != SRT_INVALID_SOCK) {
            srt_close(*s);
            *s = SRT_INVALID_SOCK;
        }
    }
}

// ---------------------------------------------------------------- checks

void check_identity()
{
    std::ostringstream os;
    const std::uint32_t v = srt_getversion();
    os << "{\"package_version\":\"" << RWSRT_FOUND_PACKAGE_VERSION
       << "\",\"header_robotweax_version\":\"" << ROBOTWEAX_SRT_VERSION_STRING
       << "\",\"header_compat_version\":\"" << SRT_VERSION_STRING
       << "\",\"srt_getversion\":\"" << ((v >> 16) & 0xff) << "." << ((v >> 8) & 0xff)
       << "." << (v & 0xff) << "\"}";
    // The package version, not srt_getversion(), identifies the product.
    report("identity", std::string(RWSRT_FOUND_PACKAGE_VERSION) == ROBOTWEAX_SRT_VERSION_STRING,
           os.str());
}

void check_startup_refcount()
{
    // Two references; release one; the runtime must still be usable.
    const int a = srt_startup();
    const int b = srt_startup();
    const int c1 = srt_cleanup();
    const SRTSOCKET s = srt_create_socket();
    const bool usable = s != SRT_INVALID_SOCK;
    if (usable)
        srt_close(s);
    const auto t0 = clock_type::now();
    const int c2 = srt_cleanup();
    const double final_ms = ms_since(t0);
    // And it can be started again afterwards (block restart in one process).
    const int again = srt_startup();
    const SRTSOCKET s2 = srt_create_socket();
    const bool restart_ok = again != SRT_ERROR && s2 != SRT_INVALID_SOCK;
    if (s2 != SRT_INVALID_SOCK)
        srt_close(s2);
    std::ostringstream os;
    os << "{\"startup\":[" << a << "," << b << "],\"cleanup\":[" << c1 << "," << c2
       << "],\"usable_after_first_cleanup\":" << (usable ? "true" : "false")
       << ",\"final_cleanup_ms\":" << final_ms << ",\"restart_ok\":"
       << (restart_ok ? "true" : "false") << "}";
    report("startup_refcount",
           a != SRT_ERROR && b != SRT_ERROR && c1 != SRT_ERROR && c2 != SRT_ERROR && usable &&
               restart_ok,
           os.str());
    // Leaves one reference held for the remaining checks.
}

void check_option_readback()
{
    const SRTSOCKET s = srt_create_socket();
    session_options o;
    const std::string failed = configure(s, o);
    std::ostringstream os;
    bool ok = failed.empty();
    os << "{\"failed_setter\":\"" << failed << "\"";
    auto rb_int = [&](const char* name, SRT_SOCKOPT opt, int expect, bool exact) {
        int v = -1;
        const bool got = get_opt(s, opt, v);
        os << ",\"" << name << "\":" << (got ? v : -9999);
        if (!got || (exact && v != expect))
            ok = false;
    };
    auto rb_bool = [&](const char* name, SRT_SOCKOPT opt, bool expect) {
        bool v = !expect;
        const bool got = get_opt(s, opt, v);
        os << ",\"" << name << "\":" << (v ? "true" : "false");
        if (!got || v != expect)
            ok = false;
    };
    rb_bool("messageapi", SRTO_MESSAGEAPI, true);
    rb_bool("sndsyn", SRTO_SNDSYN, false);
    rb_bool("rcvsyn", SRTO_RCVSYN, false);
    rb_bool("tsbpdmode", SRTO_TSBPDMODE, true);
    rb_bool("tlpktdrop", SRTO_TLPKTDROP, true);
    rb_bool("enforcedencryption", SRTO_ENFORCEDENCRYPTION, true);
    rb_int("rcvlatency", SRTO_RCVLATENCY, 250, true);
    rb_int("peerlatency", SRTO_PEERLATENCY, 250, true);
    rb_int("mss", SRTO_MSS, 1500, true);
    rb_int("payloadsize", SRTO_PAYLOADSIZE, 1200, true);
    rb_int("conntimeo", SRTO_CONNTIMEO, 3000, true);
    rb_int("peeridletimeo", SRTO_PEERIDLETIMEO, 5000, true);
    rb_int("sndbuf_default", SRTO_SNDBUF, 0, false);
    rb_int("rcvbuf_default", SRTO_RCVBUF, 0, false);
    rb_int("fc_default", SRTO_FC, 0, false);
    rb_int("udp_sndbuf_default", SRTO_UDP_SNDBUF, 0, false);
    rb_int("udp_rcvbuf_default", SRTO_UDP_RCVBUF, 0, false);
    linger lg{1, 1};
    const bool got_lg = get_opt(s, SRTO_LINGER, lg);
    os << ",\"linger\":[" << lg.l_onoff << "," << lg.l_linger << "]";
    if (!got_lg || lg.l_onoff != 0)
        ok = false;
    char sid[513] = {};
    int sid_len = sizeof(sid) - 1;
    const bool got_sid = srt_getsockflag(s, SRTO_STREAMID, sid, &sid_len) != SRT_ERROR;
    os << ",\"streamid\":\"" << sid << "\"";
    if (!got_sid || o.stream_id != sid)
        ok = false;
    os << "}";
    srt_close(s);
    report("option_readback", ok, os.str());
}

// Largest PAYLOADSIZE the setter accepts, and the largest single message a
// connected socket accepts, for one security mode.
void check_payload_ceiling(const char* name, const std::string& passphrase)
{
    std::ostringstream os;
    int setter_max = 0;
    for (int size = 1500; size >= 1000; --size) {
        const SRTSOCKET s = srt_create_socket();
        session_options o;
        o.passphrase = passphrase;
        o.payload_size = size;
        const bool ok = configure(s, o).empty();
        srt_close(s);
        if (ok) { setter_max = size; break; }
    }
    session_options o;
    o.passphrase = passphrase;
    o.payload_size = setter_max;
    pair_result p = connect_pair(o, o);
    int send_max = 0;
    int negotiated = -1;
    bool rec1200_ok = false;
    if (p.connected) {
        get_opt(p.caller, SRTO_PAYLOADSIZE, negotiated);
        std::vector<char> buf(1600, 'x');
        for (int size = 1500; size >= 1000; --size) {
            SRT_MSGCTRL mc;
            srt_msgctrl_init(&mc);
            const int n = srt_sendmsg2(p.caller, buf.data(), size, &mc);
            if (n == size) { send_max = size; break; }
        }
        SRT_MSGCTRL mc;
        srt_msgctrl_init(&mc);
        rec1200_ok = srt_sendmsg2(p.caller, buf.data(), 1200, &mc) == 1200;
    }
    os << "{\"setter_max_payloadsize\":" << setter_max << ",\"connected\":"
       << (p.connected ? "true" : "false") << ",\"effective_payloadsize\":" << negotiated
       << ",\"max_single_message_bytes\":" << send_max << ",\"record_1200_accepted\":"
       << (rec1200_ok ? "true" : "false") << "}";
    close_pair(p);
    report(name, p.error.empty() && send_max >= 1200 && rec1200_ok, os.str());
}

void check_loopback_transfer(const char* name, const std::string& passphrase)
{
    session_options o;
    o.passphrase = passphrase;
    pair_result p = connect_pair(o, o);
    std::ostringstream os;
    bool ok = p.connected;
    os << "{\"connected\":" << (p.connected ? "true" : "false") << ",\"connect_ms\":"
       << p.connect_ms << ",\"error\":\"" << p.error << "\"";
    if (p.connected) {
        // Effective values after the handshake.
        int rl = -1, pl = -1, ps = -1, kms = -1;
        get_opt(p.accepted, SRTO_RCVLATENCY, rl);
        get_opt(p.caller, SRTO_PEERLATENCY, pl);
        get_opt(p.accepted, SRTO_PAYLOADSIZE, ps);
        get_opt(p.accepted, SRTO_RCVKMSTATE, kms);
        char sid[513] = {};
        int sid_len = sizeof(sid) - 1;
        srt_getsockflag(p.accepted, SRTO_STREAMID, sid, &sid_len);

        // An empty nonblocking receive must be retryable, not blocking.
        std::vector<char> rx(2048);
        SRT_MSGCTRL rmc;
        srt_msgctrl_init(&rmc);
        const auto t_empty = clock_type::now();
        const int empty = srt_recvmsg2(p.accepted, rx.data(), static_cast<int>(rx.size()), &rmc);
        const int empty_code = srt_getlasterror(nullptr);
        const double empty_ms = ms_since(t_empty);
        const bool async_ok = empty == SRT_ERROR && empty_code == SRT_EASYNCRCV;

        // 64 distinct 1200-byte records, srctime 0 so SRT stamps submission.
        const int count = 64;
        int sent = 0;
        for (int i = 0; i < count; ++i) {
            std::vector<char> tx(1200, static_cast<char>(i));
            std::memcpy(tx.data(), "RWSR", 4);
            SRT_MSGCTRL mc;
            srt_msgctrl_init(&mc);
            mc.srctime = 0;
            if (srt_sendmsg2(p.caller, tx.data(), 1200, &mc) == 1200)
                ++sent;
        }
        const int reid = srt_epoll_create();
        const int rmask = SRT_EPOLL_IN | SRT_EPOLL_ERR;
        srt_epoll_add_usock(reid, p.accepted, &rmask);
        int received = 0;
        bool intact = true;
        const auto t0 = clock_type::now();
        double first_ms = -1;
        while (received < count && ms_since(t0) < 3000) {
            const int ev = wait_events(reid, 100);
            if (ev & SRT_EPOLL_ERR)
                break;
            for (;;) {
                srt_msgctrl_init(&rmc);
                const int n = srt_recvmsg2(p.accepted, rx.data(), static_cast<int>(rx.size()), &rmc);
                if (n == SRT_ERROR)
                    break;
                if (first_ms < 0)
                    first_ms = ms_since(t0);
                if (n != 1200 || std::memcmp(rx.data(), "RWSR", 4) != 0 ||
                    rx[1199] != static_cast<char>(received))
                    intact = false;
                ++received;
            }
        }
        SRT_TRACEBSTATS st{};
        const bool stats_ok = srt_bstats(p.caller, &st, 0) != SRT_ERROR;
        srt_epoll_release(reid);
        ok = ok && async_ok && sent == count && received == count && intact && stats_ok &&
             rl == 250 && std::string(sid) == o.stream_id;
        os << ",\"effective_rcvlatency\":" << rl << ",\"effective_peerlatency\":" << pl
           << ",\"effective_payloadsize\":" << ps << ",\"rcv_km_state\":" << kms
           << ",\"accepted_streamid\":\"" << sid << "\",\"empty_recv_is_easyncrcv\":"
           << (async_ok ? "true" : "false") << ",\"empty_recv_ms\":" << empty_ms
           << ",\"sent\":" << sent << ",\"received\":" << received << ",\"intact\":"
           << (intact ? "true" : "false") << ",\"first_delivery_ms\":" << first_ms
           << ",\"stats_rtt_ms\":" << st.msRTT << ",\"stats_pkt_sent\":" << st.pktSent;
    }
    os << "}";
    close_pair(p);
    report(name, ok, os.str());
}

// Caller with nobody listening: (a) cancel after 200 ms must return promptly,
// (b) left alone, failure must surface as an event near SRTO_CONNTIMEO.
void check_no_peer_caller()
{
    // Reserve then free a port so nothing answers on it.
    const SRTSOCKET probe = srt_create_socket();
    sockaddr_in any = loopback(0);
    srt_bind(probe, reinterpret_cast<sockaddr*>(&any), sizeof(any));
    const std::uint16_t port = bound_port(probe);
    srt_close(probe);
    sockaddr_in dst = loopback(port);
    const int mask = SRT_EPOLL_OUT | SRT_EPOLL_ERR;
    std::ostringstream os;

    // (a) cancellation
    SRTSOCKET s = srt_create_socket();
    configure(s, session_options{});
    int eid = srt_epoll_create();
    srt_epoll_add_usock(eid, s, &mask);
    auto t0 = clock_type::now();
    const int rc = srt_connect(s, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
    const double connect_call_ms = ms_since(t0);
    int slices = 0;
    const int ev_early = wait_events(eid, 200, &slices);
    const int state_pending = srt_getsockstate(s);
    t0 = clock_type::now();
    const int close_rc = srt_close(s);
    const int rel_rc = srt_epoll_release(eid);
    const double cancel_ms = ms_since(t0);
    os << "{\"connect_rc\":" << rc << ",\"connect_call_ms\":" << connect_call_ms
       << ",\"events_before_cancel\":" << ev_early << ",\"wait_slices_200ms\":" << slices
       << ",\"state_while_pending\":" << state_pending << ",\"cancel_ms\":" << cancel_ms;
    bool ok = connect_call_ms < 50 && ev_early == 0 && state_pending == SRTS_CONNECTING &&
              close_rc != SRT_ERROR && rel_rc != SRT_ERROR && cancel_ms < 100;

    // (b) natural timeout
    s = srt_create_socket();
    configure(s, session_options{});
    eid = srt_epoll_create();
    srt_epoll_add_usock(eid, s, &mask);
    t0 = clock_type::now();
    (void)srt_connect(s, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
    const int ev = wait_events(eid, 6000);
    const double timeout_ms = ms_since(t0);
    const int state_after = srt_getsockstate(s);
    const int reason = srt_getrejectreason(s);
    os << ",\"timeout_events\":" << ev << ",\"timeout_ms\":" << timeout_ms
       << ",\"state_after_timeout\":" << state_after << ",\"reject_reason\":" << reason
       << ",\"reject_text\":\"" << srt_rejectreason_str(reason) << "\"}";
    ok = ok && (ev & SRT_EPOLL_ERR) && timeout_ms > 2500 && timeout_ms < 4000;
    srt_close(s);
    srt_epoll_release(eid);
    report("no_peer_caller", ok, os.str());
}

void check_no_peer_listener()
{
    const SRTSOCKET s = srt_create_socket();
    configure(s, session_options{});
    sockaddr_in any = loopback(0);
    const bool up = srt_bind(s, reinterpret_cast<sockaddr*>(&any), sizeof(any)) != SRT_ERROR &&
                    srt_listen(s, 1) != SRT_ERROR;
    const int eid = srt_epoll_create();
    const int mask = SRT_EPOLL_IN | SRT_EPOLL_ERR;
    srt_epoll_add_usock(eid, s, &mask);
    int slices = 0;
    const int ev = wait_events(eid, 200, &slices);
    // Nonblocking accept with nothing pending must not block.
    auto t0 = clock_type::now();
    const SRTSOCKET a = srt_accept(s, nullptr, nullptr);
    const double accept_ms = ms_since(t0);
    const int accept_code = srt_getlasterror(nullptr);
    t0 = clock_type::now();
    srt_close(s);
    srt_epoll_release(eid);
    const double cancel_ms = ms_since(t0);
    std::ostringstream os;
    os << "{\"listening\":" << (up ? "true" : "false") << ",\"events\":" << ev
       << ",\"wait_slices_200ms\":" << slices << ",\"empty_accept_ms\":" << accept_ms
       << ",\"empty_accept_code\":" << accept_code << ",\"cancel_ms\":" << cancel_ms << "}";
    report("no_peer_listener",
           up && ev == 0 && a == SRT_INVALID_SOCK && accept_ms < 50 && cancel_ms < 100, os.str());
}

// A waiter blocked in epoll must be woken by release from another thread:
// this is the fallback cancellation path if a 20 ms slice were ever exceeded.
void check_epoll_release_wakes_waiter()
{
    const SRTSOCKET s = srt_create_socket();
    configure(s, session_options{});
    sockaddr_in any = loopback(0);
    srt_bind(s, reinterpret_cast<sockaddr*>(&any), sizeof(any));
    srt_listen(s, 1);
    const int eid = srt_epoll_create();
    const int mask = SRT_EPOLL_IN | SRT_EPOLL_ERR;
    srt_epoll_add_usock(eid, s, &mask);
    double waited_ms = 0;
    int wait_rc = 0;
    int wait_code = 0;
    std::thread waiter([&] {
        SRT_EPOLL_EVENT ev[2];
        const auto t0 = clock_type::now();
        wait_rc = srt_epoll_uwait(eid, ev, 2, 5000);
        wait_code = srt_getlasterror(nullptr);
        waited_ms = ms_since(t0);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const auto t0 = clock_type::now();
    srt_epoll_release(eid);
    waiter.join();
    const double wake_ms = ms_since(t0);
    srt_close(s);
    std::ostringstream os;
    os << "{\"wait_rc\":" << wait_rc << ",\"wait_error_code\":" << wait_code
       << ",\"waited_ms\":" << waited_ms << ",\"wake_after_release_ms\":" << wake_ms << "}";
    report("epoll_release_wakes_waiter", waited_ms < 1000 && wake_ms < 100, os.str());
}

// Peer disappears: how and when does the survivor learn about it?
void check_peer_close_detection()
{
    session_options o;
    pair_result p = connect_pair(o, o);
    std::ostringstream os;
    bool ok = p.connected;
    if (p.connected) {
        const int eid = srt_epoll_create();
        const int mask = SRT_EPOLL_IN | SRT_EPOLL_ERR;
        srt_epoll_add_usock(eid, p.accepted, &mask);
        const auto t0 = clock_type::now();
        srt_close(p.caller);
        p.caller = SRT_INVALID_SOCK;
        const int ev = wait_events(eid, 7000);
        const double detect_ms = ms_since(t0);
        // A buffer below the configured payload size is itself an API error
        // in Message mode, so both sizes are probed.
        char small[64];
        std::vector<char> full(2048);
        SRT_MSGCTRL mc;
        srt_msgctrl_init(&mc);
        const int n_small = srt_recvmsg2(p.accepted, small, sizeof(small), &mc);
        const int code_small = srt_getlasterror(nullptr);
        srt_msgctrl_init(&mc);
        const int n = srt_recvmsg2(p.accepted, full.data(), static_cast<int>(full.size()), &mc);
        const int code = srt_getlasterror(nullptr);
        const int state = srt_getsockstate(p.accepted);
        os << "{\"events\":" << ev << ",\"detect_ms\":" << detect_ms << ",\"recv_rc\":" << n
           << ",\"recv_error_code\":" << code << ",\"small_buffer_recv_rc\":" << n_small
           << ",\"small_buffer_error_code\":" << code_small << ",\"state\":" << state << "}";
        ok = (ev & SRT_EPOLL_ERR) && n == SRT_ERROR && code != SRT_EASYNCRCV;
        srt_epoll_release(eid);
    } else {
        os << "{\"error\":\"" << p.error << "\"}";
    }
    close_pair(p);
    report("peer_close_detection", ok, os.str());
}

// Wrong or missing key with enforced encryption: no connection, no data.
void check_credential_mismatch(const char* name, const std::string& listener_pw,
                               const std::string& caller_pw)
{
    session_options lo, co;
    lo.passphrase = listener_pw;
    co.passphrase = caller_pw;
    pair_result p = connect_pair(lo, co, 5000);
    int snd_km = -1;
    get_opt(p.caller, SRTO_SNDKMSTATE, snd_km);
    std::ostringstream os;
    os << "{\"connected\":" << (p.connected ? "true" : "false") << ",\"caller_events\":"
       << p.caller_events << ",\"elapsed_ms\":" << p.connect_ms << ",\"reject_reason\":"
       << p.reject_reason << ",\"reject_text\":\"" << srt_rejectreason_str(p.reject_reason)
       << "\",\"caller_state\":" << srt_getsockstate(p.caller) << "}";
    const bool ok = !p.connected && p.accepted == SRT_INVALID_SOCK;
    close_pair(p);
    report(name, ok, os.str());
}

} // namespace

int main()
{
    check_identity();
    check_startup_refcount(); // holds one runtime reference from here on
    check_option_readback();
    check_payload_ceiling("payload_ceiling_plaintext", "");
    check_payload_ceiling("payload_ceiling_ctr", "m0-spike-passphrase");
    check_loopback_transfer("loopback_transfer_plaintext", "");
    check_loopback_transfer("loopback_transfer_ctr", "m0-spike-passphrase");
    check_no_peer_caller();
    check_no_peer_listener();
    check_epoll_release_wakes_waiter();
    check_peer_close_detection();
    check_credential_mismatch("ctr_wrong_key_rejected", "m0-spike-passphrase", "other-passphrase-0");
    check_credential_mismatch("ctr_missing_key_rejected", "m0-spike-passphrase", "");
    check_credential_mismatch("ctr_unexpected_key_rejected", "", "m0-spike-passphrase");

    const auto t0 = clock_type::now();
    const int rc = srt_cleanup();
    std::ostringstream os;
    os << "{\"rc\":" << rc << ",\"ms\":" << ms_since(t0) << "}";
    report("final_cleanup", rc != SRT_ERROR && ms_since(t0) < 1000, os.str());

    std::printf("{\"summary\":{\"failures\":%d}}\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
