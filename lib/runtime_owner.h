/* -*- c++ -*- */
/*
 * Copyright 2026 Robotweax.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INCLUDED_ROBOTWEAX_SRT_RUNTIME_OWNER_H
#define INCLUDED_ROBOTWEAX_SRT_RUNTIME_OWNER_H

#include <memory>

namespace gr {
namespace robotweax_srt {

/*!
 * Module-level owner of the process-wide SRT runtime.
 *
 * Every worker holds a lease for as long as it owns SRT sockets or epoll
 * instances. srt_startup() runs when the first lease is taken and
 * srt_cleanup() when the last one is dropped, so one block instance can never
 * clean the runtime up underneath another. A lease must be released only
 * after its holder has closed every SRT handle it created.
 */
class runtime_owner
{
public:
    class lease
    {
    public:
        ~lease();
        lease(const lease&) = delete;
        lease& operator=(const lease&) = delete;

    private:
        friend class runtime_owner;
        lease() = default;
    };

    //! Throws std::runtime_error if srt_startup() fails.
    static std::shared_ptr<lease> acquire();

    //! Number of leases currently held by this module (diagnostics).
    static int active_leases();
};

} // namespace robotweax_srt
} // namespace gr

#endif /* INCLUDED_ROBOTWEAX_SRT_RUNTIME_OWNER_H */
