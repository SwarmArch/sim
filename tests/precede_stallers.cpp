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
 * Let t be the task-after-the-GVT-holding task. This test exposes what happens
 * when t is enqueued to a task unit whose current cores are all stalling on an
 * enqueue. When t becomes the GVT-holding task, no core can execute it because
 * all cores are busy uselessly stalling, causing deadlock.
 * Task t **precedes the stallers**.
 *
 * For the details:
 * 'first' is the first task to run, and it enqueues several 'seconds', and
 * several 'fillers'. The fillers fill their local task queue with 'spinners',
 * to the point that they should stall on an enqueue; the fillers are ordered
 * after 'seconds'.
 *
 * One of the 'seconds' is the task t named above. It is re-enqueued after all
 * fillers have stalled, by prematurely reading boolean 'firstFinished' which
 * task 'first' sets to true after it has finished executing. Task t is aborted
 * by 'first' and re-enqueued to a stalled task unit.
 */

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>
#include <emmintrin.h>
#include <boost/iterator/counting_iterator.hpp>
#include "swarm/api.h"
#include "swarm/algorithm.h"
#include "swarm/aligned.h"
#include "common.h"

swarm::aligned<bool> firstFinished = false;
std::vector<swarm::aligned<bool> > successes;

void filler(swarm::Timestamp ts, uint64_t numSpinners) {
    swarm::enqueue_all<SAMEHINT | MAYSPEC>(
            swarm::u64it(0), swarm::u64it(numSpinners), [ts](uint64_t c) {
        // This enqueue should eventually stall on its own task queue
        swarm::enqueueLambda([](swarm::Timestamp) {
            for (uint32_t i = 0; i < 10; i++) _mm_pause();
        }, ts + 1, SAMEHINT | MAYSPEC);
    }, ts);
}

// This will run twice: the first time reading an incorrect false value,
// then will be aborted, soon after which one of the "seconds" will hold up the
// GVT, but the fillers have stalled on their task queues. The aborting of this
// second task must push its way to getting onto a core.
void second(swarm::Timestamp ts, uint32_t idx) {
    successes[idx] = firstFinished;
}

void first(swarm::Timestamp ts, uint64_t spinners) {
    const uint32_t nthreads = swarm::num_threads();
    swarm::enqueue_all<NOHINT | MAYSPEC, swarm::max_children/2>(
            swarm::u64it(0), swarm::u64it(nthreads),
            [ts](uint64_t i) {
        swarm::enqueue(second, ts + 1, {i, NOHASH | MAYSPEC}, i);
    }, ts);

    swarm::enqueue_all<NOHINT | MAYSPEC, swarm::max_children/2>(
            swarm::u64it(0), swarm::u64it(nthreads),
            [ts, spinners](uint64_t i) {
        swarm::enqueue(filler, ts + 2, {i, NOHASH | MAYSPEC}, spinners);
    }, ts);

    // Artificially delay commit
    for (uint32_t i = 0; i < 10000; i++) _mm_pause();

    firstFinished = true;
}

int main(int argc, const char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <n spinners>" << std::endl;
        return -1;
    }

    successes.resize(swarm::num_threads());
    std::fill(successes.begin(), successes.end(), false);

    swarm::enqueue(first, 0, NOHINT, atoi(argv[1]));
    swarm::run();

    tests::assert_true(std::all_of(successes.begin(), successes.end(),
                [] (bool b) { return b; }));

    return 0;
}
