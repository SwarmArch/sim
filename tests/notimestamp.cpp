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
 * Fill one tile with non-timestamped tasks to trigger non-timestamped
 * spillers and requeuers. This probably duplicates aspects of
 * enqueue_all_whints, but oh well.
 */

#include "swarm/api.h"
#include "swarm/algorithm.h"
#include "common.h"

#include <array>
#include <atomic>

constexpr uint64_t NC = 1024;
std::array<std::atomic<uint64_t>, NC> counters;

static void task(swarm::Timestamp, uint64_t c) {
    counters[c]++;
}

int main(int argc, const char** argv) {
    std::fill(counters.begin(), counters.end(), 0ul);

    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <nupdates>" << std::endl;
        return -1;
    }
    uint64_t nupdates = atoi(argv[1]);

    // Flood one tile with NOTIMESTAMP tasks
    swarm::enqueue_all<NOHINT | MAYSPEC>(
            swarm::u64it(0), swarm::u64it(nupdates),
            [] (uint64_t i) {
                swarm::enqueue(task, 0ul, {0ul, NOTIMESTAMP | CANTSPEC}, i % NC);
            },
            1ul);
    swarm::run();

    uint64_t sum = std::accumulate(counters.begin(), counters.end(), 0ul);
    tests::assert_eq(nupdates, sum);
    return 0;
}

