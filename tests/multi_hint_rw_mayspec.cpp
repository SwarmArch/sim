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
 * This tests the interaction between speculative and non-speculative
 * (specifically MAYSPEC) tasks that read-modify-write the same data.
 *
 * At the time of writing, the simulator could handle running MAYSPEC tasks
 * irrevocably, provided hints with serialization were used, and a
 * non-speculative task's data was single-hint read-write. However, a more
 * general programming model should support both speculative and non-speculative
 * tasks accessing the same data (granted we need to converge on precise
 * semantics).
 *
 * At its inception on the master branch, this test revealed that the simulator
 * permitted some speculative tasks to commit between a non-speculative task's
 * read and write of a counter.
 */

#include <cstdint>
#include <iostream>
#include <emmintrin.h>

#include "swarm/api.h"
#include "swarm/algorithm.h"
#include "common.h"

#define NC (128)
static std::array<uint64_t, NC> counters alignas(SWARM_CACHE_LINE);

// Speculative tasks have a tight increment
void speculative(swarm::Timestamp, uint64_t c) {
    counters[c]++;
}

// Non-speculative tasks have a long delay between the load and store. This
// permits a speculative task to increment the counter *and commit* between the
// load and store (unless the two-phase GVT protocol is working).
void maybespeculative(swarm::Timestamp, uint64_t c) {
    uint64_t read = counters[c];
    for (uint32_t i = 0; i < 500; i++) _mm_pause();
    counters[c] = read + 1;
}

int main(int argc, const char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <nincrements>" << std::endl;
        return -1;
    }
    const uint64_t nincrements = atoi(argv[1]);
    std::fill(counters.begin(), counters.end(), 0ul);

    swarm::enqueue_all<NOHINT | MAYSPEC>(
            swarm::u32it(0), swarm::u32it(nincrements), [] (uint32_t i) {
        uint64_t c = i % NC;
        // 1 in 3 tasks are sent to a random tile and run speculatively
        bool isSpeculative = !(i % 3);
        if (isSpeculative) {
            swarm::enqueue(speculative, 0ul, NOHINT, c);
        } else {
            swarm::enqueue(maybespeculative, 0ul,
                    {swarm::Hint::cacheLine(&counters[c]), MAYSPEC}, c);
        }
    }, 0ul);
    swarm::run();

    uint64_t sum = std::accumulate(counters.begin(), counters.end(), 0ul);
    tests::assert_eq(nincrements, sum);
    return 0;
}
