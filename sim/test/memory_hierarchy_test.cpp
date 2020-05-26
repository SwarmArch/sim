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
#include "sim/memory/memory_hierarchy.h"
#include "sim/sim.h" // ossinfo needed by the messages
#include "sim/timestamp.h"


template <typename T>
class MemoryHierarchyRequestTest : public ::testing::Test {
  protected:
    MemoryHierarchyRequestTest() {
        ::ossinfo = new GlobSimInfo();
        const_cast<GlobSimInfo*>(::ossinfo)->lineSize = 0;
        const_cast<GlobSimInfo*>(::ossinfo)->lineBits = 0;
    }
};


// Factory functions
template<typename T>
T CreateTimestampedMessage();

template<typename T>
T CreateUnsetTimestampedMessage();

template<typename T>
T CreateUntimestampedMessage();

template<typename T>
T CreateIrrevocablyTimestampedMessage();


constexpr uint32_t DONTCARE = 0xacc355;
TimeStamp NONZERO_TS(7,11);
TimeStamp UNSET_TS(4);


template<>
MemReq CreateTimestampedMessage<MemReq>() {
    MemReq r = {DONTCARE, GETS, DONTCARE, MESIState::I,
                DONTCARE, 0, &NONZERO_TS, nullptr};
    return r;
}

template<>
MemReq CreateUnsetTimestampedMessage<MemReq>() {
    MemReq r = {DONTCARE, GETS, DONTCARE, MESIState::I, DONTCARE, 0, &UNSET_TS, nullptr};
    return r;
}

template<>
MemReq CreateUntimestampedMessage<MemReq>() {
    MemReq r = {DONTCARE, GETS, DONTCARE, MESIState::I, DONTCARE, 0, nullptr, nullptr};
    return r;
}

template<>
MemReq CreateIrrevocablyTimestampedMessage<MemReq>() {
    MemReq r = {DONTCARE, GETS, DONTCARE, MESIState::I,
                DONTCARE, 0, &IRREVOCABLE_TS, nullptr};
    return r;
}

template<>
InvReq CreateTimestampedMessage<InvReq>() {
    InvReq r = {DONTCARE, INV, DONTCARE, &NONZERO_TS, DONTCARE};
    return r;
}

template<>
InvReq CreateUnsetTimestampedMessage<InvReq>() {
    InvReq r = {DONTCARE, INV, DONTCARE, &UNSET_TS, DONTCARE};
    return r;
}

template<>
InvReq CreateUntimestampedMessage<InvReq>() {
    InvReq r = {DONTCARE, INV, DONTCARE, nullptr, DONTCARE};
    return r;
}

template<>
InvReq CreateIrrevocablyTimestampedMessage<InvReq>() {
    InvReq r = {DONTCARE, INV, DONTCARE, &IRREVOCABLE_TS, DONTCARE};
    return r;
}

using Implementations = ::testing::Types<MemReq, InvReq>;
TYPED_TEST_SUITE(MemoryHierarchyRequestTest, Implementations);

TYPED_TEST(MemoryHierarchyRequestTest, TimestampedMessageIs24Bytes) {
    TypeParam r = CreateTimestampedMessage<TypeParam>();
    ASSERT_EQ(16U + 8U, r.bytes());
}

TYPED_TEST(MemoryHierarchyRequestTest, UnsetTimestampedMessageIs16Bytes) {
    TypeParam r = CreateUnsetTimestampedMessage<TypeParam>();
    ASSERT_EQ(8U + 8U, r.bytes());
}

TYPED_TEST(MemoryHierarchyRequestTest, UntimestampedMessageIs8Bytes) {
    TypeParam r = CreateUntimestampedMessage<TypeParam>();
    ASSERT_EQ(8U, r.bytes());
}

TYPED_TEST(MemoryHierarchyRequestTest, IrrevocablyTimestampedMessageIs8Bytes) {
    TypeParam r = CreateIrrevocablyTimestampedMessage<TypeParam>();
    ASSERT_EQ(8U, r.bytes());
}


// Mock the sim.h global variable
const GlobSimInfo* ossinfo;
