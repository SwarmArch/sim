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

#include "swarm/api.h"
#include "swarm/algorithm.h"
#include "common.h"

uint64_t* arr;
size_t numU64PerCacheLine = SWARM_CACHE_LINE / sizeof(uint64_t);
size_t numIntegers;

static void task1(uint64_t ts, uint64_t idx) {
    arr[idx]++;
}

static void task2(uint64_t ts, uint64_t idx) {
    arr[idx]++;
}

int main(int argc, const char** argv) {
    if (argc != 2) {
        printf("Usage: %s <numIntegers>\n", argv[0]);
    }

    numIntegers = atoi(argv[1]);
    // iterate, at least by cache line, over an array of integers
    // incrementing each array of integers once
    arr = new uint64_t[numIntegers];
    size_t numIters = numIntegers / numU64PerCacheLine;


    // enqueue 
    swarm::enqueue_all<MAYSPEC>(
        swarm::u64it(0), swarm::u64it(numIters / 2),
        [] (uint64_t i) {
            uint64_t idx = i * numU64PerCacheLine;
            if (idx < numIntegers) {
                // give them all the same hint
                swarm::enqueue(task1, 0, swarm::Hint(numIntegers, NONSERIALHINT), idx);
            }
        }, 0);
    
    // task2 should be able to run in the case where all other running tasks are task1
    swarm::enqueue_all<MAYSPEC>(
        swarm::u64it(numIters/2), swarm::u64it(numIters),
        [] (uint64_t i) {
            uint64_t idx = i * numU64PerCacheLine;
            if (idx < numIntegers) {
                // give them all the same hint
                swarm::enqueue(task2, 0, swarm::Hint(numIntegers), idx);
            }
    }, 0);

    swarm::run();

    // verify
    bool success = true;

    for (size_t i = 0; i < numIntegers; i = i + numU64PerCacheLine) {
        if (arr[i] < 1) {
            success = false;
            break;
        }
    }

    tests::assert_true(success);
    return 0;
}
