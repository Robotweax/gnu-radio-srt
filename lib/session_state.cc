/* -*- c++ -*- */
/*
 * Copyright 2026 Robotweax.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "session_state.h"

#include <algorithm>

namespace rwsrt {

const char* to_string(session_state s) noexcept
{
    switch (s) {
    case session_state::stopped: return "stopped";
    case session_state::starting: return "starting";
    case session_state::connecting: return "connecting";
    case session_state::listening: return "listening";
    case session_state::connected: return "connected";
    case session_state::retry_wait: return "retry_wait";
    case session_state::draining: return "draining";
    case session_state::completed: return "completed";
    case session_state::stopping: return "stopping";
    case session_state::failed: return "failed";
    }
    return "unknown";
}

const char* to_string(session_failure f) noexcept
{
    switch (f) {
    case session_failure::none: return "none";
    case session_failure::startup_timeout: return "startup_timeout";
    case session_failure::reconnect_timeout: return "reconnect_timeout";
    case session_failure::connection_lost: return "connection_lost";
    case session_failure::credentials: return "credentials";
    case session_failure::configuration: return "configuration";
    case session_failure::protocol: return "protocol";
    case session_failure::continuity: return "continuity";
    case session_failure::queue_overflow: return "queue_overflow";
    case session_failure::transport: return "transport";
    case session_failure::internal: return "internal";
    }
    return "unknown";
}

session_fsm::session_fsm(connection_role role, continuity_policy policy, bool reconnect_enabled)
    : d_role(role),
      d_reconnect(reconnect_enabled && policy == continuity_policy::segmented_live)
{
}

bool session_fsm::active() const noexcept
{
    return d_state != session_state::stopped && d_state != session_state::completed &&
           d_state != session_state::failed;
}

session_state session_fsm::attempt_state() const noexcept
{
    return d_role == connection_role::caller ? session_state::connecting
                                             : session_state::listening;
}

void session_fsm::fail(session_failure reason) noexcept
{
    d_state = session_state::failed;
    if (d_failure == session_failure::none)
        d_failure = reason;
}

bool session_fsm::start()
{
    if (d_state != session_state::stopped)
        return false;
    d_failure = session_failure::none;
    d_generation = 0;
    // STARTING is observable only inside this call: resources are acquired
    // by the caller before start(), so the first attempt begins at once.
    d_state = session_state::starting;
    d_state = attempt_state();
    return true;
}

bool session_fsm::on_connected()
{
    if (d_state != session_state::connecting && d_state != session_state::listening)
        return false;
    ++d_generation;
    d_state = session_state::connected;
    return true;
}

bool session_fsm::on_attempt_failed(attempt_failure kind, session_failure terminal_reason)
{
    if (d_state != session_state::connecting && d_state != session_state::listening)
        return false;
    if (kind == attempt_failure::terminal)
        fail(terminal_reason == session_failure::none ? session_failure::transport
                                                      : terminal_reason);
    else
        d_state = session_state::retry_wait;
    return true;
}

bool session_fsm::on_retry_elapsed()
{
    if (d_state != session_state::retry_wait)
        return false;
    d_state = attempt_state();
    return true;
}

bool session_fsm::on_connection_lost()
{
    if (d_state == session_state::draining)
        return true; // the stream already ended; the loss changes nothing
    if (d_state != session_state::connected)
        return false;
    if (d_reconnect)
        d_state = session_state::retry_wait;
    else
        fail(session_failure::connection_lost);
    return true;
}

bool session_fsm::on_end_of_stream()
{
    if (d_state != session_state::connected)
        return false;
    d_state = session_state::draining;
    return true;
}

bool session_fsm::on_drained()
{
    if (d_state != session_state::draining)
        return false;
    d_state = session_state::completed;
    return true;
}

bool session_fsm::on_startup_deadline()
{
    const bool waiting = d_state == session_state::connecting ||
                         d_state == session_state::listening ||
                         d_state == session_state::retry_wait;
    if (!waiting || ever_connected())
        return false;
    fail(session_failure::startup_timeout);
    return true;
}

bool session_fsm::on_reconnect_budget_exhausted()
{
    const bool waiting = d_state == session_state::connecting ||
                         d_state == session_state::listening ||
                         d_state == session_state::retry_wait;
    if (!waiting || !ever_connected())
        return false;
    fail(session_failure::reconnect_timeout);
    return true;
}

bool session_fsm::on_fatal(session_failure reason)
{
    if (!active() || d_state == session_state::stopping || reason == session_failure::none)
        return false;
    fail(reason);
    return true;
}

bool session_fsm::request_stop()
{
    if (d_state == session_state::stopped || d_state == session_state::stopping)
        return false;
    // COMPLETED and FAILED still own resources until the worker has stopped.
    d_state = session_state::stopping;
    return true;
}

bool session_fsm::on_stopped()
{
    if (d_state != session_state::stopping)
        return false;
    d_state = session_state::stopped;
    return true;
}

retry_schedule::retry_schedule(const retry_config& config, std::uint64_t seed)
    : d_config(config), d_rng(seed ? seed : 0x9e3779b97f4a7c15ull)
{
}

double retry_schedule::uniform() noexcept
{
    // splitmix64: small, deterministic for a given seed, good enough for jitter.
    d_rng += 0x9e3779b97f4a7c15ull;
    std::uint64_t z = d_rng;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    z ^= z >> 31;
    return static_cast<double>(z >> 11) / 9007199254740992.0; // 2^53
}

void retry_schedule::begin(clock::time_point now) noexcept
{
    d_began = now;
    d_attempts = 0;
}

bool retry_schedule::budget_exhausted(clock::time_point now) const noexcept
{
    return now - d_began >= d_config.total_budget;
}

std::optional<std::chrono::nanoseconds> retry_schedule::next_delay(clock::time_point now) noexcept
{
    using ns = std::chrono::nanoseconds;
    const double cap = static_cast<double>(std::chrono::duration_cast<ns>(d_config.maximum).count());
    double base = static_cast<double>(std::chrono::duration_cast<ns>(d_config.initial).count());
    for (std::uint32_t i = 0; i < d_attempts && base < cap; ++i)
        base *= 2.0;
    base = std::min(base, cap);
    const double jitter = std::clamp(d_config.jitter, 0.0, 1.0);
    const double factor = 1.0 - jitter + 2.0 * jitter * uniform();
    const ns delay(static_cast<std::int64_t>(base * factor));
    if ((now - d_began) + delay >= d_config.total_budget)
        return std::nullopt;
    ++d_attempts;
    return delay;
}

} // namespace rwsrt
