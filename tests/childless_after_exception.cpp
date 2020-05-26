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

/* A task that hits an exception must squash its children before it proceeds to
 * execute a second time.
 *
 * The 'exception' task creates several children 'incrementers' who each
 * increment one counter. Each counter should be incremented once, but if
 * exceptions don't squash children, there will be two increments per counter.
 *
 * The 'delayer' task exists to hold back the GVT so that the exception task
 * does trigger an exception.
 */

#include <algorithm>
#include <cstdlib>
#include <vector>
#include <boost/iterator/counting_iterator.hpp>
#include <emmintrin.h>

#include "swarm/api.h"
#include "common.h"
#include "swarm/aligned.h"
#include "swarm/algorithm.h"

std::vector<swarm::aligned<uint64_t> > counters;

void incrementer(swarm::Timestamp, uint32_t index) {
    counters[index]++;
}

void exception(swarm::Timestamp ts, uint32_t numChildren) {
    // The effects of these incrementers should be undone, so each counter
    // should have value 1.
    swarm::enqueue_all<NOHINT | MAYSPEC>(
            boost::counting_iterator<uint64_t>(0),
            boost::counting_iterator<uint64_t>(numChildren),
            [ts] (uint64_t c) {
        swarm::enqueue(incrementer, ts, NOHINT, c);
    }, ts);

    // Trigger an exception
    swarm::serialize();
}

void delayer(swarm::Timestamp ts) {
    // Artificially delay commit
    for (uint32_t i = 0; i < 10000; i++) _mm_pause();
}

int main(int argc, const char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <n children>" << std::endl;
        return -1;
    }

    uint32_t numChildren = atoi(argv[1]);
    counters.resize(numChildren, 0ul);

    swarm::enqueue(delayer, 0, NOHINT);
    swarm::enqueue(exception, 1, NOHINT, numChildren);

    swarm::run();

    std::vector<swarm::aligned<uint64_t> > expected(numChildren, 1ul);
    tests::assert_eq(expected, counters);
    return 0;
}
