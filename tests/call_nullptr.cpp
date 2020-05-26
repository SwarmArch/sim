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

/*
 * Speculatively call a function pointer that is null.
 */

#include <cstdint>
#include <emmintrin.h>

#include "common.h"
#include "swarm/api.h"

bool success = false;
void (*fcnptr)(void) = nullptr;

void beSuccessful() { success = true; }

static inline void pauser(swarm::Timestamp ts) {
    for (uint32_t i = 0; i < 5000; i++) _mm_pause();
    fcnptr = &beSuccessful;
}

static inline void exceptioner(swarm::Timestamp) {
    (*fcnptr)();
}

int main() {
    swarm::enqueue(pauser, 0, EnqFlags::NOHINT);
    swarm::enqueue(exceptioner, 1, EnqFlags::NOHINT);
    swarm::run();
    tests::assert_eq(success, true);
    return 0;
}
