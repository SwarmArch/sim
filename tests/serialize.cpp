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

/* Simple test for swarm::serialize() (hard to do automatically, since PLS
 * serializes side-effects automatically).
 *
 * If serialize() call is not commented, numbers will be printed out in order
 * (this relies on the fact that swarm::info from speculative tasks is permitted.
 *
 * Needs to run on a system with enough task queue entries.
 */

#define NP (1024ul)

inline void task(swarm::Timestamp ts) {
    swarm::serialize();
    swarm::info("%ld", ts);
}

int main(int argc, const char** argv) {
    for (uint64_t i = 0; i < NP; i++) {
        swarm::enqueue(task, i, EnqFlags::NOHINT);
    }

    swarm::run();
    return 0;
}

