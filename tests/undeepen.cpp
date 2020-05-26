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

#include "swarm/api.h"
#include "swarm/algorithm.h"
#include "common.h"

static void incrementTask(swarm::Timestamp, uint64_t *x) { ++*x; }
static void doubleTask(swarm::Timestamp, uint64_t *x) { *x *= 2; }
static void finalTask(swarm::Timestamp, uint64_t *x) {
    if (*x != 4)
        std::abort();
    delete x;
}

static void rootTask(swarm::Timestamp ts) {
    uint64_t *x = new uint64_t;
    *x = 1;
    swarm::deepen();
    swarm::enqueue(incrementTask, ts+42, EnqFlags::NOHINT, x);
    swarm::undeepen();
    ts = swarm::timestamp();
    swarm::enqueue(doubleTask, ts, EnqFlags::NOHINT, x);
    swarm::enqueue(finalTask, ts+1, EnqFlags::NOHINT, x);
}


int main(int argc, const char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0]
                  << " <numTrials>" << std::endl;
        std::abort();
    }

    swarm::enqueue_all<NOHINT>(swarm::u64it(0), swarm::u64it(atoi(argv[1])),
        [] (uint64_t i) {
            swarm::enqueue(rootTask, i, EnqFlags::NOHINT);
    }, 0ul);
    swarm::run();

    tests::assert_true(true);

    return 0;

}
