/* -*- c++ -*- */
/*
 * Copyright 2026 Robotweax.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "status_doc.h"

#include <cmath>
#include <cstdio>

namespace rwsrt {

namespace {
void escape(const std::string& in, std::string& out)
{
    for (const char c : in) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
            out += buf;
        } else {
            out += c;
        }
    }
}
} // namespace

status_doc& status_doc::put(const std::string& key, value v)
{
    for (auto& item : d_items) {
        if (item.first == key) {
            item.second = std::move(v);
            return *this;
        }
    }
    d_items.emplace_back(key, std::move(v));
    return *this;
}

status_doc& status_doc::set_int(const std::string& key, std::int64_t v) { return put(key, v); }
status_doc& status_doc::set_uint(const std::string& key, std::uint64_t v) { return put(key, v); }
status_doc& status_doc::set_num(const std::string& key, double v) { return put(key, v); }
status_doc& status_doc::set_bool(const std::string& key, bool v) { return put(key, v); }
status_doc& status_doc::set_str(const std::string& key, const std::string& v)
{
    return put(key, v);
}

status_doc& status_doc::child(const std::string& key)
{
    for (auto& item : d_items) {
        if (item.first == key) {
            if (auto* p = std::get_if<std::shared_ptr<status_doc>>(&item.second))
                return **p;
        }
    }
    auto doc = std::make_shared<status_doc>();
    put(key, doc);
    return *doc;
}

status_doc& status_doc::set_child(const std::string& key, const status_doc& doc)
{
    return put(key, std::make_shared<status_doc>(doc));
}

std::string status_doc::to_json() const
{
    std::string out = "{";
    bool first = true;
    for (const auto& item : d_items) {
        if (!first)
            out += ',';
        first = false;
        out += '"';
        escape(item.first, out);
        out += "\":";
        if (const auto* i = std::get_if<std::int64_t>(&item.second)) {
            out += std::to_string(*i);
        } else if (const auto* u = std::get_if<std::uint64_t>(&item.second)) {
            out += std::to_string(*u);
        } else if (const auto* d = std::get_if<double>(&item.second)) {
            if (std::isfinite(*d)) {
                char buf[40];
                std::snprintf(buf, sizeof(buf), "%.6g", *d);
                out += buf;
            } else {
                out += "null"; // JSON has no NaN or infinity
            }
        } else if (const auto* b = std::get_if<bool>(&item.second)) {
            out += *b ? "true" : "false";
        } else if (const auto* s = std::get_if<std::string>(&item.second)) {
            out += '"';
            escape(*s, out);
            out += '"';
        } else {
            out += std::get<std::shared_ptr<status_doc>>(item.second)->to_json();
        }
    }
    out += '}';
    return out;
}

} // namespace rwsrt
