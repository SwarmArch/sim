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
 * Used to tests the simulator's gurad against tasks that overwrite its own
 * memory. No way to test systematically...
 */
#include <emmintrin.h>
#include "swarm/api.h"

int main(int argc, const char** argv) {
    bool done1 = false;
    bool done2 = false;
    swarm::enqueueLambda([&done1](swarm::Timestamp ts) {
        for (uint32_t i = 0; i < 10000; i++) _mm_pause();
        done1 = true;
    }, 0ul, EnqFlags::NOHINT);
    swarm::enqueueLambda([&done1, &done2](swarm::Timestamp ts) {
        if (!done1) {
            // First address in range
            *(uint64_t*)(0x0d0000000000ul) = 0xDEADBEEF;
        }
        done2 = true;
    }, 1ul, EnqFlags::NOHINT);
    swarm::enqueueLambda([&done2](swarm::Timestamp ts) {
        if (!done2) {
            // Last address in range
            *(uint64_t*)(0x0dfffffffff0ul) = 0xCAFEBABE;
        }
    }, 2ul, EnqFlags::NOHINT);
    swarm::run();
    return 0;
}
