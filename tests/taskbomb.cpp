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
 * This tests what happens when you flood the task queues with tasks. So far I
 * can't find a test that will *fill* task queues, so let's try.
 */
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <algorithm>
#include <boost/iterator/counting_iterator.hpp>
#include "swarm/api.h"
#include "swarm/algorithm.h"
#include "common.h"

using namespace std;

#define NC (128u)
std::array<uint64_t, NC> counters alignas(SWARM_CACHE_LINE);

inline swarm::Hint hint(uint32_t c) {
    return {swarm::Hint::cacheLine(&counters[c]), MAYSPEC};
}

// the cacheline of arg c is used as a spatial hint
void increment(swarm::Timestamp ts, uint32_t c, uint32_t depth) {
    counters[c] += 1 << depth;

    if (pls_unlikely(depth == 0)) return;

    swarm::enqueue(increment, ts+1, SAMETASK | SAMEHINT, c, depth - 1);
    swarm::enqueue(increment, ts+1, SAMETASK | SAMEHINT, c, depth - 1);
}

int main(int argc, const char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0]
                  << " <max tasks per level>" << std::endl;
        return -1;
    }

    // Rather than take the depth as input, to make the amount of work for a
    // user input roughly proportional to the other tests, interpret the input
    // as a proxy to depth:
    const uint32_t maxTasksPerLevel = atoi(argv[1]);
    const uint32_t depth = sizeof(maxTasksPerLevel) * 8
                           - __builtin_clz(maxTasksPerLevel) - 1;
    swarm::info("Depth %d", depth);

    swarm::enqueue_all<NOHINT | MAYSPEC>(swarm::u32it(0), swarm::u32it(NC),
            [depth](uint32_t c) {
        swarm::enqueue(increment, 0, hint(c), c, depth);
    }, 0ul);

    swarm::run();

    swarm::info("Counters:");
    for (uint32_t c = 0; c < NC; c++) {
        swarm::info("%5d: %ld", c, counters[c]);
    }

    // Let D be the depth entered by the user.
    // At depth d <= D, there are 2^(D - d) tasks incrementing a counter
    // by 2^(d).
    // So there are 2^0 * 2^D + 2^1 * 2^(D-1) ... + 2^D * 2^0 = (D+1) * 2^D
    // increments to each counter.
    std::array<uint64_t, NC> verify;
    std::fill(verify.begin(), verify.end(), (depth + 1) * (1 << depth));
    tests::assert_eq(verify, counters);
    return 0;
}
