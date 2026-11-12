/* -*- c++ -*- */
/*
 * Copyright 2026 Robotweax.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INCLUDED_RWSRT_STATUS_DOC_H
#define INCLUDED_RWSRT_STATUS_DOC_H

// Ordered key/value tree for status snapshots. One description is rendered
// as JSON (headless runs) and as a PMT dictionary (GNU Radio message port),
// so the two can never drift apart.

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace rwsrt {

class status_doc
{
public:
    using value = std::variant<std::int64_t, std::uint64_t, double, bool, std::string,
                               std::shared_ptr<status_doc>>;

    status_doc& set_int(const std::string& key, std::int64_t v);
    status_doc& set_uint(const std::string& key, std::uint64_t v);
    status_doc& set_num(const std::string& key, double v);
    status_doc& set_bool(const std::string& key, bool v);
    status_doc& set_str(const std::string& key, const std::string& v);
    //! Adds (or returns the existing) nested object.
    status_doc& child(const std::string& key);
    status_doc& set_child(const std::string& key, const status_doc& doc);

    const std::vector<std::pair<std::string, value>>& items() const noexcept { return d_items; }
    std::string to_json() const;

private:
    status_doc& put(const std::string& key, value v);
    std::vector<std::pair<std::string, value>> d_items;
};

} // namespace rwsrt

#endif /* INCLUDED_RWSRT_STATUS_DOC_H */
