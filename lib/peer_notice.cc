/* -*- c++ -*- */
/*
 * Copyright 2026 Robotweax.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "peer_notice.h"

#include <algorithm>

namespace rwsrt {

namespace {

// "RWSN": distinct from the "RWSR" magic of a record, so that neither can be
// taken for the other.
constexpr std::uint8_t magic[4] = { 'R', 'W', 'S', 'N' };

char printable(char c) noexcept { return (c >= 0x20 && c <= 0x7e) ? c : '?'; }

} // namespace

bool notice_for(session_failure failure, const std::string& detail, peer_notice& out)
{
    switch (failure) {
    case session_failure::configuration: out.reason = notice_reason::configuration; break;
    case session_failure::protocol: out.reason = notice_reason::protocol; break;
    case session_failure::continuity: out.reason = notice_reason::continuity; break;
    case session_failure::queue_overflow: out.reason = notice_reason::queue_overflow; break;
    default: return false;
    }
    out.detail = detail;
    return true;
}

session_failure to_session_failure(notice_reason r) noexcept
{
    switch (r) {
    case notice_reason::configuration: return session_failure::configuration;
    case notice_reason::protocol: return session_failure::protocol;
    case notice_reason::continuity: return session_failure::continuity;
    case notice_reason::queue_overflow: return session_failure::queue_overflow;
    }
    return session_failure::protocol;
}

std::vector<std::uint8_t> encode(const peer_notice& n)
{
    const std::size_t len = std::min(n.detail.size(), notice_max_detail);
    std::vector<std::uint8_t> out(notice_header_size + len);
    std::copy(std::begin(magic), std::end(magic), out.begin());
    out[4] = notice_version;
    out[5] = static_cast<std::uint8_t>(n.reason);
    out[6] = static_cast<std::uint8_t>(len >> 8);
    out[7] = static_cast<std::uint8_t>(len & 0xff);
    for (std::size_t i = 0; i < len; ++i)
        out[notice_header_size + i] = static_cast<std::uint8_t>(printable(n.detail[i]));
    return out;
}

bool decode(std::span<const std::uint8_t> message, peer_notice& out)
{
    if (message.size() < notice_header_size ||
        message.size() > notice_header_size + notice_max_detail)
        return false;
    if (!std::equal(std::begin(magic), std::end(magic), message.begin()))
        return false;
    if (message[4] != notice_version)
        return false;
    const std::uint8_t reason = message[5];
    if (reason < 1 || reason > 4)
        return false;
    const std::size_t len = (static_cast<std::size_t>(message[6]) << 8) | message[7];
    if (len != message.size() - notice_header_size)
        return false;
    out.reason = static_cast<notice_reason>(reason);
    out.detail.assign(len, '?');
    for (std::size_t i = 0; i < len; ++i)
        out.detail[i] = printable(static_cast<char>(message[notice_header_size + i]));
    return true;
}

} // namespace rwsrt
