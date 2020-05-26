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
 * This family of tests trigger speculative buffer overflows. In one case the
 * test overflows requeuers' spill buffers with garbage data. In another case,
 * the test overwrites the simulator's dlmalloc metadata structure.
 *
 * At the time of its inception, the test also reproduces a simulator bug where
 * a requeuer that hit an exception is dequeued.
 *
 * Details:
 * A global variable, test::buffer, is heap-allocated one uint64_t. This roughly
 * represents the base of the heap. This is the same heap on which
 * spiller/requeuer task buffers are also stored. (Note if we ever move
 * spiller/requeuer memory to its own distinct heap region, then this test
 * won't reproduce the bug any more). The global variable test::size is
 * incorrectly initialized with the user's input value (it should be one).
 *
 * The function fillBuffer reads the incorrect size value, and overflows to
 * memory locations it shouldn't touch. Meanwhile, one tile is filled beyond
 * capacity, so tasks are spilled to (heap) memory.
 *
 * At its inception, this test would have fillBuffer overwrite some requeuer's
 * task memory buffer, leading to a requeuer exception. The simulator should be
 * able to handle this, but at the time it didn't.
 */

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>
#include <emmintrin.h>
#include "swarm/api.h"
#include "swarm/algorithm.h"
#include "common.h"

namespace test {
static uint64_t* buffer;
static uint64_t size;
}

static constexpr uint64_t FILLER = 0xf00d;

#ifdef OVERFLOW_ZEROTH_TILE
// The buffer is allocated outside any task and therefore is allocated in the
// same pool as core 0 (yikes, this depends too much on the simulator
// implementation; the test is fragile).
static constexpr swarm::Hint overflowHint(0, NOHASH);
#else
// In this test we want the requeuers to be in a different tile than core 0,
// and in fact we want so many spinners that the __enqueuers themselves create
// requeuers at every tile.
static constexpr swarm::Hint overflowHint(1, NOHASH);
#endif


void fillBuffer(swarm::Timestamp) {
    std::fill(test::buffer, test::buffer + test::size, FILLER);
}

void spinner(swarm::Timestamp) {
    for (uint32_t i = 0; i < 100; i++) _mm_pause();
}

void updateSize(swarm::Timestamp, uint64_t size) {
    // Spin wait for some time to let the fillBuffer task (speculatively)
    // overwrite memory.
    for (uint64_t i = 0; i < std::max(60000ul, test::size); i++) _mm_pause();
    test::size = size;
}

static int overwrite(int argc, const char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <buffer size>" << std::endl;
        return -1;
    }

    // Initialize buffer as a 1-element array, but set the size incorrectly.
    test::buffer = new uint64_t[1ul];
    test::size = atoi(argv[1]);
    assert(test::size > 1ul);

    // Fill up one tile with spinners
    // (Note, these are ordered after fillBuffer, so that the latter will
    // definitely get a commit queue slot to start running.)
    swarm::enqueue_all<NOHINT | MAYSPEC, swarm::max_children - 2>(
            swarm::u32it(0), swarm::u32it(test::size), [] (uint32_t i) {
        swarm::enqueue(spinner, 3 + i, overflowHint);
    }, 0ul);

    // Enqueue fillBuffer to a different tile. Let it overwrite heap memory,
    // while speculative. Then a task with lower timestamp updates the buffer
    // size to the correct value of 1.
    swarm::enqueue(fillBuffer, 2ul, {2, NOHASH});
    swarm::enqueue(updateSize, 1ul, {3, NOHASH}, 1ul);

    swarm::run();

    tests::assert_eq(FILLER, *test::buffer);
    delete [] test::buffer;
    return 0;
}
