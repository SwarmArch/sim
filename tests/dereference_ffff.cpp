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

/*
 * Speculatively dereference address 0xffffffffffffffff.
 * This tests for sim bugs involving wraparound/overflow in address-related
 * computations.
 */

#include <cstdint>
#include <emmintrin.h>

#include "common.h"
#include "swarm/api.h"

bool success = false;
bool* success_ptr = (bool*)0xffffffffffffffff;

uint32_t answer32 = 0;
uint32_t* answer32_ptr  = (uint32_t*)0xfffffffffffffffc;

uint64_t answer64 = 0;
uint64_t* answer64_ptr  = (uint64_t*)0xffffffffffffffff;

static inline void slowlySetPointers(swarm::Timestamp ts) {
    for (uint32_t i = 0; i < 5000; i++) _mm_pause();

    success_ptr = &success;
    answer32_ptr = &answer32;
    answer64_ptr = &answer64;
}

static inline void dereferenceSuccess(swarm::Timestamp) {
    *success_ptr = true;
}

static inline void dereferenceAnswer32(swarm::Timestamp) {
    *answer32_ptr = 7;
}

static inline void dereferenceAnswer64(swarm::Timestamp) {
    *answer64_ptr = 42;
}

int main() {
    swarm::enqueue(slowlySetPointers, 0, EnqFlags::NOHINT);
    swarm::enqueue(dereferenceSuccess, 1, EnqFlags::NOHINT);
    swarm::enqueue(dereferenceAnswer32, 1, EnqFlags::NOHINT);
    swarm::enqueue(dereferenceAnswer64, 1, EnqFlags::NOHINT);
    swarm::run();
    tests::assert_eq(success, true);
    tests::assert_eq(answer32, 7U);
    tests::assert_eq(answer64, 42UL);
    return 0;
}
