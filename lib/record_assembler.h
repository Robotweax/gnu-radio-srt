/* -*- c++ -*- */
/*
 * Copyright 2026 Robotweax.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INCLUDED_RWSRT_RECORD_ASSEMBLER_H
#define INCLUDED_RWSRT_RECORD_ASSEMBLER_H

// Sender-side framing: turns a sample stream plus boundary events into
// sealed RWSR records with run, segment, sample and sequence coordinates.
// Not thread-safe; its owner serialises access (plan section 9).

#include "bounded_record_queue.h"
#include "sample_time.h"
#include "wire_codec.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace rwsrt {

struct assembler_config {
    sample_format format = sample_format::cf32;
    std::uint32_t rate_num = 100000; //!< normalized by the constructor
    std::uint32_t rate_den = 1;
    std::size_t record_limit_bytes = 1200; //!< header included
    std::chrono::nanoseconds flush_interval = std::chrono::milliseconds(2);
};

enum class assembler_status {
    ok,
    exhausted, //!< a coordinate or the sequence would wrap: start a new run
    finished,  //!< END was already produced
    bad_time   //!< impossible capture time; the anchor was not set
};

class record_assembler
{
public:
    using clock = std::chrono::steady_clock;
    using emit_fn = std::function<void(queue_entry&&)>;

    //! Throws std::invalid_argument for a configuration that cannot hold
    //! one sample per record or has an invalid rate.
    record_assembler(const assembler_config& config, const run_id_t& run_id);

    std::size_t max_samples_per_record() const noexcept { return d_max_samples; }

    /*!
     * Appends \p sample_count samples. \p values holds one float per f32
     * sample or I,Q pairs per cf32 sample. Every sealed record is passed to
     * \p emit before this returns. On exhausted nothing more is consumed;
     * \p consumed reports how many samples were taken.
     */
    assembler_status append(const float* values, std::size_t sample_count,
                            clock::time_point now, const emit_fn& emit,
                            std::size_t& consumed);

    //! The next appended sample was captured at \p t. Splits the record.
    assembler_status set_capture_time(capture_time t, const emit_fn& emit);
    //! Exactly \p missing samples were lost upstream before the next sample.
    assembler_status known_gap(std::uint64_t missing, const emit_fn& emit);
    //! An unknown number of samples was lost: a new segment starts.
    assembler_status unknown_gap(const emit_fn& emit);

    //! Seals the partial record if its first sample has waited at least the
    //! flush interval. Returns true when a record was emitted.
    bool flush_if_due(clock::time_point now, const emit_fn& emit);
    //! Seals the partial record, if any.
    assembler_status flush(const emit_fn& emit);
    //! Natural end of stream: flush, then END. Later calls report finished.
    assembler_status finish(const emit_fn& emit);

    bool has_partial() const noexcept { return d_partial_samples != 0; }
    bool finished() const noexcept { return d_finished; }
    const run_id_t& run_id() const noexcept { return d_run; }
    std::uint64_t segment() const noexcept { return d_segment; }
    std::uint64_t next_sample_index() const noexcept { return d_next_index; }
    std::uint64_t next_sequence() const noexcept { return d_next_sequence; }
    std::uint64_t samples_consumed() const noexcept { return d_samples_consumed; }
    std::uint64_t records_assembled() const noexcept { return d_records; }

private:
    assembler_status seal(const emit_fn& emit);
    queue_entry make_entry(record_kind kind, std::uint64_t first_index,
                           std::uint32_t count) const;

    assembler_config d_config;
    const run_id_t d_run;
    std::size_t d_floats_per_sample;
    std::size_t d_max_samples;

    std::uint64_t d_segment = 0;
    std::uint64_t d_next_index = 0;    //!< coordinate of the next appended sample
    std::uint64_t d_next_sequence = 0;
    std::uint64_t d_samples_consumed = 0;
    std::uint64_t d_records = 0;
    bool d_finished = false;
    time_anchor d_anchor;

    std::vector<float> d_partial;       //!< interleaved values of the open record
    std::size_t d_partial_samples = 0;
    std::uint64_t d_partial_first = 0;
    clock::time_point d_partial_started{};
};

} // namespace rwsrt

#endif /* INCLUDED_RWSRT_RECORD_ASSEMBLER_H */
