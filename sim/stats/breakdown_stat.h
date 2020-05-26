/** $lic$
 * Copyright (C) 2012-2021 by Massachusetts Institute of Technology
 * Copyright (C) 2010-2012 by The Board of Trustees of Stanford University
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

/* This file was adapted from zsim. */

#ifndef _BREAKDOWN_STAT_H_
#define _BREAKDOWN_STAT_H_

#include <cstring>
#include <functional>
#include <string>
#include "sim/stats/stats.h"
#include "sim/stats/table_stat.h"
#include "sim/types.h"

/* Keeps track of the cycles/times/values spent in each of a number of states.
 * Uses an std::function to retrieve the current value (e.g., cycle count) on
 * each transition. This function must always return increasing values.
 */

class BreakdownStat : public TableStat {
public:
    BreakdownStat(std::function<uint64_t()> _valFn)
        : valFn(_valFn), curRow(0), curCol(0), curVal(0ul) { }

    void transition(uint32_t r, uint32_t c, uint64_t forceVal=INVALID_INT) {
        uint64_t newVal;
        if (forceVal == INVALID_INT) {
            newVal = valFn();
        } else {
            uint64_t tempVal = valFn();
            assert(forceVal <= tempVal);
            newVal = forceVal;
        }
        if (newVal < curVal) warn("BreakdownStat: went back in time... %ld -> %ld (should be benign)", curVal, newVal);
        inc(curRow, curCol, newVal - curVal);

        curRow = r;
        curCol = c;
        curVal = newVal;
    }

    void updateImmediate(uint64_t forceVal=INVALID_INT) {
        transition(curRow, curCol, forceVal);
    }

    // Called before each stats dump
    void update() override {
        // Force a fake transition to the same place to refresh the value in curRow, curCol
        transition(curRow, curCol);
    }

private:
    const std::function<uint64_t()> valFn;
    uint32_t curRow;
    uint32_t curCol;
    uint64_t curVal;
};

#endif  // _BREAKDOWN_STAT_H_
