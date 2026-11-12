/* -*- c++ -*- */
/*
 * Copyright 2026 Robotweax.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INCLUDED_RWSRT_CONTINUITY_H
#define INCLUDED_RWSRT_CONTINUITY_H

// Receiver-side continuity accounting (docs/protocol-v1.md, "Receiver
// behaviour"). Pure logic: it sees decoded headers and connection events.

#include "wire_codec.h"

#include <cstdint>
#include <optional>
#include <string>

namespace rwsrt {

enum class continuity_policy { strict_stop, segmented_live };

//! Causes of a break. Several can apply at one boundary, so they are bits.
enum break_reason : std::uint8_t {
    reason_initial_join = 1u << 0,
    reason_sample_gap = 1u << 1,
    reason_upstream_unknown_gap = 1u << 2,
    reason_reconnect = 1u << 3,
    reason_new_run = 1u << 4,
    reason_receiver_queue_drop = 1u << 5
};

//! Comma-separated reason names, in bit order.
std::string reasons_to_string(std::uint8_t reasons);

struct coordinate {
    run_id_t run{};
    std::uint64_t segment = 0;
    std::uint64_t sample_index = 0;
    bool operator==(const coordinate&) const = default;
};

//! One reported break between what was expected and what arrived.
struct discontinuity {
    std::uint8_t reasons = 0;
    bool has_previous = false; //!< false when nothing was observed before
    coordinate previous;       //!< next sample that had been expected
    coordinate next;           //!< first sample that actually follows
    bool missing_known = true;
    std::uint64_t missing_samples = 0; //!< meaningful when missing_known
    std::uint64_t connection_generation = 0;
    bool operator==(const discontinuity&) const = default;
};

enum class ledger_failure {
    none = 0,
    config_mismatch,     //!< wire format or rate differs from configuration
    protocol_violation,  //!< sender broke an ordering rule
    continuity_violation //!< a break occurred in strict mode
};

struct ledger_result {
    bool accepted = false;
    ledger_failure failure = ledger_failure::none;
    const char* detail = "";              //!< static text for diagnostics
    std::optional<discontinuity> brk;     //!< set for a break, also on failure
    bool is_end = false;
    bool end_complete = false;            //!< receiver-observed completeness
};

struct ledger_counters {
    std::uint64_t records_accepted = 0;
    std::uint64_t samples_accepted = 0;
    std::uint64_t known_missing_samples = 0;
    std::uint64_t unknown_gaps = 0;
    std::uint64_t sequence_gaps = 0; //!< diagnostic only
    std::uint64_t discontinuities = 0;
    std::uint64_t runs = 0;
};

class continuity_ledger
{
public:
    continuity_ledger(continuity_policy policy,
                      sample_format expected_format,
                      std::uint32_t expected_rate_num,
                      std::uint32_t expected_rate_den);

    //! A new transport connection was established.
    void on_connection() noexcept;

    //! Judges the next decoded record. After a failure every later call
    //! fails too: the ledger never resynchronises silently.
    ledger_result on_record(const record_header& h) noexcept;

    bool failed() const noexcept { return d_failure != ledger_failure::none; }
    //! A run has started and its END has not been observed.
    bool run_open() const noexcept { return d_have_run && !d_end_seen; }
    bool end_seen() const noexcept { return d_end_seen; }
    std::uint64_t connection_generation() const noexcept { return d_generation; }
    const ledger_counters& counters() const noexcept { return d_counters; }
    //! Where the next sample of the current run is expected.
    std::optional<coordinate> expectation() const noexcept;

private:
    ledger_result fail(ledger_failure f, const char* detail,
                       std::optional<discontinuity> brk = std::nullopt) noexcept;

    const continuity_policy d_policy;
    const sample_format d_format;
    const std::uint32_t d_rate_num;
    const std::uint32_t d_rate_den;

    ledger_failure d_failure = ledger_failure::none;
    const char* d_failure_detail = "";
    std::uint64_t d_generation = 0;
    bool d_new_connection = false;    //!< no record accepted on it yet
    bool d_pending_reconnect = false; //!< run was open when it reconnected

    bool d_have_run = false;
    run_id_t d_run{};
    std::uint64_t d_segment = 0;
    std::uint64_t d_next_index = 0;
    std::uint64_t d_last_sequence = 0;
    bool d_end_seen = false;
    bool d_observed_start = false;
    bool d_run_unbroken = true;
    ledger_counters d_counters;
};

} // namespace rwsrt

#endif /* INCLUDED_RWSRT_CONTINUITY_H */
