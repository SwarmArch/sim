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
#include <map>
#include "sim/assert.h"
#include "sim/bithacks.h"

template <typename T>
class PrioQueueElem {
  private:
    // Managed by PrioQueue
    T* next;
    bool queued;

  public:
    PrioQueueElem() : next(nullptr), queued(false) {}
    inline uint64_t key() const;
    inline bool isQueued() const { return queued; }
    template <typename, uint32_t> friend class PrioQueue;
};

template <typename T, uint32_t B>
class PrioQueue {
    struct PQBlock {
        struct List {
            // For FIFO ordering among unordered elements,
            // dequeue from the head, enqueue to the tail
            T* head;
            T* tail;
        };
        List array[64];
        uint64_t occ;  // bit i is 1 if array[i] is populated

        PQBlock() {
            std::fill(array, array + 64, List({nullptr, nullptr}));
            occ = 0;
        }

        inline uint32_t firstPos() const {
            assert(occ);
            return __builtin_ctzl(occ);
        }

        inline void enqueue(T* obj, uint32_t pos) {
            occ |= 1L << pos;
            assert(!obj->next);
            // Append to the end of the list
            List& list = array[pos];
            assert((list.head != nullptr) ^ (list.tail == nullptr));
            if (list.tail) list.tail->next = obj;
            list.tail = obj;
            if (!list.head) list.head = obj;
        }

        inline bool erase(T* elem, uint32_t pos) {
            List& list = array[pos];
            if (!list.head) {
                assert(!list.tail);
                return false;
            } else if (list.head == elem) {
                list.head = elem->next;
                elem->next = nullptr;
                if (!list.head) {
                    occ ^= 1L << pos;
                    list.tail = nullptr;
                }
                return true;
            } else {
                T* cur = list.head;
                while (cur->next) {
                    if (cur->next == elem) {
                        cur->next = elem->next;
                        elem->next = nullptr;
                        if (cur->next == nullptr) list.tail = cur;
                        return true;
                    }
                    cur = cur->next;
                }
                return false;
            }
        }
    };

    PQBlock blocks[B];

    typedef std::multimap<uint64_t, T*> FEMap;  // far element map
    typedef typename FEMap::iterator FEMapIterator;

    FEMap feMap;

    uint64_t curBlock;
    uint64_t elems;

    // Caches first element; always valid, nullptr when empty
    T* curTop;

  public:
    PrioQueue() : curBlock(0), elems(0), curTop(nullptr) {}

    void push(T* obj) {
        assert(!obj->queued);
        uint64_t key = obj->key();
        uint64_t absBlock = key / 64;
        assert(absBlock >= curBlock);

        if (absBlock < curBlock + B) {
            uint32_t i = absBlock % B;
            uint32_t offset = key % 64;
            blocks[i].enqueue(obj, offset);
        } else {
            feMap.insert(std::pair<uint64_t, T*>(key, obj));
        }
        elems++;
        obj->queued = true;

        if (!curTop || obj->key() < curTop->key()) curTop = obj;
    }

    void erase(T* obj) {
        assert(obj->queued);
        uint64_t key = obj->key();
        uint64_t absBlock = key / 64;
        assert(absBlock >= curBlock);

        bool erased = false;
        if (absBlock < curBlock + B) {
            uint32_t i = absBlock % B;
            uint32_t offset = key % 64;
            erased = blocks[i].erase(obj, offset);
        }

        if (!erased) {
            // Element MUST be in far element map
            auto it = feMap.find(key);
            while (it->second != obj && it->first == key) ++it;
            assert(it->second == obj && it->first == key);
            feMap.erase(it);
        }
        elems--;
        obj->queued = false;

        if (obj == curTop) curTop = nextTop(curTop->key());
    }

    T* pop(uint64_t limitKey = -1ul) {
        assert(elems);
        advance(limitKey);
        T* obj = curTop;
        erase(obj);
        return obj;
    }

    T* top() {
        assert(elems);
        assert(curTop);
        return curTop;
    }

    inline uint64_t size() const { return elems; }

    inline bool empty() const { return elems == 0; }

    void reset() {
        assert(empty());
        curBlock = 0;
    }

  private:
    inline T* nextTop(uint64_t minKey) const {
        if (!elems) return nullptr;
        uint64_t minBlock = minKey / 64;
        assert(minBlock >= curBlock);
        for (uint32_t i = minBlock - curBlock; i < B / 2; i++) {
            uint32_t idx = (curBlock + i) % B;
            if (blocks[idx].occ) {
                return blocks[idx].array[blocks[idx].firstPos()].head;
            }
        }
        // Beyond B/2 blocks, there may be a far element that comes earlier
        for (uint32_t i = MAX(minBlock - curBlock, B / 2); i < B; i++) {
            uint32_t idx = (curBlock + i) % B;
            if (blocks[idx].occ) {
                uint64_t pos = blocks[idx].firstPos();
                uint64_t key = (curBlock + i) * 64 + pos;
                if (!feMap.empty()) {
                    T* firstFarElem = feMap.begin()->second;
                    if (firstFarElem->key() < key) return firstFarElem;
                }
                return blocks[idx].array[pos].head;
            }
        }

        return feMap.begin()->second;
    }

    void advance(uint64_t limitKey) {
        assert(elems);
        uint64_t limitBlock = limitKey / 64;
        while (!blocks[curBlock % B].occ && curBlock < limitBlock) {
            curBlock++;
            if ((curBlock % (B / 2)) == 0 && !feMap.empty()) {
                uint64_t topKey = (curBlock + B) * 64;
                // Move every element with key < topKey to blocks[]
                FEMapIterator it = feMap.begin();
                while (it != feMap.end() && it->first < topKey) {
                    uint64_t key = it->first;
                    T* obj = it->second;

                    uint64_t absBlock = key / 64;
                    assert(absBlock >= curBlock);
                    assert(absBlock < curBlock + B);
                    uint32_t i = absBlock % B;
                    uint32_t offset = key % 64;
                    blocks[i].enqueue(obj, offset);
                    it++;
                }
                feMap.erase(feMap.begin(), it);
            }
        }
    }
};
