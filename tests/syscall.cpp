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

#include <stdlib.h>
#include <array>
#include <iostream>
#include <algorithm>

#include "swarm/api.h"
#include "common.h"

const char* words[] = {"The", "quick", "brown", "fox", "jumps", "over", "the", "lazy", "dog"};
uint64_t count;

inline void printTask(swarm::Timestamp ts) {
    printf("%s\n", words[/*count++*/ ts]);
    fflush(stdout);
}

int main(int argc, const char** argv) {
    count = 0;
    for (uint64_t i = 0; i < sizeof(words)/sizeof(const char*); i++) {
        swarm::enqueue(printTask, i, EnqFlags::NOHINT);
    }

    swarm::run();
    tests::assert_true(true);
    return 0;
}

