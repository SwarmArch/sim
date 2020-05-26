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

// On 1-core systems, this test demonstrates a race between a task enqueue and
// a task dequeue.  This can cause correctness issues on 1-core systems, where
// we must be careful to avoid irrevocably running tasks in the wrong order.

#include <cstdint>
#include <emmintrin.h>

#include "common.h"
#include "swarm/api.h"

bool success = false;

static void pessimist(swarm::Timestamp) {
    success = false;
}

static void optimist(swarm::Timestamp) {
    success = true;
}

static void start(swarm::Timestamp) {
    swarm::enqueue(optimist, 42, EnqFlags::NOHINT);

    // This delay helps to reproduce the desired issue.
    for (uint32_t i = 0; i < 100; i++) _mm_pause();

    // spawn a task that will take at least a cycle to make it out of the TSB.
    swarm::enqueue(pessimist, 0, EnqFlags::NOHINT);

    // Immediately dequeue the next task w/o using a ret instruction.
    // Depending on the core model, this might run while pessimist is still
    // waiting to pass through the TSB!
    __asm__ __volatile__ (" xchg %rdx, %rdx");
}

int main() {
    swarm::enqueue(start, 0, EnqFlags::NOHINT);
    swarm::run();
    tests::assert_eq(success, true);
    return 0;
}
