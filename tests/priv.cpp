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

#include "common.h"
#include "swarm/api.h"
#include "swarm/algorithm.h"
#include "swarm/hooks.h"
#include <array>

typedef std::array<uint64_t, 128> Counters;
#define NC (1ul << 12)

// NOTE(dsm): Ignore provided timestamp, which simulator takes ownership of to
// execute abort handlers ars early as possible
void compensation(uint64_t, uint64_t ts, volatile uint64_t* counter) {
    __sync_fetch_and_sub(counter, ts);
}

void task(uint64_t ts, Counters* tracked, Counters* untracked) {
    size_t idx = ts % tracked->size();
    (*tracked)[idx] += ts;
    sim_priv_call();
    volatile uint64_t* counter = &(*untracked)[idx];
    // NOTE: We're in multithreaded land now, atomics are a must
    __sync_fetch_and_add(counter, ts);
    swarm::enqueue(compensation, ts, (EnqFlags)(EnqFlags::SAMEHINT | EnqFlags::CANTSPEC | EnqFlags::RUNONABORT), ts, counter);
    sim_priv_ret();
}

int main(int argc, const char** argv) {
    Counters* tracked = new Counters;
    void* ubuf = sim_zero_cycle_untracked_malloc(sizeof(Counters));
    Counters* untracked = new(ubuf) Counters;

    swarm::enqueue_all<EnqFlags::NOHINT, swarm::max_children-1>(
            swarm::u64it(0), swarm::u64it(NC), [tracked, untracked](uint64_t i) {
        swarm::enqueue(task, i, EnqFlags::NOHINT, tracked, untracked);
    }, 0ul);

    swarm::run();
    for (size_t idx = 0; idx < tracked->size(); idx++) {
        uint64_t ct = (*tracked)[idx];
        uint64_t cu = (*untracked)[idx];
        if (ct != cu) printf("Mismatch @ %ld %ld != %ld\n", idx, ct, cu);
    }

    tests::assert_eq(*tracked, *untracked);
    return 0;
}

