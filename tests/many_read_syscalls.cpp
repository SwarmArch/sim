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

/**
 * Lots of short unordered system-calling tasks. Each task
 * 1) induces a dependence on a random element of an array
 * 2) make a syscall
 * 3) induces another dependence on a random element of the array
 * As of commit 7b86b37332cacc1, this reproduces the error seen in 470.lbm
 * [sim] Panic on sim/sim.cpp:1089: Task [0x7f8c140cc5f8 (ts=0:114177 softTs=0 fcn=0x401f90 state=1 tid=8 untied irrevocable)] became non-speculative while taking exception (reason: syscall #0)
 */
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstdlib>
#include <iostream>

#include "swarm/api.h"
#include "swarm/algorithm.h"
#include "swarm/rand.h"
#include "common.h"

constexpr uint64_t NC = 256;

std::array<uint64_t, NC> counters;

inline void task(swarm::Timestamp, int filedes) {
    counters[swarm::rand64() % NC]++;
    uint8_t c;
    ssize_t x = read(filedes, &c, 1);
    if (x == 1) counters[c]++;
}

int main(int argc, const char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <number of reads>" << std::endl;
        return -1;
    }
    uint64_t nreads = atoi(argv[1]);

    int f = open("/dev/urandom", O_RDONLY);

    swarm::enqueue_all<NOHINT | MAYSPEC>(
            swarm::u64it(0), swarm::u64it(nreads),
            [f] (uint64_t) { swarm::enqueue(task, 0ul, NOHINT, f); },
            0ul);
    swarm::run();

    close(f);

    uint64_t actual = std::accumulate(counters.begin(), counters.end(), 0ul);
    tests::assert_eq(2 * nreads, actual);
    return 0;
}
