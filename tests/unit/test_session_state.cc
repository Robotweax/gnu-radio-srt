// SPDX-License-Identifier: GPL-3.0-or-later
#include "session_state.h"

#include <boost/test/unit_test.hpp>

using namespace rwsrt;
using namespace std::chrono_literals;

namespace {
const auto segmented = continuity_policy::segmented_live;
const auto strict = continuity_policy::strict_stop;
using S = session_state;
using F = session_failure;
} // namespace

BOOST_AUTO_TEST_SUITE(session)

BOOST_AUTO_TEST_CASE(roles_select_the_attempt_state)
{
    session_fsm c(connection_role::caller, strict, false);
    BOOST_TEST((c.state() == S::stopped));
    BOOST_TEST(!c.active());
    BOOST_TEST(c.start());
    BOOST_TEST((c.state() == S::connecting));
    BOOST_TEST(!c.start()); // not startable twice
    session_fsm l(connection_role::listener, strict, false);
    BOOST_TEST(l.start());
    BOOST_TEST((l.state() == S::listening));
    BOOST_TEST(l.active());
}

BOOST_AUTO_TEST_CASE(natural_completion_path)
{
    session_fsm f(connection_role::caller, strict, false);
    f.start();
    BOOST_TEST(f.on_connected());
    BOOST_TEST((f.state() == S::connected));
    BOOST_TEST(f.connection_generation() == 1u);
    BOOST_TEST(!f.on_drained()); // not before END
    BOOST_TEST(f.on_end_of_stream());
    BOOST_TEST((f.state() == S::draining));
    BOOST_TEST(f.on_connection_lost()); // irrelevant once the stream ended
    BOOST_TEST((f.state() == S::draining));
    BOOST_TEST(f.on_drained());
    BOOST_TEST((f.state() == S::completed));
    BOOST_TEST((f.failure() == F::none));
    BOOST_TEST(!f.active());
    BOOST_TEST(!f.on_fatal(F::transport)); // a completed run cannot fail afterwards
    BOOST_TEST(f.request_stop());
    BOOST_TEST(f.on_stopped());
    BOOST_TEST((f.state() == S::stopped));
}

BOOST_AUTO_TEST_CASE(startup_retries_in_both_modes_until_the_deadline)
{
    for (auto policy : { strict, segmented }) {
        session_fsm f(connection_role::caller, policy, policy == segmented);
        f.start();
        BOOST_TEST(f.on_attempt_failed(attempt_failure::transient, F::none));
        BOOST_TEST((f.state() == S::retry_wait));
        BOOST_TEST(f.on_retry_elapsed());
        BOOST_TEST((f.state() == S::connecting));
        BOOST_TEST(!f.on_reconnect_budget_exhausted()); // never connected: wrong deadline
        BOOST_TEST(f.on_attempt_failed(attempt_failure::transient, F::none));
        BOOST_TEST(f.on_startup_deadline());
        BOOST_TEST((f.state() == S::failed));
        BOOST_TEST((f.failure() == F::startup_timeout));
        BOOST_TEST(!f.on_retry_elapsed()); // terminal: no further attempts
    }
}

BOOST_AUTO_TEST_CASE(terminal_attempt_failures_are_not_retried)
{
    session_fsm f(connection_role::caller, segmented, true);
    f.start();
    BOOST_TEST(f.on_attempt_failed(attempt_failure::terminal, F::credentials));
    BOOST_TEST((f.state() == S::failed));
    BOOST_TEST((f.failure() == F::credentials));
    BOOST_TEST(!f.on_connected());
    session_fsm g(connection_role::listener, segmented, true);
    g.start();
    BOOST_TEST(g.on_attempt_failed(attempt_failure::terminal, F::none)); // reason defaults
    BOOST_TEST((g.failure() == F::transport));
}

BOOST_AUTO_TEST_CASE(connection_loss_strict_versus_segmented)
{
    session_fsm s(connection_role::caller, strict, true); // reconnect ignored in strict
    BOOST_TEST(!s.reconnect_allowed());
    s.start();
    s.on_connected();
    BOOST_TEST(s.on_connection_lost());
    BOOST_TEST((s.state() == S::failed));
    BOOST_TEST((s.failure() == F::connection_lost));

    session_fsm d(connection_role::caller, segmented, false); // reconnect disabled
    d.start();
    d.on_connected();
    d.on_connection_lost();
    BOOST_TEST((d.failure() == F::connection_lost));

    session_fsm f(connection_role::listener, segmented, true);
    f.start();
    f.on_connected();
    BOOST_TEST(f.on_connection_lost());
    BOOST_TEST((f.state() == S::retry_wait));
    BOOST_TEST(!f.on_startup_deadline()); // connected before: wrong deadline
    BOOST_TEST(f.on_retry_elapsed());
    BOOST_TEST((f.state() == S::listening));
    BOOST_TEST(f.on_connected());
    BOOST_TEST(f.connection_generation() == 2u); // every connection is a generation
    f.on_connection_lost();
    BOOST_TEST(f.on_reconnect_budget_exhausted());
    BOOST_TEST((f.failure() == F::reconnect_timeout));
}

BOOST_AUTO_TEST_CASE(fatal_and_stop_from_every_active_state)
{
    auto reach = [](S target) {
        session_fsm f(connection_role::caller, segmented, true);
        f.start();
        if (target == S::connecting)
            return f;
        if (target == S::retry_wait) {
            f.on_attempt_failed(attempt_failure::transient, F::none);
            return f;
        }
        f.on_connected();
        if (target == S::draining)
            f.on_end_of_stream();
        return f;
    };
    for (S st : { S::connecting, S::retry_wait, S::connected, S::draining }) {
        session_fsm f = reach(st);
        BOOST_REQUIRE((f.state() == st));
        BOOST_TEST(f.on_fatal(F::protocol));
        BOOST_TEST((f.state() == S::failed));
        BOOST_TEST((f.failure() == F::protocol));
        BOOST_TEST(!f.on_fatal(F::continuity)); // the first failure is retained
        BOOST_TEST((f.failure() == F::protocol));
        BOOST_TEST(f.request_stop()); // a failed session still stops cleanly
        BOOST_TEST((f.state() == S::stopping));
        BOOST_TEST(!f.on_fatal(F::transport)); // errors during teardown do not mask it
        BOOST_TEST(f.on_stopped());
        BOOST_TEST((f.failure() == F::protocol)); // retained for the status report

        session_fsm g = reach(st);
        BOOST_TEST(g.request_stop());
        BOOST_TEST(!g.request_stop());
        BOOST_TEST(!g.on_connected()); // nothing happens while stopping
        BOOST_TEST(!g.on_end_of_stream());
        BOOST_TEST(g.on_stopped());
        BOOST_TEST((g.failure() == F::none)); // user cancellation is not a failure
    }
    session_fsm idle(connection_role::caller, segmented, true);
    BOOST_TEST(!idle.request_stop());
    BOOST_TEST(!idle.on_fatal(F::internal));
    BOOST_TEST(!idle.on_fatal(F::none));
}

BOOST_AUTO_TEST_CASE(restart_clears_the_previous_result)
{
    session_fsm f(connection_role::caller, strict, false);
    f.start();
    f.on_connected();
    f.on_connection_lost();
    f.request_stop();
    f.on_stopped();
    BOOST_TEST((f.failure() == F::connection_lost));
    BOOST_TEST(f.start());
    BOOST_TEST((f.failure() == F::none));
    BOOST_TEST(f.connection_generation() == 0u);
    BOOST_TEST(!f.ever_connected());
}

BOOST_AUTO_TEST_CASE(retry_delays_grow_stay_in_jitter_bounds_and_respect_the_budget)
{
    const auto t0 = retry_schedule::clock::time_point() + 1h;
    retry_config cfg; // 250 ms .. 4000 ms, +-20 %, 60 s
    retry_schedule r(cfg, 42);
    r.begin(t0);
    const double nominal_ms[] = { 250, 500, 1000, 2000, 4000, 4000, 4000 };
    auto now = t0;
    for (double nominal : nominal_ms) {
        const auto d = r.next_delay(now);
        BOOST_REQUIRE(d.has_value());
        const double ms = std::chrono::duration<double, std::milli>(*d).count();
        BOOST_TEST(ms >= nominal * 0.8 - 1e-6);
        BOOST_TEST(ms <= nominal * 1.2 + 1e-6);
        now += *d;
    }
    BOOST_TEST(r.attempts() == 7u);
    // Keep going: the schedule must end inside the 60 s budget.
    int extra = 0;
    while (auto d = r.next_delay(now)) {
        now += *d;
        BOOST_REQUIRE(++extra < 100);
    }
    BOOST_TEST((now - t0 < 60s));
    BOOST_TEST((now - t0 > 60s - 4800ms)); // and not give up early
    BOOST_TEST(!r.budget_exhausted(now));
    BOOST_TEST(r.budget_exhausted(t0 + 60s));

    // Deterministic for a seed, different across seeds, reset by begin().
    retry_schedule a(cfg, 7), b(cfg, 7), c(cfg, 8);
    a.begin(t0);
    b.begin(t0);
    c.begin(t0);
    const auto da = a.next_delay(t0), db = b.next_delay(t0), dc = c.next_delay(t0);
    BOOST_TEST((*da == *db));
    BOOST_TEST((*da != *dc));
    a.begin(t0 + 10s);
    BOOST_TEST(a.attempts() == 0u);
    const double first_again =
        std::chrono::duration<double, std::milli>(*a.next_delay(t0 + 10s)).count();
    BOOST_TEST(first_again <= 300.0);

    // No jitter: exact doubling.
    cfg.jitter = 0.0;
    retry_schedule exact(cfg, 1);
    exact.begin(t0);
    BOOST_TEST((*exact.next_delay(t0) == 250ms));
    BOOST_TEST((*exact.next_delay(t0) == 500ms));
    // A budget shorter than the first delay allows no retry at all.
    cfg.total_budget = 100ms;
    retry_schedule tiny(cfg, 1);
    tiny.begin(t0);
    BOOST_TEST(!tiny.next_delay(t0).has_value());
}

BOOST_AUTO_TEST_SUITE_END()
