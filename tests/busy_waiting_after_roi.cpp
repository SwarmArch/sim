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
#include "swarm/hooks.h"
#include <thread>

constexpr int MAX_THREADS = 2048;

int counters[MAX_THREADS];

#define DO_PTHREAD 1

#if DO_PTHREAD
void *barrier_then_loop(void *args) {
    sim_barrier();
    sim_barrier();

    counters[(long)args] = (long)args;

    sim_barrier();
    sim_barrier();
    while (true);
}
#else
void barrier_then_loop(int tid) {
    sim_barrier();
    sim_barrier();

    counters[tid] = tid;

    sim_barrier();
    sim_barrier();
    while (true);
}
#endif

int main(int argc, const char** argv) {
    const int numThreads = swarm::num_threads();
    assert(numThreads <= MAX_THREADS);
#if DO_PTHREAD
    pthread_t t;
    for (long i = 1; i < numThreads; i++)
        pthread_create(&t, NULL, barrier_then_loop, (void *)i);
#else
    std::thread* threads = new std::thread[numThreads];
    for (int i = 1; i < numThreads; i++)
        threads[i] = std::thread(barrier_then_loop, i);
#endif

    sim_barrier();
    zsim_roi_begin();
    sim_barrier();

    sim_barrier();
    zsim_roi_end();
    sim_barrier();

    int total = 0;
    for (int i = 0; i < numThreads; i++)
        total += counters[i];

    tests::assert_eq(numThreads * (numThreads - 1) / 2, total);

    return 0;
}
