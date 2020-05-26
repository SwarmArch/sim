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

#if 0 // dsm: doesn't compile, as this requires Pin now
#include <gtest/gtest.h>
#include "stack.h"

class StackSegmentsTest : public ::testing::Test {
protected:
    static const uint32_t nthreads = 8;
    static const uint32_t logSegSize = 10;
    static const uint32_t totalSize = nthreads << logSegSize;
    const StackSegments stack;
    const Address base;

    StackSegmentsTest() : stack(nthreads, logSegSize), base(stack.base()) {}
};

TEST_F(StackSegmentsTest, AddressLessThanBaseNotInSegment) {
    ASSERT_EQ(-1, stack.segment(base - 1));
}

TEST_F(StackSegmentsTest, AddressInBaseSegmentIsZero) {
    ASSERT_EQ(0, stack.segment(base + 1));
}

TEST_F(StackSegmentsTest, AddressInTopSegmentIsTop) {
    Address addr = base + totalSize - 1;
    ASSERT_EQ(nthreads - 1, stack.segment(addr));
}

TEST_F(StackSegmentsTest, AddressJustAboveTopNotInSegment) {
    ASSERT_EQ(-1, stack.segment(base + totalSize));
}
#endif
