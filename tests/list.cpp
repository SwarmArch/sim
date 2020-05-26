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
 * This test is intended to stress the memory allocator in the face aborts.
 * The tests::append tasks append ints to the end of the list, while each
 * enqueuing a remover. The remover tasks remove from the end of the list, and
 * execute in the opposite order from the appenders. Appender a, which pushed
 * a.c to the list, will enqueue remover r that expects to see a.c at the end of
 * the list.
 */
#include <cstdlib>
#include <iostream>
#include <list>
#include <vector>
#include <algorithm>

#include "common.h"
#include "swarm/api.h"
#include "swarm/aligned.h"


std::list<uint64_t> list;
std::vector<swarm::aligned<bool> > correctorder;

namespace tests {

static void remove(swarm::Timestamp, uint64_t expected) {
    correctorder[expected] = expected == list.back();
    list.pop_back();
}

static void append(swarm::Timestamp ts, int updates) {
    if (pls_unlikely(updates == 0)) return;
    list.push_back(ts);
    swarm::enqueue(tests::append, ts + 1, NOHINT | SAMETASK, updates - 1);
    // Remove elements in reverse order as they were enqueued
    swarm::enqueue(tests::remove, ts + 2 * updates, ts, ts);
}

}

int main(int argc, const char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <nupdates>" << std::endl;
        return -1;
    }

    int nupdates = atoi(argv[1]);
    assert(nupdates >= 0);

    correctorder.resize(nupdates);
    std::fill(correctorder.begin(), correctorder.end(), false);

    swarm::enqueue(tests::append, 0, NOHINT, nupdates);
    swarm::run();

    tests::assert_true(
            std::all_of(correctorder.begin(), correctorder.end(),
                        [] (bool correct) { return correct; }));
    return 0;
}

