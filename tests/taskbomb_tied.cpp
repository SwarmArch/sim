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
 * Here we let one task enqueue a very large number of children, ideally
 * overflowing the task queue. A 'grandparent' enqueues a 'parent' that enqueues
 * many 'children'. The grandparent is rigged to take a long time to commit so
 * that some/all of the parent's tasks are still tied.
 * As of 2015.04.01 it doesn't pass, revealing nice bugs in rob.h.
 * (Note that a hardware task should have a fixed number of children, so maybe
 * this test is moot but it strains other cases that are not well handled right
 * now).
 */

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <emmintrin.h>
#include "swarm/api.h"
#include "swarm/algorithm.h"
#include "common.h"

// [mcj] Using a global hint seems to be more in the spirit of the original
// implementation
uint64_t hint = 0xf00;

void child(swarm::Timestamp ts) {
    for (uint32_t i = 0; i < 10; i++) _mm_pause();
}

void parent(swarm::Timestamp ts, uint64_t numChildren) {
    swarm::enqueue_all<NOHINT | MAYSPEC>(
            swarm::u32it(0), swarm::u32it(numChildren), [ts] (uint32_t) {
        swarm::enqueue(child, ts, {hint, MAYSPEC});
    }, ts);
}

void grandparent(swarm::Timestamp ts, uint64_t numChildren) {
    swarm::enqueue(parent, ts, SAMEHINT | MAYSPEC, numChildren);
    // Artificially delay commit
    for (uint32_t i = 0; i < 1000; i++) _mm_pause();
}

int main(int argc, const char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <n children>" << std::endl;
        return -1;
    }

    swarm::enqueue(grandparent, 0, {hint, MAYSPEC}, atoi(argv[1]));
    swarm::run();

    tests::assert_true(true);
    return 0;
}
