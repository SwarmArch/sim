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

/* Simple test for swarm::superTimestamp() */

#include <algorithm>
#include "swarm/api.h"
#include "swarm/algorithm.h"

#include "common.h"

#define NP (1024ul)

std::array<uint64_t, NP> incorrect = {0};

void checkSuperdomainTimestampTask(swarm::Timestamp ts, swarm::Timestamp super_ts) {
    swarm::Timestamp answer = swarm::superTimestamp();
    if (answer != super_ts) {
        swarm::info("swarm::superTimestamp() returned %d, when the timestamp was %d",
                  answer, super_ts);
        incorrect[super_ts] += 1;
    }
}

void createDomainTask(swarm::Timestamp ts) {
    swarm::Timestamp answer = swarm::superTimestamp();
    if (answer != UINT64_MAX) {
        swarm::info("swarm::superTimestamp() returned %d in root domain", answer);
        incorrect[ts] += 1;
    }
    swarm::deepen();
    answer = swarm::superTimestamp();
    if (answer != ts) {
        swarm::info("swarm::superTimestamp() returned %d after deepen", answer);
        incorrect[ts] += 1;
    }
    swarm::enqueue(checkSuperdomainTimestampTask, 42/*random number*/, NOHINT, ts);
    swarm::undeepen();
    answer = swarm::superTimestamp();
    if (answer != UINT64_MAX) {
        swarm::info("swarm::superTimestamp() returned %d after undeepen", answer);
        incorrect[ts] += 1;
    }
}

int main(int argc, const char** argv) {
    swarm::enqueue_all<NOHINT | MAYSPEC>(swarm::u64it(0), swarm::u64it(NP),
        [] (uint64_t i) {
            swarm::enqueue(createDomainTask, i, NOHINT);
    }, 0ul);

    swarm::run();

    tests::assert_eq(0ul,
                     std::accumulate(incorrect.begin(), incorrect.end(), 0ul));
    return 0;
}

