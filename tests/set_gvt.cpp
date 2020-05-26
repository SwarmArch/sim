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

#include <iostream>
#include "swarm/api.h"
#include "swarm/algorithm.h"
#include "swarm/aligned.h"
#include "common.h"

#undef DEBUG
#define DEBUG(args...) info(args)

#define NC (4)

using namespace std;
using swarm::info;

swarm::aligned<bool> flag;
std::array<swarm::aligned<uint64_t>, NC> counters;
std::array<swarm::aligned<uint64_t>, NC> barrierValue;
std::array<swarm::aligned<uint64_t>, NC> barrierLevel;
std::array<swarm::aligned<bool>, NC> seen;
uint64_t levels;

inline void chainTask(swarm::Timestamp ts, uint64_t curLevel, uint32_t c);
inline void triggerTask(swarm::Timestamp ts, uint64_t curLevel, uint32_t c);

inline void phaseBarrierTask(swarm::Timestamp ts, uint32_t c) {
    swarm::setGvt(0);
    flag = true;
    barrierValue[c] = counters[c];
    for (uint32_t i = 0; i < NC; ++i)
        barrierValue[i] = counters[i];
    // By pulling back the gvt we can enqueue a task with ts = 1
    swarm::enqueue(triggerTask, 1, NOHINT, levels, c);
}

inline void triggerTask(swarm::Timestamp ts, uint64_t curLevel, uint32_t c) {
    if (flag == false)
        counters[c] += 4;
    else
        counters[c]--;

    --curLevel;
    if (curLevel == 0) {
        if (flag == true) return;
        else
           swarm::enqueue(phaseBarrierTask, ts, NOHINT, c);
    } else {
        swarm::enqueue(triggerTask, ts + 1, NOHINT, curLevel, c);
    }
}

inline void chainTask(swarm::Timestamp ts, uint64_t curLevel, uint32_t c) {
    if (flag == false)
        counters[c] += 4;
    else
        counters[c]--;

    if ((flag == true) && (seen[c] == false)) {
        seen[c] = true;
        barrierLevel[c] = curLevel;
        swarm::enqueue(chainTask, ts + 1, NOHINT, levels-1, c);
        return;
    } else if (flag == true) {
        curLevel--;
        if (curLevel == 0)
            return;
        else
            swarm::enqueue(chainTask, ts + 1, NOHINT, curLevel, c);
    } else {
        swarm::enqueue(chainTask, ts + 1, NOHINT, curLevel, c);
    }
}

inline void startTask(swarm::Timestamp ts, uint64_t levels) {
    swarm::enqueue(triggerTask, levels + 1, NOHINT, levels, 0);
    for (uint32_t i = 1; i < NC; ++i) {
        swarm::enqueue(chainTask, levels + 1, NOHINT, levels, i);
    }
}

inline void initialize() {
    flag = false;
    for (uint32_t c = 0; c < NC; ++c) {
        counters[c] = levels;
        barrierValue[c] = 0;
        seen[c] = false;
    }
}

int main(int argc, const char** argv) {
    if (argc != 2) {
        cerr << "Usage: " << argv[0] << " <nlevels>" << endl;
        return -1;
    }
    levels = atoi(argv[1]);
    initialize();

    swarm::enqueue(startTask, 0, 0, levels);
    swarm::run();

    uint64_t sum1 = 0, sum2 = 0;
    for (uint32_t c = 0; c < NC; ++c) {
        uint64_t ctr = counters[c];
        uint64_t bValue = barrierValue[c];
        uint64_t bLevel = barrierLevel[c];
        sum1 += ctr; sum2 += bValue;
        printf("%u: %lu (%lu, %lu)\n", c, ctr, bValue, bLevel);
    }
    tests::assert_true((sum2 - sum1) == (NC * levels));

    return 0;
}

