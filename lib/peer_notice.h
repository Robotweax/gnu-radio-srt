/* -*- c++ -*- */
/*
 * Copyright 2026 Robotweax.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INCLUDED_RWSRT_PEER_NOTICE_H
#define INCLUDED_RWSRT_PEER_NOTICE_H

// Receiver notice: one small message that a receiver sends back to the
// sender, on the same SRT connection, when it ends the session because of
// what it received. Without it the sender only sees the connection close.
// Normative description: docs/protocol-v1.md, "Receiver notice".
// Pure C++: no GNU Radio and no socket dependency.

#include "session_state.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace rwsrt {

inline constexpr std::size_t notice_header_size = 8;
inline constexpr std::size_t notice_max_detail = 200;
inline constexpr std::uint8_t notice_version = 1;

//! Why the receiver ended the session. Wire values; never renumber.
enum class notice_reason : std::uint8_t {
    configuration = 1, //!< sample format or rate differs from the receiver's
    protocol = 2,      //!< the receiver could not accept a record
    continuity = 3,    //!< a strict receiver saw a loss
    queue_overflow = 4 //!< the receiver's own queue overflowed
};

struct peer_notice {
    notice_reason reason = notice_reason::protocol;
    std::string detail; //!< printable ASCII, at most notice_max_detail bytes
};

//! The notice for a terminal failure of a receiver, or false if that kind
//! of failure is not reported to the sender.
bool notice_for(session_failure failure, const std::string& detail, peer_notice& out);
session_failure to_session_failure(notice_reason r) noexcept;

//! Encodes one message. The detail is truncated and made printable.
std::vector<std::uint8_t> encode(const peer_notice& n);

//! Decodes one complete message. False for anything that is not exactly one
//! well-formed notice. Never reads outside \p message.
bool decode(std::span<const std::uint8_t> message, peer_notice& out);

} // namespace rwsrt

#endif /* INCLUDED_RWSRT_PEER_NOTICE_H */
