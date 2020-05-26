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
#include "swarm/algorithm.h"
#include "swarm/api.h"
#include "common.h"


inline void allocTask(swarm::Timestamp ts, uint64_t alloc_size) {
    long* tm = (long*) malloc(alloc_size*sizeof(long));
    const int WRITES = 100000; /*some large number sufficient to fill up bloom filters */
    for (int i=0;i<WRITES;i++){
        // write somthing so that OS actually assigns the memory
        tm[i%alloc_size] = i;
    }
    swarm::info("%ld %d",ts, tm[1] ); // make sure tm isn't optimized away
    free(tm);
}

int main(int argc, const char** argv) {
    if (argc < 2 || argc > 3) {
        std::cerr << "Usage: " << argv[0]
                  << " <allocsize> [<ntasks>]" << std::endl;
        std::abort();
    }
    uint64_t alloc_size = atoi(argv[1]);
    uint64_t ntasks = (argc == 3)? atoi(argv[2]) : 128;


    swarm::enqueue_all<NOHINT | MAYSPEC, swarm::max_children-1>(
            swarm::u32it(0),
            swarm::u32it(ntasks),
            [=] (int a) {
        swarm::enqueue(allocTask, 0, NOHINT, alloc_size);
    }, 0);

    swarm::run();
    tests::assert_true(true);
    return 0;
}

