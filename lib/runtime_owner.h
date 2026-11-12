/* -*- c++ -*- */
/*
 * Copyright 2026 Robotweax.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INCLUDED_RWSRT_RUNTIME_OWNER_H
#define INCLUDED_RWSRT_RUNTIME_OWNER_H

#include <cstdint>
#include <memory>
#include <string>

namespace rwsrt {

/*!
 * Module-level owner of the process-wide SRT runtime.
 *
 * Every worker holds a lease for as long as it owns SRT sockets or epoll
 * instances. srt_startup() runs when the first lease is taken and
 * srt_cleanup() when the last one is dropped, so one block instance can never
 * clean the runtime up underneath another. A lease must be released only
 * after its holder has closed every SRT handle it created.
 *
 * While any lease is held, SRT library log output is routed into this
 * module (counted, last message kept) instead of the process's stderr. Set
 * the environment variable RWSRT_SRT_LOG=1 to also print it.
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

    struct log_summary {
        std::uint64_t messages = 0; //!< since the first lease of the process
        std::string last;           //!< most recent message, without secrets
    };

    //! Throws std::runtime_error if srt_startup() fails.
    static std::shared_ptr<lease> acquire();

    //! Number of leases currently held by this module (diagnostics).
    static int active_leases();

    static log_summary library_log();
};

} // namespace rwsrt

#endif /* INCLUDED_RWSRT_RUNTIME_OWNER_H */
