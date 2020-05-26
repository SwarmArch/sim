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
#include "swarm/api.h"
#include "common.h"
#include <emmintrin.h>

using namespace std;

uint64_t __pad0[32];
uint64_t counter1;
uint64_t __pad1[32];
uint64_t counter2;
uint64_t __pad2[32];


void ping(uint64_t ts, uint64_t upsLeft) {
    if (!upsLeft) return;
    counter2 = counter1 + 1;
    for (uint32_t i = 0; i < 1000; i++) _mm_pause();
    swarm::enqueue(ping, ts+2, EnqFlags::NOHINT, upsLeft-1);
}

void pong(uint64_t ts, uint64_t upsLeft) {
    if (!upsLeft) return;
    counter1 = counter2 + 1;
    swarm::enqueue(pong, ts+2, EnqFlags::NOHINT, upsLeft-1);
}

int main(int argc, const char** argv) {
    if (argc != 2) {
        cerr << "Usage: " << argv[0] << " <nupdates>" << endl;
        return -1;
    }

    uint32_t nupdates = atoi(argv[1]);

    counter1 = counter2 = 0;
    swarm::enqueue(ping, 0, EnqFlags::NOHINT, nupdates - nupdates/2);
    swarm::enqueue(pong, 1, EnqFlags::NOHINT, nupdates/2);

    swarm::run();

    if (nupdates % 2 == 0) tests::assert_eq(counter1, counter2 + 1);
    else tests::assert_eq(counter2, counter1 + 1);
    return 0;
}
