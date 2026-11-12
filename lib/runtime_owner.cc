/* -*- c++ -*- */
/*
 * Copyright 2026 Robotweax.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "runtime_owner.h"

#include <srt/srt.h>

#include <mutex>
#include <stdexcept>
#include <string>

namespace gr {
namespace robotweax_srt {

namespace {
std::mutex g_mutex;
int g_leases = 0;
} // namespace

std::shared_ptr<runtime_owner::lease> runtime_owner::acquire()
{
    std::lock_guard<std::mutex> guard(g_mutex);
    if (g_leases == 0 && srt_startup() == SRT_ERROR) {
        throw std::runtime_error(std::string("srt_startup failed: ") +
                                 srt_getlasterror_str());
    }
    ++g_leases;
    return std::shared_ptr<lease>(new lease());
}

runtime_owner::lease::~lease()
{
    std::lock_guard<std::mutex> guard(g_mutex);
    if (--g_leases == 0)
        (void)srt_cleanup();
}

int runtime_owner::active_leases()
{
    std::lock_guard<std::mutex> guard(g_mutex);
    return g_leases;
}

} // namespace robotweax_srt
} // namespace gr
