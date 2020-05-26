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
#include <boost/iterator/counting_iterator.hpp>
#include "common.h"

#include "swarm/api.h"
#include "swarm/algorithm.h"


using namespace std;

uint64_t v = 0;

inline void task(swarm::Timestamp ts, uint64_t level, uint64_t prefix) {
    uint64_t val = (prefix << 1) + ts;
    if (level == 0) {
        assert(v == val);
        v = val + 1;
    } else {
        swarm::deepen();
        swarm::enqueue(task, 0, EnqFlags::NOHINT, level-1, val);
        swarm::enqueue(task, 1, EnqFlags::NOHINT, level-1, val);
    }
}

int main(int argc, const char** argv) {
    if (argc != 2) {
        cerr << "Usage: " << argv[0] << " <nlevels>" << endl;
        return -1;
    }

    uint64_t nlevels = atoi(argv[1]);

    swarm::enqueue(task, 0, EnqFlags::NOHINT, nlevels, 0);
    swarm::run();

    tests::assert_eq(v, 1ul<<nlevels);
    return 0;
}
