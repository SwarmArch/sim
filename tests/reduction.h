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
 * This test performs an in-place O(lg n) parallel sum reduction on the
 * "counters" vector. Enqueuer tasks first fan out (with equal timestamp,
 * "base"), identifying pairs of elements to sum together.
 * The sum tasks are ordered by the size of the range they represent; a sum that
 * represents a large range is ordered *after* a sum that represents a small
 * range (e.g. two neighboring vector elements should be summed immediately, but
 * the final sum of the left and right halves of the array should be performed
 * at the very end).
 *
 * This task structure is able to bypass any level-based barriers, but is
 * subject to mis-speculations that would be avoided with barriers.
 */

#pragma once

#include <cstdlib>
#include <algorithm>
#include <iostream>
#include <numeric>
#include "swarm/api.h"

extern std::vector<int> counters;

static void sum(swarm::Timestamp, uint32_t left, uint32_t right) {
    counters[left] += counters[right];
}

template<bool usehint>
static void enqueuer(swarm::Timestamp base, uint32_t begin, uint32_t end) {
    if (begin == end) return;

    uint32_t middle = begin/2 + end/2 + 1;

    swarm::enqueue(enqueuer<usehint>, base,
                 NOHINT | SAMETASK,
                 begin, middle - 1);
    swarm::enqueue(enqueuer<usehint>, base,
                 NOHINT | SAMETASK,
                 middle, end);
    // Since the sum task modifies the counter at 'begin',
    // use its cache line as the hint. Unfortunately this doesn't put the task
    // close to both cachelines
    swarm::enqueue(sum, base + (end - begin),
                 (usehint ?
                  swarm::Hint(swarm::Hint::cacheLine(&counters[begin]),
                            MAYSPEC) :
                  swarm::Hint(NOHINT)),
                 begin, middle);
}

static void setup(int argc, const char**argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <vector size>" << std::endl;
        std::abort();
    }

    counters.resize(atoi(argv[1]));
    std::iota(counters.begin(), counters.end(), 0);
}

static void dump() {
    printf("Counters:\n");
    for (uint32_t c = 0; c < counters.size(); c++) {
        printf("%5d: %d\n", c, counters[c]);
    }
}
