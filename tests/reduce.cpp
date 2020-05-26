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

// This tests the swarm::reduce API. It should have better scalability than
// the reduction.cpp test, whose purpose is to create noisy memory traffic.

#include <algorithm>
#include <cstdint>
#include <vector>

#include "swarm/api.h"
#include "swarm/numeric.h"
#include "common.h"

uint64_t sum;
std::vector<uint16_t> values;

int main(int argc, const char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <size of vector>" << std::endl;
        return -1;
    }
    size_t size = atoi(argv[1]);
    assert(size >= 1);

    values.resize(size);
    std::iota(values.begin(), values.end(), 0);

    swarm::reduce(values.begin(), values.end(), 0ul, std::plus<uint64_t>(),
            0ul, [] (swarm::Timestamp, uint64_t r) { sum = r; });
    swarm::run();

    uint64_t blockSize = UINT16_MAX + 1ul;
    uint64_t blocks = size / blockSize;
    uint64_t lastBlock = size - blocks * blockSize;
    uint64_t expected = blocks * (blockSize * (blockSize - 1)) / 2 +
            (lastBlock * (lastBlock - 1) / 2);
    tests::assert_eq(expected, sum);
    return 0;
}
