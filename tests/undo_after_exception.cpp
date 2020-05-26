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

/* A task that hits an exception must undo its speculative writes before it
 * proceeds to execute a second time.
 *
 * The 'exception' task transitions the 'finished' variable from false to true.
 * However it should read a value of false first. If it does not undo its
 * speculative writes before executing a second time, the exception task will
 * read true.
 *
 * The 'delayer' task exists to hold back the GVT so that the exception task
 * does trigger an exception.
 */

#include <emmintrin.h>
#include "swarm/api.h"
#include "common.h"
#include "swarm/aligned.h"


swarm::aligned<bool> finished = false;
swarm::aligned<bool> success = false;

void exception(swarm::Timestamp) {
    success = !finished;
    finished = true;

    // Trigger an exception
    swarm::serialize();
}

void delayer(swarm::Timestamp ts) {
    swarm::enqueue(exception, ts, EnqFlags::NOHINT);

    // Artificially delay commit
    for (uint32_t i = 0; i < 10000; i++) _mm_pause();
}

int main(int argc, const char** argv) {
    swarm::enqueue(delayer, 0, EnqFlags::NOHINT);
    swarm::run();

    tests::assert_true(success);
    return 0;
}
