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

/* Used for testing early program termination using heartbeats. Should not be
 * an automated test. You should manually check that the number of committed
 * heartbeatTasks() + 2 matches maxHeartbeats. The +2 comes from (a) the last
 * commit not being counted, and (b) the program calling zsim_heartbeat outside
 * of a task. The test is designed to make aborts frequent. Note there are also
 * enqueuer tasks, so run with sim.profileByPc == True.
 */

#define NC (128ul)
static std::array<uint64_t, NC> counters;

static void task(uint64_t ts) {
    zsim_heartbeat();
    counters[ts % NC]++;
}

int main(int argc, const char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <nupdates>" << std::endl;
        std::abort();
    }
    uint64_t nupdates = atoi(argv[1]);
    std::fill(counters.begin(), counters.end(), 0ul);

    zsim_heartbeat();

    swarm::enqueue_all<NOHINT>(
        swarm::u64it(0), swarm::u64it(nupdates),
        [](uint64_t i) { swarm::enqueue(task, i, NOHINT); }, 0ul);
    swarm::run();
}
