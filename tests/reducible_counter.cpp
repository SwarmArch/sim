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
#include <iostream>
#include <boost/iterator/counting_iterator.hpp>
#include "swarm/api.h"
#include "swarm/algorithm.h"
#include "common.h"
#include "swarm/counter.h"

using namespace std;
typedef boost::counting_iterator<uint32_t> u32it;

uint64_t __pad0[32];
volatile uint64_t scounter;
uint64_t __pad1[32];
swarm::ParallelCounter<uint64_t> pcounter;


void increment(uint64_t ts) {
    uint64_t r = scounter;
    pcounter += 1;
    scounter = r + 1;
}

int main(int argc, const char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <nupdates>" << std::endl;
        return -1;
    }
    uint32_t nupdates = atoi(argv[1]);

    scounter = 0;

    swarm::enqueue_all<NOHINT | MAYSPEC>(u32it(0), u32it(nupdates),
            [](uint32_t i) {
        swarm::enqueue(increment, 0, NOHINT);
    }, 0ul);

    swarm::run();

    printf("Counters: s %ld p %ld\n", scounter, pcounter.reduce());
    tests::assert_true(
            (scounter == nupdates) && (pcounter.reduce() == nupdates));
    return 0;
}
