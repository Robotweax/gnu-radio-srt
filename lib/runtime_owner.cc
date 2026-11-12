/* -*- c++ -*- */
/*
 * Copyright 2026 Robotweax.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "runtime_owner.h"

#include <srt/srt.h>

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <stdexcept>

namespace rwsrt {

namespace {

std::mutex g_mutex;
int g_leases = 0;

std::mutex g_log_mutex;
runtime_owner::log_summary g_log;
bool g_log_passthrough = false;

void on_srt_log(void* /*opaque*/, int /*level*/, const char* /*file*/, int /*line*/,
                const char* area, const char* message)
{
    // Runs on arbitrary SRT threads: keep it short, never call back into SRT.
    std::lock_guard<std::mutex> guard(g_log_mutex);
    ++g_log.messages;
    g_log.last = std::string(area ? area : "") + ": " + (message ? message : "");
    while (!g_log.last.empty() && (g_log.last.back() == '\n' || g_log.last.back() == '\r'))
        g_log.last.pop_back();
    if (g_log_passthrough)
        std::fprintf(stderr, "[srt] %s\n", g_log.last.c_str());
}

} // namespace

std::shared_ptr<runtime_owner::lease> runtime_owner::acquire()
{
    std::lock_guard<std::mutex> guard(g_mutex);
    if (g_leases == 0) {
        if (srt_startup() == SRT_ERROR)
            throw std::runtime_error(std::string("srt_startup failed: ") +
                                     srt_getlasterror_str());
        const char* env = std::getenv("RWSRT_SRT_LOG");
        {
            std::lock_guard<std::mutex> log_guard(g_log_mutex);
            g_log_passthrough = env != nullptr && env[0] == '1';
        }
        srt_setloghandler(nullptr, &on_srt_log);
    }
    ++g_leases;
    return std::shared_ptr<lease>(new lease());
}

runtime_owner::lease::~lease()
{
    std::lock_guard<std::mutex> guard(g_mutex);
    if (--g_leases == 0) {
        srt_setloghandler(nullptr, nullptr); // back to the library default
        (void)srt_cleanup();
    }
}

int runtime_owner::active_leases()
{
    std::lock_guard<std::mutex> guard(g_mutex);
    return g_leases;
}

runtime_owner::log_summary runtime_owner::library_log()
{
    std::lock_guard<std::mutex> guard(g_log_mutex);
    return g_log;
}

} // namespace rwsrt
