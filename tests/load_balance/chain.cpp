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
 */
#include <iostream>
#include <cmath>
#include <numeric>
#include <boost/iterator/counting_iterator.hpp>
#include "common.h"

#include "swarm/api.h"
#include "swarm/algorithm.h"
#include "hash.h"
#include "swarm/aligned.h"

using namespace std;
using swarm::info;
typedef boost::counting_iterator<uint64_t> u64it;

#ifdef INSUFFICIENT_PARALLELISM
#define NC (16ul)
#define NS (4ul)
#define NT (4ul)
#define HIGH_INTENSITY (32u)
#define LOW_INTENSITY (8u)

#else // SUFFICIENT_PARALLELISM

#define NC (64ul)
#define NS (4ul)
#define NT (4ul)
#define HIGH_INTENSITY (8u)
#define LOW_INTENSITY (2u)              // Spills dominate if we use (32, 8).
                                        // Overall same # of initial tasks.

#endif

#undef DEBUG
#define DEBUG(args...) info(args)

std::array<std::array<swarm::aligned<uint64_t>, NS>, NC> counters;
std::array<uint32_t, NC> intensity;

H3HashFamily hashFn(1, 32, 0xF00BAD);   // Same hash function used in simulator
                                        // TODO Eventually pull out the hash header files
                                        // to the(/a) common include(?) directory.

void initialize() {
    for (uint32_t t = 0; t < NT; ++t)
        if (t % 2 == 0) intensity[t] = HIGH_INTENSITY;
        else intensity[t] = LOW_INTENSITY;

    for (auto &c: counters) {
        for (auto &e : c)
            e = 0;

        uint32_t elem = &c - &counters[0];
        uint32_t tile = hashFn.hash(0, elem) % NT;
        uint32_t bucket = (hashFn.hash(0, elem)) % (1024)/*total buckets*/;
        DEBUG("c: %u, tile: %u, intensity: %u, bucket: %u", elem, tile, intensity[tile], bucket);
    }
}

inline void task(swarm::Timestamp ts, uint64_t c, uint64_t maxLevels) {
    for (auto &e : counters[c]) e++;

    if (pls_likely(ts - maxLevels > 0))
    swarm::enqueue(task, ts+1, c, c, maxLevels);
}

inline void enqueuerTask(swarm::Timestamp ts, uint64_t c, uint64_t maxLevels) {
    uint32_t tile = hashFn.hash(0, c) % NT;
    swarm::enqueue_all(u64it(0), u64it(intensity[tile]), [c, maxLevels](uint64_t x/*unused*/) {
        swarm::enqueue(task, 1/*ts*/, c/*hint*/, /*args...*/c, maxLevels);
    },
    [] (uint64_t) { return 0ul; },
    [c] (uint64_t) { return c; }
    );
}

inline void startTask(swarm::Timestamp ts, uint64_t levels) {
    for (uint32_t c = 0; c < NC; ++c) {
        printf("c: %u, tile: %lu, intensity: %u\n", c, hashFn.hash(0, c) % NT, intensity[hashFn.hash(0, c) % NT]);
        swarm::enqueue(enqueuerTask, 0/*ts*/, c/*hint*/, /*args...*/c, levels);
    }
}

int main(int argc, const char** argv) {
    if (argc != 2) {
        cerr << "Usage: " << argv[0] << " <nlevels>" << endl;
        return -1;
    }
    uint64_t levels = atoi(argv[1]);

    initialize();

    swarm::enqueue(startTask, 0, 0, levels);

    swarm::run();

    printf("Counters:\n");
    for (uint32_t c = 0; c < NC; ++c) {
        uint32_t thisIntensity = intensity[hashFn.hash(0,c)%NT];
        uint64_t expected = thisIntensity * levels * NS;
        uint64_t actual = std::accumulate(counters[c].begin(), counters[c].end(), 0ul);
        printf("%u: %lu(%lu)\n", c, actual, expected);
    }

    return 0;
}
