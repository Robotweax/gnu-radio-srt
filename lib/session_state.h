/* -*- c++ -*- */
/*
 * Copyright 2026 Robotweax.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INCLUDED_RWSRT_SESSION_STATE_H
#define INCLUDED_RWSRT_SESSION_STATE_H

// Session state machine and retry timing (plan section 9). Pure logic: the
// worker reports events and asks what state it is in.

#include "continuity.h"

#include <chrono>
#include <cstdint>
#include <optional>

namespace rwsrt {

enum class connection_role { caller, listener };

enum class session_state {
    stopped,
    starting,
    connecting,
    listening,
    connected,
    retry_wait,
    draining,
    completed,
    stopping,
    failed
};

enum class session_failure {
    none = 0,
    startup_timeout,   //!< no peer within the startup deadline
    reconnect_timeout, //!< outage outlived the reconnect budget
    connection_lost,   //!< strict mode, or reconnect disabled
    credentials,       //!< passphrase missing, wrong or unexpected
    configuration,     //!< local or peer configuration cannot work
    protocol,          //!< peer sent something the protocol forbids
    continuity,        //!< strict mode continuity loss
    queue_overflow,    //!< strict mode: local queue full or record expired
    transport,         //!< unexpected SRT API failure
    internal
};

const char* to_string(session_state s) noexcept;
const char* to_string(session_failure f) noexcept;

//! Whether a failed connection attempt may be retried.
enum class attempt_failure { transient, terminal };

/*!
 * Every method returns false, changing nothing, when the event is not valid
 * in the current state. A terminal failure is retained until the next
 * start() and is never overwritten by a later one.
 *
 * Interpretation of the plan's diagram, recorded in docs/decisions.md:
 * before the first connection, failed attempts are retried in both
 * continuity modes, bounded by the startup deadline. After a connection was
 * established, a loss is retried only when reconnect is enabled, which
 * strict mode never is.
 */
class session_fsm
{
public:
    session_fsm(connection_role role, continuity_policy policy, bool reconnect_enabled);

    session_state state() const noexcept { return d_state; }
    session_failure failure() const noexcept { return d_failure; }
    std::uint64_t connection_generation() const noexcept { return d_generation; }
    bool ever_connected() const noexcept { return d_generation != 0; }
    bool reconnect_allowed() const noexcept { return d_reconnect; }
    //! Not stopped, completed or failed.
    bool active() const noexcept;

    bool start();
    bool on_connected();
    bool on_attempt_failed(attempt_failure kind, session_failure terminal_reason);
    bool on_retry_elapsed();
    bool on_connection_lost();
    bool on_end_of_stream();
    bool on_drained();
    bool on_startup_deadline();
    bool on_reconnect_budget_exhausted();
    bool on_fatal(session_failure reason);
    bool request_stop();
    bool on_stopped();

private:
    session_state attempt_state() const noexcept;
    void fail(session_failure reason) noexcept;

    const connection_role d_role;
    const bool d_reconnect;
    session_state d_state = session_state::stopped;
    session_failure d_failure = session_failure::none;
    std::uint64_t d_generation = 0;
};

struct retry_config {
    std::chrono::milliseconds initial{ 250 };
    std::chrono::milliseconds maximum{ 4000 };
    double jitter = 0.2; //!< each delay is scaled by a factor in [1-j, 1+j]
    std::chrono::milliseconds total_budget{ 60000 };
};

//! Exponential backoff with jitter inside one wall-time budget.
class retry_schedule
{
public:
    using clock = std::chrono::steady_clock;

    retry_schedule(const retry_config& config, std::uint64_t seed);

    //! An outage (or the startup phase) begins now.
    void begin(clock::time_point now) noexcept;
    //! Delay before the next attempt, or nullopt when waiting that long
    //! would exceed the budget.
    std::optional<std::chrono::nanoseconds> next_delay(clock::time_point now) noexcept;
    bool budget_exhausted(clock::time_point now) const noexcept;
    std::uint32_t attempts() const noexcept { return d_attempts; }

private:
    double uniform() noexcept; //!< in [0, 1)

    const retry_config d_config;
    std::uint64_t d_rng;
    clock::time_point d_began{};
    std::uint32_t d_attempts = 0;
};

} // namespace rwsrt

#endif /* INCLUDED_RWSRT_SESSION_STATE_H */
