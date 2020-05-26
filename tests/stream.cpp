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

#include <iostream>
#include <malloc.h>
#include <stdlib.h>
#include "swarm/api.h"
#include "swarm/algorithm.h"

using swarm::u64it;

// A bunch of stores write to a large array; tests store buffers

volatile uint64_t* array;
uint64_t passes;
uint64_t elems;
uint64_t strands;

#define N (1024ul)
#define STRIDE (8ul)  // one cache line

void task(swarm::Timestamp ts, uint64_t strand, uint64_t pass, uint64_t i) {
    auto sstart = strand * elems / strands;
    auto ssup = (strand + 1) * elems / strands;

    auto start = sstart + i * N * STRIDE;
    auto sup = std::min(ssup, start + N * STRIDE);

    for (auto n = start; n < sup; n += STRIDE) {
        // Should just be a store
        array[n] = n;
    }

    if (sup < ssup) {
        swarm::enqueue(task, ts, NOHINT, strand, pass, i + 1);
    } else if (pass < passes - 1) {
        swarm::enqueue(task, ts + 1, NOHINT, strand, pass + 1, 0);
    }
}

int main(int argc, const char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <memMB> <passes>" << std::endl;
        return -1;
    }

    uint64_t memBytes = atoi(argv[1]) * 1024ul * 1024ul;
    passes = atol(argv[2]);

    array = (volatile uint64_t*)memalign(64, memBytes);
    elems = memBytes / sizeof(array[0]);
    strands = swarm::num_threads() * 8;

    swarm::enqueue_all<NOHINT | MAYSPEC>(u64it(0), u64it(strands),
            [](uint64_t s) {
        swarm::enqueue(task, 0, NOHINT, s, 0, 0);
    }, 0ul);

    swarm::run();
    return 0;
}
