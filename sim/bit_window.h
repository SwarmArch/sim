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

#include <deque>
#include <stdint.h>
#include "sim/assert.h"

#undef DEBUG_BWIN
#define DEBUG_BWIN(args...) //info(args)

/* dsm: This class uses sliding hierarchical bitmaps to implement scheduling in
 * a potentially large future window in O(1) with little contention and O(logN)
 * with heavy contention.
 *
 * Each sliding bitmap is a deque of 64-bit words. The first level has one bit
 * per position, and tracks occupancy at each position. schedule() takes a
 * minimum position (e.g., a minimum cycle), sets the first free position, and
 * returns that position.
 *
 * Upper-level bitmaps accelerate this search. Each bit in higher-level bitmaps
 * stores a 1 if the 64-bit word below is completely full. Without contention,
 * these bitmaps are not used or even initialized, so they add no space or time
 * overheads (in particular, note that unlike in a tree, lookups go bottom-up,
 * then top-down).
 *
 * Bitmaps use __builtin_ctzl to find unset bits in one word, so each bit
 * should take ~10 instructions to set with little contention, and up to few
 * hundred instructions if the upper-level bitmaps need to be checked. Each
 * additional level yields a factor of 64 in accelerating the search.
 *
 * Call advance() every now and then to discard elements in the window below a
 * given threshold. Note that advance() is implemented in such a way that, even
 * with a lot of levels, if the current window is small, almost as little space
 * will be used as with fewer levels.
 */
template <uint32_t N /*levels*/> class BitWindow {
  private:
    std::deque<uint64_t> queues[N];
    uint64_t baseVal;

  public:
    BitWindow() : baseVal(0) {}

    void advance(uint64_t minVal) {
        assert(minVal >= baseVal);
        // Skip small increments
        if (minVal < baseVal + 256) return;

        for (uint32_t n = 0; n < N; n++) {
            DEBUG_BWIN("  pre-advance L%d sz %ld", n, queues[n].size());
        }

        auto removeElems = [this](uint32_t n, uint64_t elems) {
            if (elems >= queues[n].size()) {
                queues[n].clear();
            } else {
                for (uint32_t i = 0; i < elems; i++) {
                    queues[n].pop_front();
                }
            }
        };

        uint64_t newBase = baseVal;
        uint64_t elemsToRemove = 0;
        for (int32_t n = N-1; n >= 0; n--) {
            uint64_t chunks = (minVal - baseVal) >> shiftFactor(n+1);
            if (!elemsToRemove) {
                if (chunks) {
                    newBase = baseVal + (chunks << shiftFactor(n+1));
                    removeElems(n, chunks);
                    elemsToRemove = chunks * 64;
                } else if (queues[n].empty()) {
                    continue;
                } else if (queues[n].size() == 1) {
                    // Special case: bitshift single word
                    elemsToRemove = (minVal - baseVal) >> shiftFactor(n);
                    // NOTE: elemsToRemove can be 0
                    DEBUG_BWIN("Shift-advance by %ld", elemsToRemove);
                    newBase = baseVal + (elemsToRemove << shiftFactor(n));
                    queues[n][0] >>= elemsToRemove;
                } else {
                    // Can't shift this level easily, keep growing
                    assert(queues[n].size() > 1);
                    break;
                }
            } else {
                removeElems(n, elemsToRemove);
                elemsToRemove *= 64;
            }
        }
        // NOTE: Once code is stable, we can get rid of newBase entirely
        DEBUG_BWIN("advance(%ld): %ld -> %ld", minVal, baseVal, newBase);
        assert(elemsToRemove == newBase - baseVal);
        baseVal = newBase;

        for (uint32_t n = 0; n < N; n++) {
            DEBUG_BWIN("  post-advance L%d sz %ld", n, queues[n].size());
        }
    }

    uint64_t schedule(uint64_t minVal) {
        assert(minVal >= baseVal);
        uint64_t offset = minVal - baseVal;
        uint64_t block = offset / 64;
        uint64_t pos = offset & 63;

        // Fastpath
#if 0  // dsm: Doen't seem to help... remove if unneeded
        if (queues[0].size() > block) {
            uint64_t bword = queues[0][block];
            if (!(bword & (1ul << pos))) {
                bword |= (1ul << pos);
                queues[0][block] = bword;
                if (unlikely(bword == -1ul)) markFull<1>(block);
                return minVal;
            }
        }
#endif
        return schedule(block, pos);
    }

  private:
    template <bool FullAllowed = true>
    inline uint64_t schedule(uint64_t block, uint64_t pos) {
        DEBUG_BWIN("schedule block %ld pos %ld sz %ld",
                   block, pos, queues[0].size());
        if (unlikely(queues[0].size() <= block)) {
            queues[0].resize(block + 1);
            queues[0][block] = 1ul << pos;
            return baseVal + block * 64  + pos;
        }

        uint64_t bword = queues[0][block];
        assert(FullAllowed || bword != -1ul);
        if (unlikely(bword == -1ul)) {
            uint32_t nextFreeBlock = findNext<1>(block);
            assert(nextFreeBlock > block);
            return schedule<false /* can't be full! */>(nextFreeBlock, 0);
        }

        // Note, the result of __builtin_ctzl is undefined for 0.
        uint64_t mod = ~(bword >> pos);
        assert(mod);
        uint32_t zeroPos = __builtin_ctzl(mod) + pos;
        if (unlikely(zeroPos >= 64)) {
            // Word not full, but segment we're interested on is
            assert(pos);  // we better have shifted something
            return schedule(block+1, 0);
        } else {
            bword |= (1ul << zeroPos);
            queues[0][block] = bword;

            if (unlikely(bword == -1ul)) markFull<1>(block);

            return baseVal + block * 64 + zeroPos;
        }
    }

    constexpr uint32_t shiftFactor(uint32_t K) const {
        return 6 * K;
    }

    template<uint32_t K> void markFull(uint64_t offset) {
        assert(K < N);  // Could be static_assert with "constexpr if" at callsite
        const uint64_t block = offset / 64;
        const uint64_t pos = offset & 63;

        if (queues[K].size() <= block) {
            queues[K].resize(block + 1);
        }

        uint64_t bword = queues[K][block];
        bword |= (1ul << pos);
        queues[K][block] = bword;
        // dsm: Need this ridiculous expression to avoid infinite recursion on
        // templates... read as markFull<K + 1>(...)
        // [victory] C++17's "constexpr if" would cleanly avoid this mess
        if (bword == -1ul && K + 1 < N) markFull<K+1<N? (K+1) : N>(block);

        DEBUG_BWIN("markFull<%d>(%ld, %ld) sz %ld bword %lx", K, block, pos,
                   queues[K].size(), queues[K][block]);
    }

    template <uint32_t K> uint64_t findNext(uint64_t offset) const {
        uint64_t block = offset / 64;
        uint64_t pos = offset & 63;

        // We can't be looking from an offset we haven't marked full first
        DEBUG_BWIN("findNext<%d>(%ld, %ld) sz %ld bword %lx", K, block, pos,
                   queues[K].size(), queues[K][block]);
        assert(K < N);  // Could be static_assert with "constexpr if" at callsite
        assert(queues[K].size() > block);
        assert(queues[K][block] & (1ul << pos));

        uint64_t bword = queues[K][block];
        if (bword == -1ul) {
            if (queues[K].size() == block+1) return (block + 1) * 64;
            if (K + 1 < N) {
                // Read as findNext<K + 1>(...)
                uint32_t newBlock = findNext<K + 1 < N ? K + 1 : N>(block);
                assert(newBlock > block);
                if (queues[K].size() == newBlock) return newBlock * 64;
                assert(queues[K].size() > newBlock);
                bword = queues[K][newBlock];
                assert(bword != -1ul);
                uint32_t zeroPos = __builtin_ctzl(~bword);
                return newBlock * 64 + zeroPos;
            }

            // Last level, iterate
            while (++block < queues[K].size()) {
                bword = queues[K][block];
                if (bword != -1ul) {
                    uint32_t zeroPos = __builtin_ctzl(~bword);
                    return block * 64 + zeroPos;
                }
            }
            // Finished iteration, all blocks full
            return block * 64;
        }

        uint64_t mod = ~(bword >> pos);
        assert(mod);
        uint32_t zeroPos = __builtin_ctzl(mod) + pos;
        if (zeroPos >= 64) {
            assert(pos);
            DEBUG_BWIN("Lateral %d sz %ld b %ld", K, queues[K].size(), block);
            if (queues[K].size() == block + 1) {
                return (block + 1) * 64;
            } else if ((queues[K][block + 1] & 1ul) == 0) {
                return (block + 1) * 64;
            } else {
                return findNext<K>((block + 1) * 64);
            }
        }
        return block * 64 + zeroPos;
    }
};
