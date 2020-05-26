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
#include <malloc.h>
#include "swarm/algorithm.h"
#include "swarm/api.h"
#include "common.h"

static void readTask(swarm::Timestamp ts, uint64_t **p) {
    if (**p != ts) {
        swarm::info("Incorrect value %ld != %ld.", **p, ts);
        std::abort();
    }
}

static void writeTask(swarm::Timestamp ts, uint64_t **p) {
    **p = ts;
    swarm::enqueue(readTask, ts, NOHINT, p);
}

static void reallocTask(swarm::Timestamp ts, uint64_t **p) {
    size_t sz = malloc_usable_size(*p);
    *p = (uint64_t*)realloc(*p, 4 * sz);
}

static void freeTask(swarm::Timestamp ts, uint64_t **p) {
    free(*p);
    free(p);
}

static void allocTask(swarm::Timestamp ts) {
    uint64_t *val = (uint64_t*)malloc(sizeof(*val));
    uint64_t **p = (uint64_t**)malloc(sizeof(*p));
    *p = val;
    swarm::enqueue(writeTask, ts, NOHINT, p);
    swarm::enqueue(reallocTask, ts, NOHINT, p);
    swarm::enqueue(freeTask, ts+1, NOHINT, p);
}

int main(int argc, const char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0]
                  << " <numallocs>" << std::endl;
        std::abort();
    }
    uint64_t numallocs = atoi(argv[1]);

    swarm::enqueue_all<NOHINT | MAYSPEC>(
            swarm::u32it(0),
            swarm::u32it(numallocs),
            [=] (int i) {
        swarm::enqueue(allocTask, i, NOHINT);
    }, 0);

    swarm::run();
    tests::assert_true(true);
    return 0;
}
