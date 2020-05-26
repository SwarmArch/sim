/** $lic$
 * Copyright (C) 2012-2021 by Massachusetts Institute of Technology
 * Copyright (C) 2010-2012 by The Board of Trustees of Stanford University
 *
 * This file is part of the Swarm simulator.
 *
 * This simulator is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, version 2.
 *
 * This simulator was developed as part of the Swarm architecture project. If
 * you use this software in your research, we request that you reference the
 * Swarm MICRO 2018 paper ("Harmonizing Speculative and Non-Speculative
 * Execution in Architectures for Ordered Parallelism", Jeffrey et al.,
 * MICRO-51, 2018) as the source of this simulator in any publications that use
 * this software, and that you send us a citation of your work.
 *
 * This simulator is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

/* This file was adapted from zsim. */

#ifndef __SIM_ASSERT_H__
#define __SIM_ASSERT_H__

// assertions are often frequently executed but never inlined. Might as well tell the compiler about it
#define likely(x)       __builtin_expect((x), 1)
#define unlikely(x)     __builtin_expect((x), 0)

#ifdef SWARM_SIM_NO_LOG
// victory: Minimize dependencies of unit tests, just use glibc.

#include <cassert>
#include <cstdlib>

#define panic(args...) \
{ \
    fprintf(stderr, "\nPanic on %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, args); \
    fprintf(stderr, "\n"); \
    fflush(stderr); \
    std::exit(EXIT_FAILURE); \
}

#define assert_msg(cond, args...) \
if (unlikely(!(cond))) { \
    fprintf(stderr, "\nFailed assertion on %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, args); \
    fprintf(stderr, "\n"); \
    fflush(stderr); \
    std::exit(EXIT_FAILURE); \
};

#endif //SWARM_SIM_NO_LOG

#endif //__SIM_ASSERT_H__

// victory: Bring sim/log.{h,cpp} and its dependencies only in the simulator proper.
#ifndef SWARM_SIM_NO_LOG

// Be as insidious as <assert.h>
#ifdef assert
#undef assert
#endif

#ifdef assert_msg
#undef assert_msg
#endif

// dsm: NO IFDEF GUARD. Always redefine assert

#include "sim/log.h"

#ifndef NASSERT

#define assert(expr) \
if (unlikely(!(expr))) { \
    fprintf(logFdErr, "%sFailed assertion on %s:%d '%s' ", \
            logHeader, __FILE__, __LINE__, #expr); \
    /*In case PrintExpr crashes, ensure we printed the assertion info above*/ \
    fflush(logFdErr); \
    std::stringstream __assert_ss__LINE__; \
    PrintExpr __printExpr__LINE__(__assert_ss__LINE__); \
    (void) (__printExpr__LINE__->*expr); \
    fprintf(logFdErr, "(with '%s')\n", __assert_ss__LINE__.str().c_str()); \
    fflush(logFdErr); \
    __log_fail(); \
};

#define assert_msg(cond, args...) \
if (unlikely(!(cond))) { \
    fprintf(logFdErr, "%sFailed assertion on %s:%d: ", logHeader, __FILE__, __LINE__); \
    fprintf(logFdErr, args); \
    fprintf(logFdErr, "\n"); \
    fflush(logFdErr); \
    __log_fail(); \
};
#else
// Avoid unused warnings, never emit any code
// see http://cnicholson.net/2009/02/stupid-c-tricks-adventures-in-assert/
#define assert(cond) do { (void)sizeof(cond); } while (0);
#define assert_msg(cond, args...) do { (void)sizeof(cond); } while (0);
#endif

#endif //SWARM_SIM_NO_LOG
