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

#include <cassert>
#include <stdint.h>
#include <stdio.h>
#include <sstream>
#include <boost/iterator/counting_iterator.hpp>
#include "swarm/api.h"
#include "swarm/algorithm.h"
#include "common.h"

typedef boost::counting_iterator<uint32_t> u32it;

using swarm::Timestamp;

static inline void test(Timestamp ts) {
    char buf[1024];
    snprintf(buf, sizeof(buf)-1, "%s\n", "hello world!");
}

int main(int argc, const char** argv) {
    swarm::enqueue_all<NOHINT | MAYSPEC>(
            u32it(0), u32it(32),
            [](uint32_t i) { swarm::enqueue(test, 0, NOHINT); },
            0ul);

    swarm::run();

    // Print the verification string so that our test scripts don't complain
    tests::assert_true(true);
    return 0;
}

