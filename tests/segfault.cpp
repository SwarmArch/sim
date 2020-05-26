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

#include <stdlib.h>
#include <array>
#include <iostream>
#include <algorithm>
#include <emmintrin.h>

#include "common.h"
#include "swarm/api.h"
#include "swarm/algorithm.h"

/* Causes a null-pointer dereference, both speculatively
 * and non-speculatively. Program should end with a
 * SIGSEGV. Used to test the Swarm exception model.
 */

#define NC (4)

uint64_t count;
bool success = false;

inline void countTask(swarm::Timestamp ts) {
    for (uint32_t i = 0; i < 1000; i++) _mm_pause();
    count++;
}

inline void exceptionTask(swarm::Timestamp ts) {
    //FIXME(victory): Clang is is too smart, it sees this null-pointer dereference,
    //                gives a warning, and, depending on version, gets rid of it.
    if (count < NC) *((volatile uint64_t*)nullptr) = 17;
    else success = true;
}

int main(int argc, const char** argv) {
    count = 0;
    swarm::enqueue_all<NOHINT | MAYSPEC, swarm::max_children-1>(
            swarm::u64it(0), swarm::u64it(NC), [](uint64_t i) {
        swarm::enqueue(countTask, i, NOHINT);
    }, 0ul);
    swarm::enqueue(exceptionTask, NC, NOHINT);

    swarm::run();

    // Should never be reached
    tests::assert_eq(success, true);
    return 0;
}

