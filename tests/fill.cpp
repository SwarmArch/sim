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
#include <cstdint>
#include <vector>

#define PLS_APP_MAX_ARGS 3  // swarm::fill's internal tasks require 3 args
#include "swarm/api.h"
#include "swarm/algorithm.h"
#include "common.h"

constexpr size_t ZEROS = 5u;
constexpr uint16_t FILLER = 0xf00d;

int main(int argc, const char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <size of vector>" << std::endl;
        return -1;
    }
    size_t size = atoi(argv[1]);
    assert(size >= ZEROS);

    std::vector<uint8_t> actual;
    std::vector<uint8_t> expected;
    actual.resize(size);
    expected.resize(actual.size());

    swarm::fill(actual.begin() + ZEROS, actual.end() - ZEROS, FILLER, 14ul);
    swarm::fill(actual.begin(), actual.end(), 0ul, 13ul);
    swarm::run();

    std::fill(expected.begin(), expected.end(), 0ul);
    std::fill(expected.begin() + ZEROS, expected.end() - ZEROS, FILLER);
    tests::assert_eq(expected, actual);
    return 0;
}
