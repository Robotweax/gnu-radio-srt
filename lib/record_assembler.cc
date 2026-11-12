/* -*- c++ -*- */
/*
 * Copyright 2026 Robotweax.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "record_assembler.h"

#include <algorithm>
#include <limits>
#include <span>
#include <stdexcept>

namespace rwsrt {

namespace {
constexpr std::uint64_t u64_max = std::numeric_limits<std::uint64_t>::max();
}

record_assembler::record_assembler(const assembler_config& config, const run_id_t& run_id)
    : d_config(config), d_run(run_id)
{
    if (config.format != sample_format::f32 && config.format != sample_format::cf32)
        throw std::invalid_argument("record_assembler: unsupported sample format");
    if (config.rate_num == 0 || config.rate_den == 0)
        throw std::invalid_argument("record_assembler: sample rate must be nonzero");
    normalize_rate(d_config.rate_num, d_config.rate_den);
    const std::size_t bytes = sample_size(config.format);
    if (config.record_limit_bytes < header_size + bytes)
        throw std::invalid_argument("record_assembler: record limit holds no sample");
    d_floats_per_sample = bytes / 4;
    d_max_samples = std::min<std::size_t>((config.record_limit_bytes - header_size) / bytes,
                                          std::numeric_limits<std::uint32_t>::max() / bytes);
    d_partial.reserve(d_max_samples * d_floats_per_sample);
}

queue_entry record_assembler::make_entry(record_kind kind, std::uint64_t first_index,
                                         std::uint32_t count) const
{
    queue_entry e;
    record_header& h = e.header;
    h.kind = kind;
    h.format = d_config.format;
    h.run_id = d_run;
    h.segment_id = d_segment;
    h.first_sample_index = first_index;
    h.sample_count = count;
    h.rate_num = d_config.rate_num;
    h.rate_den = d_config.rate_den;
    h.record_sequence = d_next_sequence;
    if (kind == record_kind::data) {
        capture_time t;
        if (d_anchor.at(first_index, d_config.rate_num, d_config.rate_den, t)) {
            h.capture_time_valid = true;
            h.capture_time_ns = t.time_ns;
            h.capture_uncertainty_ns = t.uncertainty_ns;
        }
    }
    return e;
}

assembler_status record_assembler::seal(const emit_fn& emit)
{
    if (d_partial_samples == 0)
        return assembler_status::ok;
    if (d_next_sequence >= u64_max - 1)
        return assembler_status::exhausted; // keep one number for END
    queue_entry e = make_entry(record_kind::data, d_partial_first,
                               static_cast<std::uint32_t>(d_partial_samples));
    const std::size_t payload = d_partial_samples * sample_size(d_config.format);
    e.message.resize(header_size + payload);
    const wire_error err = encode_header(
        e.header, std::span<std::uint8_t, header_size>(e.message.data(), header_size));
    if (err != wire_error::none)
        return assembler_status::exhausted; // only coordinate rules can fail here
    pack_f32le(std::span<const float>(d_partial.data(), d_partial_samples * d_floats_per_sample),
               std::span<std::uint8_t>(e.message.data() + header_size, payload));
    ++d_next_sequence;
    ++d_records;
    d_partial.clear();
    d_partial_samples = 0;
    emit(std::move(e));
    return assembler_status::ok;
}

assembler_status record_assembler::append(const float* values, std::size_t sample_count,
                                          clock::time_point now, const emit_fn& emit,
                                          std::size_t& consumed)
{
    consumed = 0;
    if (d_finished)
        return assembler_status::finished;
    while (consumed < sample_count) {
        const std::size_t room = d_max_samples - d_partial_samples;
        const std::size_t want = std::min(room, sample_count - consumed);
        // Never accept a sample whose coordinate could not be encoded.
        const std::uint64_t headroom = u64_max - d_next_index;
        const std::size_t take =
            static_cast<std::size_t>(std::min<std::uint64_t>(want, headroom));
        if (take == 0)
            return assembler_status::exhausted;
        if (d_partial_samples == 0) {
            d_partial_first = d_next_index;
            d_partial_started = now;
        }
        const float* first = values + consumed * d_floats_per_sample;
        d_partial.insert(d_partial.end(), first, first + take * d_floats_per_sample);
        d_partial_samples += take;
        d_next_index += take;
        d_samples_consumed += take;
        consumed += take;
        if (d_partial_samples == d_max_samples) {
            if (const assembler_status s = seal(emit); s != assembler_status::ok)
                return s;
        }
    }
    return assembler_status::ok;
}

assembler_status record_assembler::set_capture_time(capture_time t, const emit_fn& emit)
{
    if (d_finished)
        return assembler_status::finished;
    if (const assembler_status s = seal(emit); s != assembler_status::ok)
        return s;
    return d_anchor.set(d_next_index, t) ? assembler_status::ok : assembler_status::bad_time;
}

assembler_status record_assembler::known_gap(std::uint64_t missing, const emit_fn& emit)
{
    if (d_finished)
        return assembler_status::finished;
    if (const assembler_status s = seal(emit); s != assembler_status::ok)
        return s;
    if (missing > u64_max - d_next_index)
        return assembler_status::exhausted;
    d_next_index += missing; // the sample clock kept running: the anchor holds
    return assembler_status::ok;
}

assembler_status record_assembler::unknown_gap(const emit_fn& emit)
{
    if (d_finished)
        return assembler_status::finished;
    if (const assembler_status s = seal(emit); s != assembler_status::ok)
        return s;
    if (d_segment == u64_max)
        return assembler_status::exhausted;
    ++d_segment;
    d_next_index = 0;
    d_anchor.clear(); // elapsed time across the gap is unknown
    return assembler_status::ok;
}

bool record_assembler::flush_if_due(clock::time_point now, const emit_fn& emit)
{
    if (d_partial_samples == 0 || now - d_partial_started < d_config.flush_interval)
        return false;
    return seal(emit) == assembler_status::ok;
}

assembler_status record_assembler::flush(const emit_fn& emit)
{
    return d_finished ? assembler_status::finished : seal(emit);
}

assembler_status record_assembler::finish(const emit_fn& emit)
{
    if (d_finished)
        return assembler_status::finished;
    if (const assembler_status s = seal(emit); s != assembler_status::ok)
        return s;
    queue_entry e = make_entry(record_kind::end, d_next_index, 0);
    e.message.resize(header_size);
    if (encode_header(e.header, std::span<std::uint8_t, header_size>(
                                    e.message.data(), header_size)) != wire_error::none)
        return assembler_status::exhausted;
    ++d_next_sequence;
    ++d_records;
    d_finished = true;
    emit(std::move(e));
    return assembler_status::ok;
}

} // namespace rwsrt
