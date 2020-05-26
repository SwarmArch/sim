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
#include "sim/gvt_arbiter.h"

class GVTArbiterEpochTest : public ::testing::Test {
};


TEST_F(GVTArbiterEpochTest, PrioOf2Is1) {
    GVTArbiter::Epoch e(2, 0);
    ASSERT_EQ(1U, e.prio());
}

TEST_F(GVTArbiterEpochTest, PrioOf8Is3) {
    GVTArbiter::Epoch e(8, 0);
    ASSERT_EQ(3U, e.prio());
}

TEST_F(GVTArbiterEpochTest, PrioOf7Is0) {
    GVTArbiter::Epoch e(7, 0);
    ASSERT_EQ(0U, e.prio());
}

TEST_F(GVTArbiterEpochTest, PrioOf7IsLessThan8) {
    GVTArbiter::Epoch l(7, 0);
    GVTArbiter::Epoch r(8, 0);
    ASSERT_LT(l.prio(), r.prio());
}

TEST_F(GVTArbiterEpochTest, PrioOf8Exceeds9) {
    GVTArbiter::Epoch l(8, 0);
    GVTArbiter::Epoch r(9, 0);
    ASSERT_GT(l.prio(), r.prio());
}

TEST_F(GVTArbiterEpochTest, PrioOf796Exceeds797) {
    GVTArbiter::Epoch l(796, 0);
    GVTArbiter::Epoch r(797, 0);
    ASSERT_GT(l.prio(), r.prio());
}
