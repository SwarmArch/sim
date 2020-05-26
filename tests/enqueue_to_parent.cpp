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

#include <iostream>
#include <algorithm>
#include <stdlib.h>
#include <cmath>

#include "common.h"
#include "swarm/api.h"
#include "swarm/algorithm.h"

#define FANOUT      (3)
#define CHAINLENGTH (3)

using namespace std;

uint64_t fanoutCounter = 0;
uint64_t chainCounter = 0;

/*
    + ------------- S0 -- C0a1-C0a2 -- C0b1--C0b2 -- +          Domain depth/level = 0
                  /    \
               /          \
            /                \
         /                      \
      /                            \
    + -- F1a-Ca1-Ca2 -- F1b-Cb1-Cb2 -- +                        Domain depth/level = 1
       /    \          /  \
    /          \      .......
  + - D--F2a--F2b - +   ...                                     Domain depth/level = 2
  ...       ...         ...

  S: Start task in root domain (level = 0)

  Each fanout task (F) increments fanoutCounter and enqueues:
    + One chain task (C) to the same level
    + One chain task (C) to the parent domain
    + One deepen task (D) to a subdomain

  Each chain task (C):
    + increments chainCounter
    + enqueues another chain task (C) while numUpdates-- > 0

  Each deepen task (D) enqueues:
    + FANOUT fanout tasks (F) to its domain

  At level = L, there are FANOUT^L fanout tasks (F) --> so fanoutCounter is incremented FANOUT^L times.
  At level = L, CHAINLENGTH chain tasks (C) are enqueued by each fanout task (F) to same level
                CHAINLENGTH chain tasks (C) are enqueued by each fanout task (F) to parent level (ie. to level L-1)
                --> i.e chainCounter is incrememented 2 * (# times fanoutCounter is incremented at level L)
*/

// Forward declaration
inline void fanoutTask(swarm::Timestamp ts, uint64_t levelsRemaining, uint64_t upTs);

inline void chainTask(swarm::Timestamp ts, uint64_t updatesRemaining) {
    chainCounter++;
    if (updatesRemaining > 0)
        swarm::enqueue(chainTask, ts, NOHINT, updatesRemaining-1);
}

inline void deepenTask(swarm::Timestamp ts, uint64_t levelsRemaining, uint64_t upTs) {
    for (uint32_t i = 0; i < FANOUT; ++i) {
        swarm::enqueue(fanoutTask, i+1/*ts*/, NOHINT, levelsRemaining, upTs);
    }
}

inline void fanoutTask(swarm::Timestamp ts, uint64_t levelsRemaining, uint64_t upTs) {
    fanoutCounter++;
    swarm::enqueue(chainTask, ts, NOHINT, CHAINLENGTH);
    swarm::enqueue(chainTask, upTs, NOHINT | PARENTDOMAIN, CHAINLENGTH);
    if (levelsRemaining > 0) {
        swarm::deepen();
        swarm::enqueue(deepenTask, 0, NOHINT, levelsRemaining-1, ts);
    }

}

inline void startTask(swarm::Timestamp ts, uint64_t levelsRemaining) {
    swarm::deepen();
    for (uint32_t i = 0; i < FANOUT; ++i) {
        swarm::enqueue(fanoutTask, i+1/*ts*/, NOHINT, levelsRemaining, 0);
    }
}

int main(int argc, const char** argv) {
    if (argc != 2) {
        cerr << "Usage: " << argv[0] << " <nlevels>" << endl;
        return -1;
    }

    uint64_t nlevels = atoi(argv[1]);

    swarm::enqueue(startTask, 0, NOHINT, nlevels);
    swarm::run();

    uint64_t expectedFanoutCounter =
        (1.0 * FANOUT * (std::pow(FANOUT, nlevels + 1) - 1)) / (FANOUT - 1);
    uint64_t expectedChainCounter = expectedFanoutCounter * 2 * (CHAINLENGTH + 1);

    tests::assert_eq(expectedFanoutCounter, fanoutCounter);
    tests::assert_eq(expectedChainCounter, chainCounter);

    return 0;
}
