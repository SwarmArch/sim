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

#include <cassert>
#include <stdint.h>
#include <stdio.h>
#include <sstream>

#include "swarm/api.h"
#include "common.h"

// Ensure asserts are operational
#undef NDEBUG

using swarm::Timestamp;
using swarm::info;

static inline void test2(Timestamp ts, const char* v, int i) {
    info("%s %d", v, i);
}

static inline void test4(Timestamp ts, const char* v, int i, int x, int y) {
    info("%s %d %d %d", v, i, x, y);
}

static inline void test6(Timestamp ts, const char* v, int i, int x, int y, int z, int a) {
    assert(x == 1);
    assert(y == 2);
    assert(z == 3);
    assert(a == 4);
    info("%s %d %d %d %d %d", v, i, x, y, z, a);
}

static inline void testFloat(Timestamp ts, float x, double y, int a, int b) {
    info("%f %f %d %d", x, y, a, b);
}

// This is so meta...
template <int i>
static inline void testCascade(Timestamp ts) {
    swarm::info("%3d/%3ld: It's the end of the rainbow...", i, ts);
}

template <int i, typename X, typename... Args>
static inline void testCascade(Timestamp ts, X x, Args... args) {
    // stringstream calls sys_futex? hmmm
    //std::stringstream ss("");
    //ss << x;
    //swarm::info("%d/%ld: value %s", i, ts, ss.str().c_str());
    swarm::info("%3d/%3ld: value %ld", i, ts, (uint64_t)x);
    swarm::enqueue((testCascade<i - 1, Args...>), ts + 10, EnqFlags::NOHINT,
                 args...);
}

template <typename... Args>
static inline void queueCascade(Timestamp ts, Args... args) {
    swarm::enqueue((testCascade<sizeof...(Args), Args...>), ts, EnqFlags::NOHINT,
                 args...);
}


struct A {
    uint64_t x;
    double y;
    uint32_t i;
    uint32_t j;
};

struct B {
    virtual void call(uint64_t x) {
        info("xss %ld", x);
    }
};

static inline void testStruct(Timestamp ts, A a) {
    info("%ld %f %d %d", a.x, a.y, a.i, a.j);
}

int main(int argc, const char** argv) {
    swarm::enqueue(test2, 3ul, 0ul, "hello world", 4);
    swarm::enqueue(test2, 3ul, 0ul, argv[0], argc);
    swarm::enqueue(test4, 3ul, 0ul, argv[0], argc, 0, 7);
    swarm::enqueue(test6, 3ul, 0ul, argv[0], argc, 1, 2, 3, 4);
    swarm::enqueue(testFloat, 3ul, 0ul, 3.14159f, 7.5, 0, 7);
    swarm::enqueue(testStruct, 3ul, 0ul, A{32, 3.141594, 40, 47});

    auto testLambda = [](Timestamp ts) {info("this is a lambda %ld", ts + 42);};
    swarm::enqueueLambda(testLambda, 3ul, 0ul);

    auto testLambda2 = [](Timestamp ts, uint32_t x) {info("argLambda %ld", ts + 42 + x);};
    swarm::enqueueLambda(testLambda2, 3ul, 0ul, 275);

    swarm::enqueue(test2, 3ul, EnqFlags::NOHINT, "nohint, deal with it", 0);

    B b;
    b.call(0);
    swarm::enqueueLambda([argc,&b](Timestamp ts) { b.call(ts+argc); }, 3ul, 0ul);

    queueCascade(10,1,2,3ul,4,5,6,7,8,9,10,11,12,13,14,15ul,16,17u,18,19,20);

    swarm::run();

    // Print the verification string so that our test scripts don't complain
    tests::assert_true(true);
    return 0;
}
