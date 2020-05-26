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
#include "sim/conflicts/precise_address_accessors.h"
#include "sim/conflicts/bloom_address_accessors.h"

template <typename T>
AddressAccessorTable* CreateTable();

template <>
AddressAccessorTable* CreateTable<PreciseAddressAccessorTable>() {
    return new PreciseAddressAccessorTable();
}

template <>
AddressAccessorTable* CreateTable<BloomAddressAccessorTable>() {
    return new BloomAddressAccessorTable(4096, 8);
}

template <typename T>
class AddressAccessorsTest : public ::testing::Test {
  protected:
    AddressAccessorsTest() : table(CreateTable<T>()),
                             set(table->createAddressSet()) {}
    ~AddressAccessorsTest() {
        delete table;
        delete set;
    }

    AddressAccessorTable* table;
    AddressAccessorTable::AddressSet* set;

    const Address addr = 0xfeed;
    // Fake address, doesn't matter
    const Task* task = reinterpret_cast<const Task*>(0xfa4e7a54);
};

// Type-parameterized test
// https://github.com/google/googletest/blob/master/googletest/docs/AdvancedGuide.md#type-parameterized-tests
TYPED_TEST_SUITE_P(AddressAccessorsTest);


TYPED_TEST_P(AddressAccessorsTest, EmptyTableIsEmpty) {
    auto actual = this->table->allAccessors(this->addr);
    ASSERT_EQ(Accessors(), actual);
    ASSERT_FALSE(this->table->anyAccessors(this->addr));
    ASSERT_FALSE(this->table->anyAddresses(this->task));
}

TYPED_TEST_P(AddressAccessorsTest, TaskAddressPairIsPresent) {
    this->table->update(this->task, this->addr);
    auto actual = this->table->allAccessors(this->addr);

    Accessors expected({this->task});
    ASSERT_EQ(expected, actual);
    ASSERT_TRUE(this->table->anyAccessors(this->addr));
    ASSERT_TRUE(this->table->anyAddresses(this->task));
    ASSERT_TRUE(this->table->test(this->task, this->addr));
}

// [mcj] Disable as it's not implemented for Bloom filters yet
TYPED_TEST_P(AddressAccessorsTest,
       DISABLED_InsertedAddressSetFindsAccessorOfAll) {
    std::vector<Address> addresses = {3,4,5,10,3,0xfffff};
    for (Address a : addresses) this->set->insert(a);

    this->table->insert(this->task, *this->set);

    Accessors expected({this->task});
    for (Address a : addresses) {
        const Accessors& actual = this->table->allAccessors(a);
        ASSERT_EQ(expected, actual);
        ASSERT_TRUE(this->table->anyAccessors(a));
        ASSERT_TRUE(this->table->anyAddresses(this->task));
        ASSERT_TRUE(this->table->test(this->task, a));
    }
}

TYPED_TEST_P(AddressAccessorsTest,
        TwoTasksWithSameAddressYieldBothInAccessors) {
    const Task* task2 = reinterpret_cast<const Task*>(0xffffff);
    this->table->update(this->task, this->addr);
    this->table->update(task2, this->addr);
    auto actual = this->table->allAccessors(this->addr);

    Accessors expected({this->task, task2});
    ASSERT_EQ(expected, actual);
    ASSERT_TRUE(this->table->anyAccessors(this->addr));
    ASSERT_TRUE(this->table->anyAddresses(task2));
    ASSERT_TRUE(this->table->test(task2, this->addr));
}

TYPED_TEST_P(AddressAccessorsTest, ClearATaskSoItIsNoLongerAnAccessor) {
    const Task* task2 = reinterpret_cast<const Task*>(0xffffff);
    this->table->update(this->task, this->addr);
    this->table->update(task2, this->addr);
    this->table->clear(this->task);
    this->table->clear(task2);
    auto actual = this->table->allAccessors(this->addr);

    ASSERT_EQ(Accessors(), actual);
    ASSERT_FALSE(this->table->anyAccessors(this->addr));
    ASSERT_FALSE(this->table->anyAddresses(this->task));
    ASSERT_FALSE(this->table->anyAddresses(task2));
    ASSERT_FALSE(this->table->test(this->task, this->addr));
    ASSERT_FALSE(this->table->test(task2, this->addr));
}

REGISTER_TYPED_TEST_SUITE_P(
        AddressAccessorsTest,
        EmptyTableIsEmpty,
        TaskAddressPairIsPresent,
        DISABLED_InsertedAddressSetFindsAccessorOfAll,
        TwoTasksWithSameAddressYieldBothInAccessors,
        ClearATaskSoItIsNoLongerAnAccessor);

using MyTypes = ::testing::Types<PreciseAddressAccessorTable,
                                 BloomAddressAccessorTable>;
INSTANTIATE_TYPED_TEST_SUITE_P(
        PreciseAndBloom,
        AddressAccessorsTest,
        MyTypes);
