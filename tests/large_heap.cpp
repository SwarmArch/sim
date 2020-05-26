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
#include <malloc.h>
#include "swarm/api.h"
#include "swarm/rand.h"
#include "common.h"

int main(int argc, const char** argv) {
    // No need to run this inside tasks. This is a functional plsalloc test.
    struct Alloc {
        uint64_t* ptr;
        size_t sz;
    };
    Alloc allocs[256];
    for (auto& a : allocs) a.ptr = nullptr;
    for (uint32_t n = 0; n < 100000; n++) {
        uint32_t i = swarm::rand64() % 256;
        if (allocs[i].ptr) {
            uint64_t* first = allocs[i].ptr;
            uint64_t* last = first + (allocs[i].sz / sizeof(uint64_t)) - 1;
            // Check first and last words haven't been overwritten
            assert(*first == allocs[i].sz);
            assert(*last == allocs[i].sz);
            assert(malloc_usable_size(allocs[i].ptr) >= allocs[i].sz);
            free(allocs[i].ptr);
            allocs[i].ptr = nullptr;
        } else {
            size_t sz = (16 * 1024)  + (swarm::rand64() % (256 * 1024));
            allocs[i] = {(uint64_t*) malloc(sz), sz};
            *allocs[i].ptr = sz;
            *(allocs[i].ptr + sz/sizeof(uint64_t) - 1) = sz;
        }
    }

    swarm::run();
    tests::assert_true(true);
    return 0;
}
