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
 * This tests the mutation of the same data on different tiles while irrevocable
 *
 * At its inception, on the master branch, this test passes just fine.
 */
#include <atomic>
#include <cstdint>
#include <iostream>
#include "swarm/api.h"
#include "swarm/algorithm.h"
#include "common.h"

std::atomic<uint64_t> counter;

void increment(swarm::Timestamp) { counter++; }

int main(int argc, const char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <nincrements>" << std::endl;
        return -1;
    }
    const uint64_t nincrements = atoi(argv[1]);
    counter = 0;

    swarm::enqueue_all<NOHINT | MAYSPEC>(
            swarm::u32it(0), swarm::u32it(nincrements), [] (uint32_t i) {
        swarm::enqueue(increment, 0ul, NOHINT | MAYSPEC);
    }, 0ul);
    swarm::run();

    tests::assert_eq(nincrements, counter.load());
    return 0;
}
