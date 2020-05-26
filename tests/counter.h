/** $lic$
 * Copyright (C) 2014-2021 by Massachusetts Institute of Technology
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

#pragma once

#include <stdlib.h>
#include <array>
#include <iostream>
#include <algorithm>

static void task(uint64_t, uint64_t, uint64_t);

//#define PLS_SINGLE_TASKFUNC task
//#define PLS_SINGLE_TASKFUNC_ARGS uint64_t, uint64_t
//#define PLS_ORDERED_ENQUEUES  // result is valid but behavior is incorrect, enqueues don't happen in order

#include "swarm/api.h"
#include "swarm/algorithm.h"
#include "common.h"

#define NC (32768ul)
static std::array<uint64_t, NC> counters alignas(SWARM_CACHE_LINE);

#if defined(PLS_SPATIAL)
#define HINTFLAG SAMEHINT | MAYSPEC
inline swarm::Hint hint(uint64_t c) {
    return {swarm::Hint::cacheLine(&counters[c]), MAYSPEC};
}
#elif defined(PLS_SPATIAL_COLLIDE)
#define HINTFLAG SAMEHINT
inline uint64_t hint(uint64_t c) { return c; }
#else
#define HINTFLAG NOHINT
inline EnqFlags hint(uint64_t) { return NOHINT; }
#endif


static void task(uint64_t ts, uint64_t c, uint64_t upsLeft) {
    counters[c]++;
    if (pls_likely(--upsLeft)) {
        swarm::enqueue(task, ts+c+1, HINTFLAG | SAMETASK, c, upsLeft);
    }
}


static int counter(int argc, const char** argv) {
    if (argc < 2 || argc > 3) {
        std::cerr << "Usage: " << argv[0]
                  << " <nupdates> [<ncounters>]" << std::endl;
        std::abort();
    }

    uint64_t nupdates = atoi(argv[1]);
    uint64_t ncounters = (argc == 3)? atoi(argv[2]) : 128;

    assert(ncounters > 0);
    assert(ncounters <= NC);

    std::fill(counters.begin(), counters.end(), 0ul);

    uint64_t upc = nupdates / ncounters;
    uint64_t rem = nupdates % ncounters;
    swarm::enqueue_all<NOHINT | MAYSPEC>(
            swarm::u64it(0), swarm::u64it(ncounters),
            [upc, rem] (uint64_t c) {
        uint64_t updates = upc + (c < rem);
        if (updates) swarm::enqueue(task, 0, hint(c), c, updates);
    }, 0ul);

    swarm::run();

    std::array<uint64_t, NC> verify;
    std::fill(verify.begin(), verify.begin() + rem, upc + 1);
    std::fill(verify.begin() + rem, verify.begin() + ncounters, upc);
    std::fill(verify.begin() + ncounters, verify.end(), 0ul);
    tests::assert_eq(verify, counters);
    return 0;
}
