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

/**
 * This test is mostly a copy of the counter test, but uses memalign to allocate
 * memory. The simulator's Pin alloc replacement methods aren't playing well
 * with memalign and other functions.
 */
#include <stdlib.h>
#include <array>
#include <iostream>
#include <algorithm>
#include <malloc.h>
#include <boost/iterator/counting_iterator.hpp>
#include "swarm/api.h"
#include "swarm/algorithm.h"
#include "common.h"

typedef boost::counting_iterator<uint64_t> u64it;

#define NC (128ul)
std::array<uint64_t, NC> counters;

inline void task(swarm::Timestamp ts, uint64_t c, uint64_t upsLeft) {
    if (!upsLeft) return;
    int* i = (int*) memalign(64, sizeof(int));
    *i = 1;
    counters[c] += *i;
    swarm::enqueue(task, ts+c+1, NOHINT | SAMETASK, c, --upsLeft);
    free(i);
}

int main(int argc, const char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <nupdates>" << std::endl;
        return -1;
    }

    uint64_t nupdates = atoi(argv[1]);

    std::fill(counters.begin(), counters.end(), 0ul);

    swarm::enqueue_all<NOHINT | MAYSPEC>( u64it(0), u64it(NC),
            [nupdates](uint64_t c) {
        uint64_t cUpdates = nupdates / NC + (c < (nupdates % NC));
        if (cUpdates) swarm::enqueue(task, 0, NOHINT, c, cUpdates);
    }, 0ul);

    swarm::run();

    printf("Counters:\n");
    uint64_t last = counters[0];
    uint64_t lastStart = 0;
    for (uint64_t c = 0; c < NC; c++) {
        uint64_t v = counters[c];
        if (v != last) {
            printf(" %3ld - %3ld:  %ld\n", lastStart, c-1, last);
            last = v;
            lastStart = c;
        }
    }
    if (lastStart < NC-1) printf(" %3ld - %3ld:  %ld\n", lastStart, NC-1, last);

    tests::assert_eq(nupdates,
                     std::accumulate(counters.begin(), counters.end(), 0ul));
    return 0;
}
