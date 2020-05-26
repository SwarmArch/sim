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

#include <cstdint>
#include <gtest/gtest.h>
#include "swarm/aligned.h"

class AlignedUint64Test : public ::testing::Test {
protected:
    swarm::aligned<uint64_t> n;
};


TEST_F(AlignedUint64Test, PrimitiveAssignmentTest) {
    n = 3ul;

    ASSERT_EQ(3ul, n);
    ASSERT_EQ(n, 3ul);
}

TEST_F(AlignedUint64Test, AlignedAssignmentTest) {
    n = swarm::aligned<uint64_t>(3ul);

    ASSERT_EQ(3ul, n);
    ASSERT_EQ(n, 3ul);
}

TEST_F(AlignedUint64Test, AlignedEqualityTest) {
    n = 4ul;
    swarm::aligned<uint64_t> other(4);
    ASSERT_TRUE(n == other);
    ASSERT_EQ(n, other);
}

TEST_F(AlignedUint64Test, AlignedNotEqualTest) {
    n = 4;
    swarm::aligned<uint64_t> other(3);
    ASSERT_TRUE(n != other);
    ASSERT_NE(n, other);
}

TEST_F(AlignedUint64Test, PrimitiveNotEqualTest) {
    n = 4;
    uint64_t other = 3;
    ASSERT_TRUE(n != other);
    ASSERT_NE(n, other);
}
