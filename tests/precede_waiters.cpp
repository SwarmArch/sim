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
 * This test creates a scenario (for some core/tile counts) where the
 * GVT-holding task is to be aborted, due to an impending parent->child abort
 * message.
 *
 * Each waiter task creates one other waiter task (usually) at a remote tile.
 * There is a bias in the distribution of waiter tasks, where more will be sent
 * to "biasTile", than to other tiles.
 *
 * The 'setup' task creates the string of waiters with TS=1, then spins for a
 * while to let them fill up biasTile's commit queue. The setup task then
 * creates an 'urgent' task with TS=0 at biasTile that should cause a resource
 * abort at that tile. Because each waiter task in a sequence is enqueued to a
 * different tile, then one of the aborted waiters should eventually become the
 * GVT-holding task. That's the goal, but that case is only reproduced for a few
 * core counts.
 */

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>
#include <emmintrin.h>
#include <boost/iterator/counting_iterator.hpp>
#include "swarm/api.h"
#include "swarm/rand.h"
#include "swarm/algorithm.h"
#include "swarm/aligned.h"
#include "common.h"

swarm::aligned<uint64_t> counter;
swarm::Hint biasTile(4ul, NOHASH | MAYSPEC);
uint64_t numTiles;

void waiter(swarm::Timestamp ts, uint64_t remaining) {
    if (remaining == 0) {
        // Base case
        counter++;
    } else if ((remaining % (3 * numTiles / 5)) == 0) {
        // Bias the distribution of waiter tasks: send 67% more tasks to
        // biasTile, relative to all other tiles, to ensure its commit queue is
        // full.
        swarm::enqueue(waiter, ts, biasTile, remaining - 1);
    } else {
        uint64_t rand = biasTile.hint;
        while (rand == biasTile.hint) {
            rand = swarm::rand64() % numTiles;
        }
        swarm::Hint hint(rand, NOHASH);
        swarm::enqueue(waiter, ts, hint, remaining - 1);
    }
}

void urgent(swarm::Timestamp ts) {
    // no-op
    // This has lower timestamp than waiters, and causes a waiter to abort due
    // to commit queue exhaustion.
}

void setup(swarm::Timestamp ts, uint64_t waiters) {
    // Start the string of waiter tasks, that quickly finish but wait in their
    // commit queue slots.
    swarm::enqueue(waiter, ts + 1, NOHINT, waiters);

    // Artificially delay enqueue of the urgent task, to ensure the biasTile
    // tile's commit queue is full.
    for (uint32_t i = 0; i < 500000; i++) _mm_pause();

    // Send the urgent task to the tile whose commit queue is loaded, to cause a
    // resource abort, that then aborts a whole sequence of tasks. Ideally one
    // of these aborted tasks will become the GVT task.
    swarm::enqueue(urgent, ts, biasTile);
}

int main(int argc, const char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <n waiters-per-core>" << std::endl;
        return -1;
    }

    counter = 0ul;
    // FIXME(mcj) the ability of this test to reproduce making the GVT task
    // abort is hackily fragile and dependent on the number of tiles.
    numTiles = std::max(4ul, swarm::num_threads() / 4ul);

    uint64_t waiters = atoi(argv[1]) * swarm::num_threads();
    swarm::enqueue(setup, 0ul, NOHINT, waiters);
    swarm::run();

    tests::assert_eq(1ul, static_cast<uint64_t>(counter));
    return 0;
}
