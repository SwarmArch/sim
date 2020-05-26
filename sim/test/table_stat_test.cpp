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

#include <gtest/gtest.h>
#include <iostream>
#include <vector>
#include "sim/stats/table_stat.h"

class TableStatTest : public ::testing::Test {
  protected:
    const std::vector<std::string> rows = {"r1","r2","r3","r4","r5"};
    const std::vector<std::string> cols = {"c1","c2","c3"};

    // TODO(mcj) this should probably be parameterized, or even randomized?
    const uint64_t increments[5][3] = {
         {5,6,1},
         {3,3,3},
         {1<<20,3<<3,4<<4},
         {1<<1,32ul<<32,20<<20},
         {33ul<<33,2,3}
    };

    TableStat ts;

    TableStatTest() { ts.init("dontcare", "dontcare", rows, cols); }

    void fill(TableStat& ts) {
        for (size_t r = 0; r < rows.size(); r++) {
            for (size_t c = 0; c < cols.size(); c++) {
                ts.inc(r, c, increments[r][c] - 1);
                ts.inc(r, c);
            }
        }
    }
};


static void AssertCellEquals(uint64_t expected, const TableStat& ts,
                             uint32_t r, uint32_t c) {
    ASSERT_EQ(expected, ts.row(r).count(c));
    ASSERT_EQ(expected, ts.element(r, c));
}


TEST_F(TableStatTest, TableInitiallyContainsZeros) {
    for (size_t r = 0; r < rows.size(); r++) {
        for (size_t c = 0; c < cols.size(); c++) {
            AssertCellEquals(0, ts, r, c);
        }
    }
    ASSERT_EQ(0UL, ts.total());
}


TEST_F(TableStatTest, IncrementingOneCellLeavesOthersZero) {
    ts.inc(0, 0);
    AssertCellEquals(1, ts, 0, 0);

    for (size_t r = 0; r < rows.size(); r++) {
        for (size_t c = 0; c < cols.size(); c++) {
            if (!(r == 0 && c == 0)) AssertCellEquals(0, ts, r, c);
        }
    }
    ASSERT_EQ(1UL, ts.total());
}


TEST_F(TableStatTest, IncrementingEveryCellIsCountedCorrectly) {
    fill(ts);

    for (size_t r = 0; r < rows.size(); r++) {
        for (size_t c = 0; c < cols.size(); c++) {
            AssertCellEquals(increments[r][c], ts, r, c);
        }
    }

    uint64_t expected = std::accumulate(
            (uint64_t*)increments,
            (uint64_t*)increments + sizeof(increments) / sizeof(uint64_t),
            0ul);
    ASSERT_EQ(expected, ts.total());
}


TEST_F(TableStatTest, IncrementsRowFromVectorCounter) {
    fill(ts);

    VectorCounter vc;
    vc.init("dontcare", "dontcare", cols.size());
    for (size_t c = 0; c < cols.size(); c++) vc.inc(c);

    const uint64_t r = 0;
    ts.row(r) += vc;
    for (size_t c = 0; c < cols.size(); c++) {
        AssertCellEquals(1 + increments[r][c], ts, r, c);
    }
}


TEST_F(TableStatTest, DecrementsRowFromVectorCounter) {
    fill(ts);

    VectorCounter vc;
    vc.init("dontcare", "dontcare", cols.size());
    for (size_t c = 0; c < cols.size(); c++) vc.inc(c);

    const uint64_t r = 0;
    ts.row(r) -= vc;
    for (size_t c = 0; c < cols.size(); c++) {
        AssertCellEquals(increments[r][c] - 1, ts, r, c);
    }
}


TEST_F(TableStatTest, IncrementsFromTableStat) {
    fill(ts);

    TableStat other;
    other.init("dontcare", "dontcare", rows, cols);
    fill(other);

    ts += other;

    for (size_t r = 0; r < rows.size(); r++) {
        for (size_t c = 0; c < cols.size(); c++) {
            AssertCellEquals(2 * increments[r][c], ts, r, c);
        }
    }
}


TEST_F(TableStatTest, DecrementsFromTableStat) {
    fill(ts);

    TableStat other;
    other.init("dontcare", "dontcare", rows, cols);
    fill(other);

    ts -= other;

    for (size_t r = 0; r < rows.size(); r++) {
        for (size_t c = 0; c < cols.size(); c++) {
            AssertCellEquals(0, ts, r, c);
        }
    }
}
