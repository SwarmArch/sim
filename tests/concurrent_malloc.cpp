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
#include "swarm/algorithm.h"
#include "swarm/api.h"
#include "swarm/rand.h"
#include "common.h"

struct Alloc {
    uint64_t* ptr;
    size_t sz;
    uint64_t pad[6];
};

std::vector<Alloc> allocs;

static inline void* allocFn(size_t sz) {
    return  // Pick your poison
        malloc(sz)
        //calloc(1, sz)
        //memalign(sz)
        ;
}

void task(uint64_t ts, size_t i) {
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
        bool largeAlloc = (swarm::rand64() % 128) < 4;
        size_t sz = 64 + swarm::rand64() % ((largeAlloc? 256 : 15) * 1024);
        allocs[i].ptr = (uint64_t*) allocFn(sz);
        allocs[i].sz = sz;
        *allocs[i].ptr = sz;
        *(allocs[i].ptr + sz/sizeof(uint64_t) - 1) = sz;
    }
}

int main(int argc, const char** argv) {
    if (argc != 3) {
        printf("Usage: %s <allocElems> <numTasks>\n", argv[0]);
        return -1;
    }

    size_t allocElems = atoi(argv[1]);
    size_t numTasks = atoi(argv[2]);

    allocs.resize(allocElems);
    for (auto& a : allocs) a.ptr = nullptr;

    swarm::enqueue_all(
        swarm::u64it(0), swarm::u64it(numTasks),
        [=](int a) {
            size_t i = swarm::rand64() % allocs.size();
            swarm::enqueue(task, 0, swarm::Hint(i, /*NOHINT*/ NOFLAGS), i);
        }, 0);

    swarm::run();
    tests::assert_true(true);
    return 0;
}
