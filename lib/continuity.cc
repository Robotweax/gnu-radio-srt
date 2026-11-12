/* -*- c++ -*- */
/*
 * Copyright 2026 Robotweax.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "continuity.h"

namespace rwsrt {

std::string reasons_to_string(std::uint8_t reasons)
{
    static constexpr struct {
        std::uint8_t bit;
        const char* name;
    } names[] = {
        { reason_initial_join, "initial_join" },
        { reason_sample_gap, "sample_gap" },
        { reason_upstream_unknown_gap, "upstream_unknown_gap" },
        { reason_reconnect, "reconnect" },
        { reason_new_run, "new_run" },
        { reason_receiver_queue_drop, "receiver_queue_drop" },
    };
    std::string out;
    for (const auto& n : names) {
        if (reasons & n.bit) {
            if (!out.empty())
                out += ',';
            out += n.name;
        }
    }
    return out;
}

continuity_ledger::continuity_ledger(continuity_policy policy,
                                     sample_format expected_format,
                                     std::uint32_t expected_rate_num,
                                     std::uint32_t expected_rate_den)
    : d_policy(policy),
      d_format(expected_format),
      d_rate_num(expected_rate_num),
      d_rate_den(expected_rate_den)
{
}

void continuity_ledger::on_connection() noexcept
{
    ++d_generation;
    d_new_connection = true;
    if (d_have_run && !d_end_seen)
        d_pending_reconnect = true;
}

std::optional<coordinate> continuity_ledger::expectation() const noexcept
{
    if (!d_have_run)
        return std::nullopt;
    return coordinate{ d_run, d_segment, d_next_index };
}

ledger_result continuity_ledger::fail(ledger_failure f, const char* detail,
                                      std::optional<discontinuity> brk) noexcept
{
    d_failure = f;
    d_failure_detail = detail;
    ledger_result r;
    r.failure = f;
    r.detail = detail;
    r.brk = brk;
    return r;
}

ledger_result continuity_ledger::on_record(const record_header& h) noexcept
{
    if (failed()) {
        ledger_result r;
        r.failure = d_failure;
        r.detail = d_failure_detail;
        return r;
    }
    if (h.format != d_format)
        return fail(ledger_failure::config_mismatch, "sample format differs from configuration");
    if (h.rate_num != d_rate_num || h.rate_den != d_rate_den)
        return fail(ledger_failure::config_mismatch, "sample rate differs from configuration");

    const bool is_end = h.kind == record_kind::end;
    const bool clean_start =
        h.segment_id == 0 && h.first_sample_index == 0 && h.record_sequence == 0;
    const bool same_run = d_have_run && h.run_id == d_run;

    discontinuity brk;
    brk.connection_generation = d_generation;
    brk.next = coordinate{ h.run_id, h.segment_id, h.first_sample_index };
    if (d_have_run) {
        brk.has_previous = true;
        brk.previous = coordinate{ d_run, d_segment, d_next_index };
    }

    if (!d_have_run) {
        if (!clean_start) {
            brk.reasons |= reason_initial_join;
            brk.missing_known = false;
        }
    } else if (!same_run) {
        if (!d_new_connection)
            return fail(ledger_failure::protocol_violation,
                        "run changed without a new connection");
        brk.reasons |= reason_new_run;
        brk.missing_known = false;
        if (d_pending_reconnect)
            brk.reasons |= reason_reconnect;
        if (!clean_start)
            brk.reasons |= reason_initial_join;
    } else {
        if (d_end_seen)
            return fail(ledger_failure::protocol_violation, "record after END of its run");
        if (h.record_sequence <= d_last_sequence)
            return fail(ledger_failure::protocol_violation,
                        "record sequence repeated or regressed");
        if (h.segment_id < d_segment)
            return fail(ledger_failure::protocol_violation, "segment regressed");
        if (h.segment_id == d_segment) {
            if (h.first_sample_index < d_next_index)
                return fail(ledger_failure::protocol_violation,
                            is_end ? "END before the expected coordinate"
                                   : "sample interval overlaps accepted samples");
            if (h.first_sample_index > d_next_index) {
                brk.reasons |= reason_sample_gap;
                brk.missing_samples = h.first_sample_index - d_next_index;
            }
        } else {
            brk.reasons |= reason_upstream_unknown_gap;
            brk.missing_known = false;
        }
        if (d_pending_reconnect)
            brk.reasons |= reason_reconnect;
    }
    if (!brk.missing_known)
        brk.missing_samples = 0;

    if (brk.reasons != 0 && d_policy == continuity_policy::strict_stop)
        return fail(ledger_failure::continuity_violation,
                    "continuity break in strict mode", brk);

    // Accepted: commit.
    if (same_run && h.record_sequence != d_last_sequence + 1)
        ++d_counters.sequence_gaps;
    if (!same_run) {
        ++d_counters.runs;
        d_observed_start = clean_start;
        d_run_unbroken = true;
        d_end_seen = false;
    }
    if (brk.reasons != 0) {
        ++d_counters.discontinuities;
        // A new run is a break in the output stream, but the new run itself
        // can still be observed completely from its clean start.
        if (same_run || (brk.reasons & reason_initial_join))
            d_run_unbroken = false;
        if (brk.missing_known)
            d_counters.known_missing_samples += brk.missing_samples;
        else
            ++d_counters.unknown_gaps;
    }
    d_have_run = true;
    d_run = h.run_id;
    d_segment = h.segment_id;
    d_next_index = h.first_sample_index + h.sample_count; // no wrap: validated
    d_last_sequence = h.record_sequence;
    d_pending_reconnect = false;
    d_new_connection = false;
    ++d_counters.records_accepted;
    d_counters.samples_accepted += h.sample_count;

    ledger_result r;
    r.accepted = true;
    if (brk.reasons != 0)
        r.brk = brk;
    if (is_end) {
        d_end_seen = true;
        r.is_end = true;
        r.end_complete = d_observed_start && d_run_unbroken;
    }
    return r;
}

} // namespace rwsrt
