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
 * This test was intended to reproduce a suspicion that mispeculating tasks
 * could clobber undo logs, while they aren't allocated in untracked memory.
 * (this is during the era where pinStart == pinEnd).
 *
 * This test doesn't provide any evidence of undo log overwrites, but does
 * reproduce the warning:
 * WARN: Undo log walk of [... untied ex received signal 11 @ PC 0x4020e4)] failed to restore ...
 * which might be instructional for swarm/Swarm-IR#657. I think those warnings
 * suggest that exception handling somehow does not block a bad store from
 * appending to the undo log.
 */

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>
#include <emmintrin.h>
#include "swarm/api.h"
#include "swarm/algorithm.h"
#include "swarm/aligned.h"
#include "common.h"

constexpr uint64_t NC = 1000;
constexpr uint64_t FILLER = 0xf00d;

namespace test {
static uint64_t* buffer;
static uint64_t size;

using aligned_block = uint64_t[NC];
alignas(64) static aligned_block safe_buffer;
}


void mispeculator(swarm::Timestamp) {
    std::fill(test::buffer, test::buffer + test::size, FILLER);
}


void logger(swarm::Timestamp) {
    std::fill(test::safe_buffer,
              test::safe_buffer + NC,
              FILLER);
}


void updateBufferAndSize(swarm::Timestamp, uint64_t* buffer, uint64_t size) {
    // Spin wait for some time to let the mispeculator task (speculatively)
    // overwrite memory.
    for (uint64_t i = 0; i < std::max(60000ul, test::size); i++) _mm_pause();
    test::buffer = buffer;
    test::size = size;
}


int main(int argc, const char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <buffer size>" << std::endl;
        return -1;
    }

    // buffer should be a 1-element array, but we initialize the size and
    // pointer incorrectly, and let the task updateBufferAndSize correct them
    uint64_t correct_buffer[1ul];
    test::buffer = reinterpret_cast<uint64_t*>(&logger);
    test::size = atoi(argv[1]);
    assert(test::size > 1ul);

    // Create a logger (aka it stores a lot to safe_buffer, and grows an
    // undolog). Note, this is ordered after mispeculator, so that the latter
    // will definitely get a commit queue slot to start running.
    swarm::enqueue(logger, 3, EnqFlags::NOHINT);

    // Enqueue fillBuffer to a different tile. Let it overwrite heap memory,
    // while speculative. Then a task with lower timestamp updates the buffer
    // size to the correct value of 1.
    swarm::enqueue(mispeculator, 2, EnqFlags::NOHINT);
    swarm::enqueue(updateBufferAndSize, 1, EnqFlags::NOHINT, correct_buffer, 1ul);

    swarm::run();

    tests::assert_true(
            test::buffer == correct_buffer
            && *test::buffer == FILLER);
    return 0;
}
