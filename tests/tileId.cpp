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

/* Simple test for swarm::tileId() and swarm::numTiles() */

#include <algorithm>
#include "swarm/api.h"
#include "swarm/algorithm.h"

#include "common.h"

constexpr unsigned NP = 1024u;

std::array<uint64_t, NP> counters = {0};

static void checkSameHint(swarm::Timestamp, unsigned tileId) {
    assert(tileId == swarm::tileId());
}

static void task(swarm::Timestamp ts, int i) {
    const unsigned tileId = swarm::tileId();
    assert(tileId < swarm::numTiles());
    assert(i % swarm::numTiles() == swarm::tileId());
    counters[tileId] += i;
    swarm::enqueue(checkSameHint, ts, SAMEHINT | NONSERIALHINT, tileId);
}

int main(int argc, const char** argv) {
    assert(swarm::tileId() == 0);
    assert(swarm::tid() == 0);
    std::fill(counters.begin(), counters.end(), 0ul);

    const auto numTiles = swarm::numTiles();
    assert(numTiles < NP);

    swarm::enqueue_all<NOHINT | MAYSPEC>(swarm::u64it(0), swarm::u64it(numTiles * 2),
        [] (uint64_t i) {
            swarm::enqueue(task, i, {i, NOHASH | NONSERIALHINT}, i);
    }, 0ul);

    swarm::run();

    std::array<uint64_t, NP> expected = {0};
    for (unsigned i = 0; i < numTiles; i++) expected[i] = 2 * i + numTiles;
    tests::assert_eq(expected, counters);
    return 0;
}
