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

#pragma once

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>
#include <numeric>

namespace tests {

/**
 * This interface borrows from googletest's, albeit it has poor handling if
 * these methods are invoked multiple times in the same test. For example,
 *   assert_true(true);
 *   assert_true(false);
 * would print out
 *   Verify OK
 *   Verify Incorrect
 * which is confusing. Scripts that read the output should account for this.
 */

static inline void assert_true(bool verify) {
    std::cout << "Verify: " << (verify ? "OK" : "Incorrect") << std::endl;
    if (!verify) std::abort();
}

template <typename T>
static inline void __assert_eq(T expected, T actual, long /*dontcare*/) {
    bool verify = expected == actual;
    if (!verify) {
        std::cout << "Expected " << expected
                  << " actual " << actual << std::endl;
    }
    tests::assert_true(verify);
}

template <typename C>
static inline std::string __collectionToString(const C& c) {
    using It = decltype(std::begin(c));
    using T = typename std::iterator_traits<It>::value_type;
    // From http://en.cppreference.com/w/cpp/algorithm/accumulate
    std::string str = std::accumulate(
            std::begin(c), std::end(c), std::string{},
            [](const std::string& a, const T& t) {
                return a.empty() ? std::to_string(t)
                       : a + ", " + std::to_string(t);
            });
    return str;
}

template <typename C>
static inline auto __assert_eq(C expected, C actual, int /*dontcare*/)
        -> decltype(std::begin(actual)) {
    bool verify = std::equal(std::begin(expected), std::end(expected),
                             std::begin(actual));
    if (!verify) {
        std::cout << "expected " << __collectionToString(expected) << std::endl
                  << "actual   " << __collectionToString(actual) << std::endl;
    }
    tests::assert_true(verify);
    // Useless return value for SFINAE
    return std::begin(actual);
}

// Use SFINAE https://stackoverflow.com/a/9154394
template <typename T>
static inline void assert_eq(T expected, T actual) {
    // Prefer the collections one if possible.
    __assert_eq(expected, actual, 0);
}


}
