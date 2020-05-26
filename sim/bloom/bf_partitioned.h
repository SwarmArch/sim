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
/*****************************************************************************
 * Bloom Filter class with
 * - partitioned bit array
 * - using templated hash functions
 ****************************************************************************/
#include <boost/dynamic_bitset.hpp>
#include <cstdint>

#include "sim/assert.h"
#include "sim/bithacks.h"
#include "sim/memory/hash.h"

namespace bloom {

using hash_t = uint32_t;

template <typename T>
class BloomFilterPartitioned {
    const uint32_t k_;
    const uint32_t shift_;
    const hash_t mask_;

    // Hopefully faster than std::vector<bool>
    boost::dynamic_bitset<uint64_t> bits_;

    const H3HashFamily& hash_;

  public:
    BloomFilterPartitioned(uint32_t nbits, uint32_t k, const H3HashFamily& h)
      : k_(k),
        shift_(ilog2(nbits / k)),
        mask_((nbits / k) - 1),
        bits_(nbits, 0ul),
        hash_(h)
    {
        assert(nbits % k == 0);
        assert(isPow2(nbits / k));
        assert((1u << shift_) == (mask_ + 1));
    }

    // Constructor for unit tests
    // FIXME(mcj) the destructor should free the memory
    BloomFilterPartitioned(uint32_t nbits, uint32_t k)
        : BloomFilterPartitioned(
                nbits, k, *(new H3HashFamily(k, ilog2(nbits / k), 0x1010101)))
    {}

    virtual ~BloomFilterPartitioned() = default;

    inline void insert(T val) {
        // Set one bit in each partition of word_filter
        for (uint32_t i = 0; i < k_; i++) {
            bits_.set(index(i, val));
        }
    }

    inline bool contains(T val) const {
        for (uint32_t i = 0; i < k_; i++) {
            if (!bits_.test(index(i, val))) return false;
        }
        return true;
    }

    void clear() {
        bits_.reset();
    }

    bool empty() const {
        return bits_.none();
    }

  protected:
    // [mcj] Risky; this assumes the length of the hashes array is k_
    bool containsHashes(const hash_t* hashes) const {
        for (uint32_t i = 0; i < k_; i++) {
            if (!bits_.test(indexFromHash(i, hashes[i]))) return false;
        }
        return true;
    }

  private:
    hash_t index(uint32_t id, T val) const {
        // Bound the output of each hash fcn to range [0, BITS/K), since
        // there are K partitions of BITS
        hash_t index = hash_.hash(id, val) & mask_;
        hash_t offset = id << shift_;
        return index + offset;
    }

    hash_t indexFromHash(uint32_t id, hash_t h) const {
        hash_t index = h & mask_;
        hash_t offset = id << shift_;
        return index + offset;
    }
};


}  // end namespace bloom
