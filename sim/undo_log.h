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

#include <array>
#include <boost/intrusive/slist.hpp>
#include <boost/container/static_vector.hpp>
#include <stdint.h>
#include "sim/memory/memory_hierarchy.h"
#include "sim/memory/constants.h"
#include "sim/pad.h"
#include "sim/log.h"

/* Undo logs use chunked LIFO linked lists with a carefully controlled memory
 * layout. Because all memory accesses are performed inside the simulator and
 * we do not want to expose undo-log memory to speculative accesses, they use
 * simulator memory.
 */

class UndoLog {
  public:
    struct Entry {
        const Address addr;
        const uint64_t value;
        Entry(Address a, uint64_t v) : addr(a), value(v) {}
    };

  private:
    // Carefully sized to be an *odd* whole number of cache lines, to spread
    // consecutive chunks across cache sets to avoid conflict misses.
    static constexpr size_t CHUNK_SIZE = 1024 - 64;
    static constexpr size_t CHUNK_ELEMS = (CHUNK_SIZE - 16) / sizeof(Entry);
    struct Chunk : public boost::intrusive::slist_base_hook<> {
        boost::container::static_vector<Entry, CHUNK_ELEMS> elems;
        inline bool full() const { return elems.size() == elems.capacity(); }
    };
    static_assert(sizeof(Chunk) == CHUNK_SIZE, "");

    typedef boost::intrusive::slist<Chunk> ChunkList;

    static std::array<ChunkList, MAX_THREADS> freeLists;

    ChunkList chunks;
    uint32_t cid;  // required to grab from the right freelist

  public:
    UndoLog() : cid(-1u) {}

    inline bool empty() const { return chunks.empty(); }

    inline void init(uint32_t c) {
        assert(cid == -1u);
        assert(empty());
        cid = c;
        assert(cid < MAX_THREADS);
    }

    Address push(Address addr, uint64_t val) {
        if (chunks.empty() || chunks.front().full()) grow();
        chunks.front().elems.emplace_back(addr, val);
        return (uintptr_t) &chunks.front().elems.back();
    }

    Entry pop() {
        assert(!empty() && !chunks.front().elems.empty());
        Entry e = chunks.front().elems.back();
        chunks.front().elems.pop_back();
        if (chunks.front().elems.empty()) shrink();
        return e;
    }

    void clear() {
        // If any, return chunks in LIFO order
        if (!chunks.empty()) {
            if (!freeLists[cid].empty())
                chunks.splice(chunks.end(), freeLists[cid]);
            freeLists[cid].swap(chunks);
            assert(!freeLists[cid].empty());
        }
        assert(chunks.empty());
        cid = -1u;
    }

  private:
    void grow() {
        assert(cid < MAX_THREADS);
        Chunk* ck;
        if (freeLists[cid].empty()) {
            ck = new Chunk();
            // line alignment is crucial to avoid false sharing!
            assert(!(reinterpret_cast<uintptr_t>(ck) & (CACHE_LINE_BYTES - 1)));
        } else {
            ck = &freeLists[cid].front();
            freeLists[cid].pop_front();
        }
        // NOTE: Because we use splice() in clear(), we must clear chunks
        // lazily (splice is O(1) and does not traverse chunk lists)
        ck->elems.clear();
        chunks.push_front(*ck);
    }

    void shrink() {
        assert(cid < MAX_THREADS);
        Chunk& ck = chunks.front();
        chunks.pop_front();
        freeLists[cid].push_front(ck);
    }
};
