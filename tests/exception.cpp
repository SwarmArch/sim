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

/* Causes null-pointer dereferences when run speculatively.
 * Used to test the Swarm exception model.
 */

#include <stdlib.h>
#include <vector>
#include <iostream>
#include <algorithm>

#include "common.h"
#include "swarm/api.h"
#include "swarm/algorithm.h"

std::vector<uint64_t*> ptrs;

uint64_t count;

inline void task(swarm::Timestamp ts) {
    *ptrs[ts] += ts;
    ptrs[ts+1] = ptrs[ts];
}

int main(int argc, const char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <vector length>" << std::endl;
        return -1;
    }

    uint64_t size = atoi(argv[1]);

    // one extra elem, written but not read
    ptrs.resize(size + 1);
    count = 0;
    ptrs[0] = &count;
    std::fill(ptrs.begin()+1, ptrs.end(), nullptr);

    swarm::enqueue_all<NOHINT | MAYSPEC>(
            swarm::u64it(0), swarm::u64it(size), [] (uint64_t i) {
        swarm::enqueue(task, i, NOHINT);
    }, 0ul);

    swarm::run();

    tests::assert_eq(size*(size-1)/2, count);
    return 0;
}

