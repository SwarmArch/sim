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

#include <stdint.h>
#include <stdexcept>
#include <iostream>
#include <emmintrin.h>

#include "swarm/api.h"
#include "common.h"

volatile uint64_t counter;

void reader(uint64_t ts, uint64_t upsLeft);
void writer(uint64_t ts, uint64_t upsLeft);

void reader(uint64_t ts, uint64_t upsLeft) {
    const uint64_t sample1 = counter;

    swarm::enqueue(writer, ts+1, EnqFlags::NOHINT, upsLeft);
    for (uint32_t i = 0; i < 1000; i++) _mm_pause();

    const uint64_t sample2 = counter;

    if (sample1 != sample2) throw std::runtime_error("Two reads aren't equal");
    if (sample2 != ts) throw std::runtime_error("Read not as expected");
}

void writer(uint64_t ts, uint64_t upsLeft) {
    if (!upsLeft) return;
    counter = ts + 1;
    swarm::enqueue(reader, ts + 1, EnqFlags::NOHINT, upsLeft - 1);
}

int main(int argc, const char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <nupdates>" << std::endl;
        return -1;
    }
    const uint64_t nupdates = atoi(argv[1]);

    counter = 0;
    swarm::enqueue(reader, 0, EnqFlags::NOHINT, nupdates);
    swarm::run();

    tests::assert_eq(counter, 2 * nupdates);
    return 0;
}

