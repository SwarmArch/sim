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

#include <functional>
#include <gtest/gtest.h>
#include "sim/collections/ordered_set.h"

class OrderedSetIntPointerTest : public ::testing::Test {
protected:
    class IntPointerCompare {
    public:
        bool operator()(const int* p1, const int* p2) const { return *p1 < *p2; }
    };

    ordered_pointer_set<const int*, ordered_non_unique<identity<const int*>, IntPointerCompare>> set;
    int i;

    virtual void SetUp() {
        i = 3;
        set.insert(&i);
    }
};

TEST_F(OrderedSetIntPointerTest, MinAndMaxPeekOnlyIntegerElement) {
    ASSERT_FALSE(set.empty());
    ASSERT_EQ(1U, set.size());
    ASSERT_EQ(&i, set.min());
    ASSERT_EQ(3, *set.min());
    ASSERT_EQ(3, *set.max());
}

TEST_F(OrderedSetIntPointerTest, BeginReturnsIteratorToI) {
    auto it = set.cbegin();
    ASSERT_EQ(&i, *it);
    ASSERT_EQ(3, **it);
}

TEST_F(OrderedSetIntPointerTest, EraseWorksOnSingleElementQueue) {
    set.erase(set.min());
    ASSERT_TRUE(set.empty());
    ASSERT_EQ(0U, set.size());
}

TEST_F(OrderedSetIntPointerTest, PushingMinSecondUpdatesMin) {
    const int j = 1;
    set.insert(&j);
    ASSERT_EQ(&j, set.min());
    ASSERT_EQ(1, *set.min());
    ASSERT_EQ(3, *set.max());
}

TEST_F(OrderedSetIntPointerTest, PushingInReverseOrderYieldsCorrectIteration) {
    const int j = 2;
    const int k = 1;
    set.insert(&j);
    set.insert(&k);

    ASSERT_FALSE(set.empty());
    ASSERT_EQ(3U, set.size());
    ASSERT_EQ(1, *set.min());
    ASSERT_EQ(3, *set.max());

    auto it = set.cbegin();
    ASSERT_EQ(&k, *it++);
    ASSERT_EQ(&j, *it++);
    ASSERT_EQ(&i, *it);
}

TEST_F(OrderedSetIntPointerTest, ErasingMinElementPointsToNewTop) {
    const int a[] = {5,4};
    ASSERT_LT(i, a[1]);

    set.insert(&a[0]);
    set.insert(&a[1]);

    set.erase(&i);

    ASSERT_EQ(4, *set.min());
}

class OrderedSetIntPointerTest3445 : public OrderedSetIntPointerTest {
protected:
    const int a[3] = {5,4,4};

    virtual void SetUp() {
        set.insert(&a[0]);
        set.insert(&a[1]);
        set.insert(&a[2]);

        i = 3;
        set.insert(&i);
    }

};

TEST_F(OrderedSetIntPointerTest3445, EraseBasedOnPointerOnly) {
    set.erase(&a[1]);

    ASSERT_FALSE(set.empty());
    ASSERT_EQ(3U, set.size());  // We only expect one of the 4's to be erased
    ASSERT_EQ(3, *set.min());
    ASSERT_EQ(5, *set.max());

    set.erase(set.min());

    // The second 4 should be the min now
    ASSERT_EQ(&a[2], set.min());

    set.erase(set.min());

    // Finally there is only the 5 remaining
    ASSERT_EQ(&a[0], set.min());
    ASSERT_EQ(1U, set.size());
}


TEST_F(OrderedSetIntPointerTest3445, FindBasedOnPointerOnly) {
    const int missing = 4;
    auto it = set.find(&missing);

    // Even though several 4's exist in the set, the address of
    // 'missing' is not there
    ASSERT_EQ(it, set.end());
}

TEST_F(OrderedSetIntPointerTest3445, LowerBoundTest) {
    int lb = 4;
    auto it = set.lower_bound(&lb);

    ASSERT_NE(it, set.end());
    ASSERT_EQ(**(it++), 4);
    ASSERT_EQ(**(it++), 4);
    ASSERT_EQ(**(it++), 5);
}

TEST_F(OrderedSetIntPointerTest3445, UpperBoundTest) {
    int ub = 4;
    auto it = set.upper_bound(&ub);

    ASSERT_NE(it, set.end());
    ASSERT_EQ(**(it++), 5);
    ASSERT_EQ(it, set.end());

    ub = 5;
    ASSERT_EQ(set.upper_bound(&ub), set.end());
}

TEST_F(OrderedSetIntPointerTest3445, ReverseUpperBoundTest) {
    int ub = 4;
    auto it = boost::make_reverse_iterator(set.upper_bound(&ub));

    ASSERT_NE(it, set.rend());
    ASSERT_EQ(**(it++), 4);
    ASSERT_EQ(**(it++), 4);
    ASSERT_EQ(**(it++), 3);
    ASSERT_EQ(it, set.rend());

    ub = 5;
    ASSERT_EQ(boost::make_reverse_iterator(set.upper_bound(&ub)), set.rbegin());
}

TEST_F(OrderedSetIntPointerTest3445, ReverseLowerBound) {
    int lb = 4;
    auto it = boost::make_reverse_iterator(set.lower_bound(&lb));

    ASSERT_NE(it, set.rend());
    ASSERT_EQ(**(it++), 3);
    ASSERT_EQ(it, set.rend());
}
