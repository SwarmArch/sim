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

// This tests the swarm::transform API task
// It should suffer zero aborts, but it took careful changes to cps.h to make
// that happen.

#include <algorithm>
#include <cstdint>
#include <vector>

#include "swarm/api.h"
// TODO(mcj) move swarm::transform to algorithm.h to mimic <algorithm>
// That isn't possible right now because
//  transform.h #includes cps.h which #includes algorithm.h
// We should move enqueue_all out of algorithm.h
#include "swarm/impl/transform.h"
#include "common.h"

int main(int argc, const char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <size of vector>" << std::endl;
        return -1;
    }
    size_t size = atoi(argv[1]);
    assert(size >= 1);

    std::vector<uint16_t> input, output, expected;
    input.resize(size);
    output.resize(size);
    expected.resize(size);

    std::iota(input.begin(), input.end(), 0);
    std::iota(expected.begin(), expected.end(), 1);

    swarm::transform(input.cbegin(), input.cend(), output.begin(),
            [] (uint16_t i) { return i + 1; },
            0ul);
    swarm::run();

    tests::assert_eq(expected, output);
    return 0;
}
