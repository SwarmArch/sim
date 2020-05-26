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
#include "sim/timestamp.h"

class TimeStampTest : public ::testing::Test {
protected:
    const TimeStamp left;
    const TimeStamp feed;

    TimeStampTest() : left(5, 10), feed(0xfeed, 0xdeadbeef) {}
};

void AllGTAssertions(const TimeStamp & left, const TimeStamp & right) {
    ASSERT_GT(left, right);
    ASSERT_LT(right, left);
    ASSERT_GE(left, right);
    ASSERT_LE(right, left);
    ASSERT_FALSE(left < right);
    ASSERT_NE(left, right);
    ASSERT_NE(right, left);
}

void AllLTAssertions(const TimeStamp & left, const TimeStamp & right) {
    ASSERT_LT(left, right);
    ASSERT_GT(right, left);
    ASSERT_LE(left, right);
    ASSERT_GE(right, left);
    ASSERT_FALSE(left > right);
    ASSERT_NE(left, right);
    ASSERT_NE(right, left);
}

void AllEQAssertions(const TimeStamp & left, const TimeStamp & right) {
    ASSERT_FALSE(left < right);
    ASSERT_FALSE(left > right);
    ASSERT_FALSE(left != right);
    ASSERT_EQ(left, right);
    ASSERT_EQ(right, left);
    ASSERT_LE(left, right);
    ASSERT_LE(right, left);
    ASSERT_GE(left, right);
    ASSERT_GE(right, left);
}


TEST_F(TimeStampTest, GTTest) {
    TimeStamp right(3, 11);
    AllGTAssertions(left, right);
}

TEST_F(TimeStampTest, LTTest) {
    TimeStamp right(7, 9);
    AllLTAssertions(left, right);
}

TEST_F(TimeStampTest, LETest) {
    TimeStamp right(5, 11);
    AllLTAssertions(left, right);
}

TEST_F(TimeStampTest, GETest) {
    TimeStamp right(5, 9);
    AllGTAssertions(left, right);
}

TEST_F(TimeStampTest, EQTest) {
    TimeStamp right = left;
    AllEQAssertions(left, right);
}

TEST_F(TimeStampTest, UnsetTieBreakerEQTest) {
    TimeStamp unsetLeft(5);
    TimeStamp unsetRight = unsetLeft;
    AllEQAssertions(unsetLeft, unsetRight);
}

TEST_F(TimeStampTest, MixedTieBreakerNETest) {
    TimeStamp unset(5);
    // New assumption: if a timestamp is unset, its tiebreaker will be higher
    // than all tiebreakers in the system. This seems easy to screw up, I know.
    AllLTAssertions(left, unset);
}

TEST_F(TimeStampTest, UnsetLTTest) {
    TimeStamp unsetRight(6);
    AllLTAssertions(left, unsetRight);
}

TEST_F(TimeStampTest, UnsetGTTest) {
    TimeStamp unsetRight(4);
    AllGTAssertions(left, unsetRight);
}

TEST_F(TimeStampTest, LazyTieBreakerAssignment) {
    TimeStamp right(5);
    right.assignTieBreaker(10);
    AllEQAssertions(left, right);
}

TEST_F(TimeStampTest, ReturnTheAppTimeStampToRequester) {
    ASSERT_EQ(0xfeedUL, feed.app());
    ASSERT_NE(0xdeadbeefUL, feed.app());
}

TEST_F(TimeStampTest, CantChangeTieBreakerOfInfinity) {
    TimeStamp inf = INFINITY_TS;
    inf.assignTieBreaker(3);
    AllEQAssertions(INFINITY_TS, inf);
}

TEST_F(TimeStampTest, TestOStreamOperator) {
    std::ostringstream buffer;
    std::string expected = "5:10";

    buffer << left;

    ASSERT_EQ(expected, buffer.str());
    ASSERT_EQ(expected, left.toString());
}

TEST_F(TimeStampTest, FractalDeepEquality) {
    TimeStamp deep = left;
    deep.deepen();
    AllEQAssertions(left, deep);
}

TEST_F(TimeStampTest, FractalRootIsLessThanMultiDomainDescendant) {
    TimeStamp shallow = left;
    shallow.deepen();
    TimeStamp deep = shallow.childTS(0);
    deep.assignTieBreaker(42);
    deep.deepen();
    deep = deep.childTS(0);
    deep.assignTieBreaker(43);
    deep.deepen();
    AllLTAssertions(shallow, deep);
}

TEST_F(TimeStampTest, FractalRootIsLessThanDomainChild) {
    TimeStamp aux = left;
    aux.deepen();
    TimeStamp child = aux.childTS(0);
    // 5:10 < (5:10, 0:INF)
    AllLTAssertions(left, child);
    child.assignTieBreaker(150);
    // 5:10 < (5:10, 0:150)
    AllLTAssertions(left, child);
}

TEST_F(TimeStampTest, FractalDeepTSPrecedesShallowTS) {
    TimeStamp aux(left.app(), left.tiebreaker()-1);
    aux.deepen();
    TimeStamp right = aux.childTS(1);
    right.assignTieBreaker(250);
    // 5:10 > (5:9, 1:250)
    AllGTAssertions(left, right);
}

TEST_F(TimeStampTest, FractalSameDepthDifferentRootComparison) {
    TimeStamp aux1(5, 13);
    TimeStamp aux2(6, 7);
    aux1.deepen();
    aux2.deepen();
    TimeStamp child1 = aux1.childTS(3);
    TimeStamp child2 = aux2.childTS(1);
    // (5:13, 3:INF) < (6:7, 1:INF)
    AllLTAssertions(aux1, aux2);
}

TEST_F(TimeStampTest, FractalCompareDifferentDomainPtrsButEqualDomainValue) {
    TimeStamp deepenedRoot = left;
    deepenedRoot.deepen();
    TimeStamp child = left;
    child.deepen();
    child = child.childTS(4);
    // (5:10,0:0) < (5:10,4:INF)
    //        ^             ^
    // But these have different domain pointers, even though the domains are
    // equal (5:10)
    // [mcj] I'm not sure if we should need to handle this case, but when
    // running msf-fg at 256 cores, this issue arose and triggered an assertion.
    AllLTAssertions(deepenedRoot, child);
}

TEST_F(TimeStampTest,
        FractalCompareDiffDomainPtrsButEqualDomainValueWithNon0CompsForBoth) {
    TimeStamp child1 = left;
    child1.deepen();
    child1 = child1.childTS(1);
    TimeStamp child2 = left;
    child2.deepen();
    child2 = child2.childTS(4);
    // (5:10,1:INF) < (5:10,4:INF)
    //        ^             ^
    // These have different domain pointers, even though the domains are equal
    // (5:10)
    AllLTAssertions(child1, child2);
}

TEST_F(TimeStampTest, TestTimeStampAtLevel) {
    TimeStamp parent = left;
    parent.deepen();
    TimeStamp child1 = parent.childTS(0);
    TimeStamp levelTSsub = parent.getTimeStampAtLevel(1);
    ASSERT_EQ(child1, levelTSsub);

    TimeStamp parent2 = child1;
    parent2.assignTieBreaker(20);
    parent2.deepen();
    TimeStamp child2 = parent2.childTS(0);
    TimeStamp levelTS0 = parent2.getTimeStampAtLevel(0);
    TimeStamp levelTS1 = parent2.getTimeStampAtLevel(1);
    TimeStamp levelTS2 = parent2.getTimeStampAtLevel(2);
    levelTS0.assignTieBreaker(10);  // same as parent's tie-breaker
    ASSERT_EQ(parent, levelTS0);
    ASSERT_EQ(child1, levelTS1);
    ASSERT_EQ(child2, levelTS2);
}

