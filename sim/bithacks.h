/** $lic$
 * Copyright (C) 2012-2021 by Massachusetts Institute of Technology
 * Copyright (C) 2010-2012 by The Board of Trustees of Stanford University
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

/* This file was adapted from zsim. */

#ifndef BITHACKS_H_
#define BITHACKS_H_

#include <math.h>
#include <stdint.h>

/* Assortment of efficient implementations for required, "bithack" operations, see the bithacks
 * website, http://graphics.stanford.edu/~seander/bithacks.html
 */

/* Max and min: These work with side-effects, are type-safe, and gcc recognizes this pattern and uses
 * conditional moves (i.e., predication --> no unpredictable branches and great preformance)
 */
#ifdef MAX
#undef MAX
#endif
#define MAX(x, y) ({ __typeof__(x) xx = (x); __typeof__(y) yy = (y); (xx > yy)? xx : yy;})
#define MAX3(x,y,z) ({ MAX(MAX(x,y),z); })

#ifdef MIN
#undef MIN
#endif
#define MIN(x, y) ({ __typeof__(x) xx = (x); __typeof__(y) yy = (y); (xx < yy)? xx : yy;})
#define MIN3(x,y,z) ({ MIN(MIN(x,y),z) })

namespace bithacks {                    // annoying but necessary to avoid std namespace conflicts

template<class T>
T log2(T v) {
    T r = 0;
    while (v >>= 1) r++; // unroll for more speed...
    return r;
}

template<class T>
T sqrt(T v) {
    // apparently faster to use std sqrt and convert than custom integer sqrt algorithm
    return (T)::sqrt((double)v);
}

}

template<typename T>
static inline T LOG2(T val) { //TODO: Use builtin
    T bits = 0;
    T tmp = val;
    while (tmp >>= 1) bits++;
    return bits;
}
// Integer log2 --- called ilog2 because cmath defines log2 for floats/doubles,
// and promotes int calls to use FP
template<typename T> static inline uint32_t ilog2(T val);
// Only specializations of unsigned types (no calling these with ints)
// __builtin_clz is undefined for 0 (internally, this uses bsr in x86-64)
template<> inline uint32_t ilog2<uint32_t>(uint32_t val) {
    return val? 31 - __builtin_clz(val) : 0;
}
template<> inline uint32_t ilog2<uint64_t>(uint64_t val) {
    return val? 63 - __builtin_clzl(val) : 0;
}

template<typename T>
static inline bool isPow2(T val) {
    return val && !(val & (val - 1));
}

/* Some variadic template magic for max/min with N args.
 *
 * Type-wise, you can compare multiple types (e.g., maxN(1, -7, 3.3)), but the
 * output type is the first arg's type (e.g., returns 3)
 */
template <typename T> static inline T maxN(T a) { return a; }
template <typename T, typename U, typename ... V> static inline T maxN(T a, U b, V... c) {
    return maxN(((a > b)? a : b), c...);
}

template <typename T> static inline T minN(T a) { return a; }
template <typename T, typename U, typename ... V> static inline T minN(T a, U b, V... c) {
    return minN(((a < b)? a : b), c...);
}


#endif  // BITHACKS_H_
