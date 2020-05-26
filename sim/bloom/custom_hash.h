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

#include <cstdint>

class Custom64To32Hash {
public:
    Custom64To32Hash() {}

    __attribute__((always_inline))
    uint32_t operator()(uint64_t key) const {
        return hashint(fullAvalanche(hash6432shift(key)));
    }

private:
    // Source: https://gist.github.com/badboy/6267743
    uint32_t hash6432shift(uint64_t key) const {
      key = (~key) + (key << 18); // key = (key << 18) - key - 1;
      key = key ^ (key >> 31);
      key = key * 21; // key = (key + (key << 2)) + (key << 4);
      key = key ^ (key >> 11);
      key = key + (key << 6);
      key = key ^ (key >> 22);
      return (uint32_t) key;
    }

    // Source: http://burtleburtle.net/bob/hash/integer.html
    uint32_t fullAvalanche(uint32_t a) const {
        a = (a+0x7ed55d16) + (a<<12);
        a = (a^0xc761c23c) ^ (a>>19);
        a = (a+0x165667b1) + (a<<5);
        a = (a+0xd3a2646c) ^ (a<<9);
        a = (a+0xfd7046c5) + (a<<3);
        a = (a^0xb55a4f09) ^ (a>>16);
        return a;
    }

    uint32_t hashint(uint32_t a) const {
        a -= (a<<6);
        a ^= (a>>17);
        a -= (a<<9);
        a ^= (a<<4);
        a -= (a<<3);
        a ^= (a<<10);
        a ^= (a>>15);
        return a;
    }
};
