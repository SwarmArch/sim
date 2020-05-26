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

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <vector>
#include "swarm/algorithm.h"
#include "common.h"
#include "swarm/api.h"

std::vector<uint64_t> counters;

inline swarm::Hint hint(uint64_t c) {
    return {swarm::Hint::cacheLine(&counters[c]), MAYSPEC};
}

void task(swarm::Timestamp, uint64_t c) {
    counters[c] = c;
}

int main(int argc, const char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <ncounters>" << std::endl;
        return -1;
    }

    counters.resize(atoi(argv[1]), 0ul);
    assert(counters.size() > 0);

    // counters is a vector of randomly-shuffled integers from 0 -> ncounters-1.
    std::iota(counters.begin(), counters.end(), 0ul);
    std::random_shuffle(counters.begin(), counters.end());

    swarm::enqueue_all<NOHINT | MAYSPEC>(
            counters.begin(), counters.end(),
            [] (uint64_t c) { swarm::enqueue(task, 1, hint(c), c); },
            0);
    swarm::run();

    std::vector<uint64_t> expected(counters.size(), 0ul);
    std::iota(expected.begin(), expected.end(), 0ul);

    tests::assert_eq(expected, counters);
    return 0;
}
