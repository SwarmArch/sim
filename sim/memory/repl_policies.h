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

#ifndef REPL_POLICIES_H_
#define REPL_POLICIES_H_

#include <functional>
#include <random>
#include <string.h>
#include <vector>
#include "sim/bithacks.h"
#include "sim/memory/cache_arrays.h"
#include "sim/memory/coherence_ctrls.h"
#include "sim/memory/memory_hierarchy.h"
#include "sim/sim.h"

using std::vector;

/* Generic replacement policy interface. A replacement policy is initialized by the cache (by calling setTop/BottomCC) and used by the cache array. Usage follows two models:
 * - On lookups, update() is called if the replacement policy is to be updated on a hit
 * - On each replacement, rank() is called with the req and a list of replacement candidates.
 * - When the replacement is done, replaced() is called. (See below for more detail.)
 */
class ReplPolicy  {
    protected:
        CC* cc; //coherence controller, used to figure out whether candidates are valid or number of sharers

    public:
        ReplPolicy() : cc(nullptr) {}

        virtual void setCC(CC* _cc) {cc = _cc;}

        virtual void update(uint32_t id, const MemReq* req) = 0;
        virtual void replaced(uint32_t id) = 0;

        virtual uint32_t rankCands(const MemReq* req, SetAssocCands cands) = 0;
        virtual uint32_t rankCands(const MemReq* req, ZCands cands) = 0;

        virtual void initStats(AggregateStat* parent) {}
};

/* Add DECL_RANK_BINDINGS to each class that implements the new interface,
 * then implement a single, templated rank() function (see below for examples)
 * This way, we achieve a simple, single interface that is specialized transparently to each type of array
 * (this code is performance-critical)
 */
#define DECL_RANK_BINDING(T) uint32_t rankCands(const MemReq* req, T cands) override { return rank(req, cands); }
#define DECL_RANK_BINDINGS DECL_RANK_BINDING(SetAssocCands); DECL_RANK_BINDING(ZCands);

/* Legacy support.
 * - On each replacement, the controller first calls startReplacement(), indicating the line that will be inserted;
 *   then it calls recordCandidate() for each candidate it finds; finally, it calls getBestCandidate() to get the
 *   line chosen for eviction. When the replacement is done, replaced() is called. The division of getBestCandidate()
 *   and replaced() happens because the former is called in preinsert(), and the latter in postinsert(). Note how the
 *   same restrictions on concurrent insertions extend to this class, i.e. startReplacement()/recordCandidate()/
 *   getBestCandidate() will be atomic, but there may be intervening update() calls between getBestCandidate() and
 *   replaced().
 */
class LegacyReplPolicy : public virtual ReplPolicy {
    protected:
        virtual void startReplacement(const MemReq* req) {} //many policies don't need it
        virtual void recordCandidate(uint32_t id) = 0;
        virtual uint32_t getBestCandidate() = 0;

    public:
        template <typename C> inline uint32_t rank(const MemReq* req, C cands) {
            startReplacement(req);
            for (auto ci = cands.begin(); ci != cands.end(); ci.inc()) {
                recordCandidate(*ci);
            }
            return getBestCandidate();
        }

        DECL_RANK_BINDINGS;
};

/* Plain ol' LRU, though this one is sharers-aware, prioritizing lines that have
 * sharers down in the hierarchy vs lines not shared by anyone.
 */
template <bool sharersAware>
class LRUReplPolicy : public ReplPolicy {
    protected:
        uint64_t timestamp; // incremented on each access
        uint64_t* array;
        uint32_t numLines;

    public:
        explicit LRUReplPolicy(uint32_t _numLines) : timestamp(1), numLines(_numLines) {
            array = new uint64_t[numLines] ();
        }

        ~LRUReplPolicy() {
            delete [] array;
        }

        void update(uint32_t id, const MemReq* req) override {
            array[id] = timestamp++;
        }

        void replaced(uint32_t id) override {
            array[id] = 0;
        }

        template <typename C> inline uint32_t rank(const MemReq* req, C cands) {
            uint32_t bestCand = -1;
            uint64_t bestScore = (uint64_t)-1L;
            for (auto ci = cands.begin(); ci != cands.end(); ci.inc()) {
                uint32_t s = score(*ci);
                bestCand = (s < bestScore)? *ci : bestCand;
                bestScore = MIN(s, bestScore);
            }
            return bestCand;
        }

        DECL_RANK_BINDINGS;

    private:
        inline uint64_t score(uint32_t id) { //higher is least evictable
            //array[id] < timestamp always, so this prioritizes by:
            // (1) valid (if not valid, it's 0)
            // (2) sharers, and
            // (3) timestamp
            return (sharersAware? cc->numSharers(id) : 0)*timestamp + array[id]*cc->isValid(id);
        }
};

//This is VERY inefficient, uses LRU timestamps to do something that in essence requires a few bits.
//If you want to use this frequently, consider a reimplementation
class TreeLRUReplPolicy : public LRUReplPolicy<true> {
    private:
        uint32_t* candArray;
        uint32_t numCands;
        uint32_t candIdx;

    public:
        TreeLRUReplPolicy(uint32_t _numLines, uint32_t _numCands) : LRUReplPolicy<true>(_numLines), numCands(_numCands), candIdx(0) {
            candArray = new uint32_t[numCands] ();
            if (numCands & (numCands-1)) panic("Tree LRU needs a power of 2 candidates, %d given", numCands);
        }

        ~TreeLRUReplPolicy() {
            delete [] candArray;
        }

        void recordCandidate(uint32_t id) {
            candArray[candIdx++] = id;
        }

        uint32_t getBestCandidate() {
            assert(candIdx == numCands);
            uint32_t start = 0;
            uint32_t end = numCands;

            while (end - start > 1) {
                uint32_t pivot = start + (end - start)/2;
                uint64_t t1 = 0;
                uint64_t t2 = 0;
                for (uint32_t i = start; i < pivot; i++) t1 = MAX(t1, array[candArray[i]]);
                for (uint32_t i = pivot; i < end; i++)   t2 = MAX(t2, array[candArray[i]]);
                if (t1 > t2) start = pivot;
                else end = pivot;
            }
            //for (uint32_t i = 0; i < numCands; i++) printf("%8ld ", array[candArray[i]]);
            //info(" res: %d (%d %ld)", start, candArray[start], array[candArray[start]]);
            return candArray[start];
        }

        void replaced(uint32_t id) override {
            candIdx = 0;
            array[id] = 0;
        }
};

//2-bit NRU, see A new Case for Skew-Associativity, A. Seznec, 1997
class NRUReplPolicy : public LegacyReplPolicy {
    private:
        //read-only
        uint32_t* array;
        uint32_t* candArray;
        uint32_t numLines;
        uint32_t numCands;

        //read-write
        uint32_t youngLines;
        uint32_t candVal;
        uint32_t candIdx;

    public:
        NRUReplPolicy(uint32_t _numLines, uint32_t _numCands) :numLines(_numLines), numCands(_numCands), youngLines(0), candIdx(0) {
            array     = new uint32_t[numLines] ();
            candArray = new uint32_t[numCands] ();
            candVal   = (1<<20);
        }

        ~NRUReplPolicy() {
            delete [] array;
            delete [] candArray;
        }

        void update(uint32_t id, const MemReq* req) override {
            //if (array[id]) info("update PRE %d %d %d", id, array[id], youngLines);
            youngLines += 1 - (array[id] >> 1); //+0 if young, +1 if old
            array[id] |= 0x2;

            if (youngLines >= numLines/2) {
                //info("youngLines = %d, shifting", youngLines);
                for (uint32_t i = 0; i < numLines; i++) array[i] >>= 1;
                youngLines = 0;
            }
            //info("update POST %d %d %d", id, array[id], youngLines);
        }

        void recordCandidate(uint32_t id) override {
            uint32_t iVal = array[id];
            if (iVal < candVal) {
                candVal = iVal;
                candArray[0] = id;
                candIdx = 1;
            } else if (iVal == candVal) {
                candArray[candIdx++] = id;
            }
        }

        uint32_t getBestCandidate() override {
            assert(candIdx > 0);
            return candArray[youngLines % candIdx]; // youngLines used to sort-of-randomize
        }

        void replaced(uint32_t id) override {
            //info("repl %d val %d cands %d", id, array[id], candIdx);
            candVal = (1<<20);
            candIdx = 0;
            array[id] = 0;
        }
};

class RandReplPolicy : public LegacyReplPolicy {
    private:
        //read-only
        uint32_t*const candArray;
        const uint32_t numCands;

        //read-write
        std::mt19937 gen;
        std::uniform_int_distribution<uint32_t> pickCand;
        uint32_t candIdx;

    public:
        explicit RandReplPolicy(uint32_t _numCands)
            : candArray(new uint32_t[_numCands]()),
              numCands(_numCands),
              gen(0x23A5F + (uint64_t)this),
              pickCand(0, numCands-1),
              candIdx(0) { }

        ~RandReplPolicy() {
            delete [] candArray;
        }

        void update(uint32_t id, const MemReq* req) override {}

        void recordCandidate(uint32_t id) override {
            candArray[candIdx++] = id;
        }

        uint32_t getBestCandidate() override {
            assert(candIdx == numCands);
            uint32_t idx = pickCand(gen);
            return candArray[idx];
        }

        void replaced(uint32_t id) override {
            candIdx = 0;
        }
};

class LFUReplPolicy : public LegacyReplPolicy {
    private:
        uint64_t timestamp; // incremented on each access
        int32_t bestCandidate; // id
        struct LFUInfo {
            uint64_t ts;
            uint64_t acc;
            LFUInfo () : ts (0), acc (0) {}
        };
        LFUInfo* array;
        uint32_t numLines;

        //NOTE: Rank code could be shared across Replacement policy implementations
        struct Rank {
            LFUInfo lfuInfo;
            uint32_t sharers;
            bool valid;

            void reset() {
                valid = false;
                sharers = 0;
                lfuInfo.ts = 0;
                lfuInfo.acc = 0;
            }

            inline bool lessThan(const Rank& other, const uint64_t curTs) const {
                if (!valid && other.valid) {
                    return true;
                } else if (valid == other.valid) {
                    if (sharers == 0 && other.sharers > 0) {
                        return true;
                    } else if (sharers > 0 && other.sharers == 0) {
                        return false;
                    } else {
                        if (lfuInfo.acc == 0) return true;
                        if (other.lfuInfo.acc == 0) return false;
                        uint64_t ownInvFreq = (curTs - lfuInfo.ts)/lfuInfo.acc; //inverse frequency, lower is better
                        uint64_t otherInvFreq = (curTs - other.lfuInfo.ts)/other.lfuInfo.acc;
                        return ownInvFreq > otherInvFreq;
                    }
                }
                return false;
            }
        };

        Rank bestRank;

    public:
        explicit LFUReplPolicy(uint32_t _numLines) : timestamp(1), bestCandidate(-1), numLines(_numLines) {
            array = new LFUInfo[numLines] ();
            bestRank.reset();
        }

        ~LFUReplPolicy() {
            delete [] array;
        }

        void update(uint32_t id, const MemReq* req) override {
            //ts is the "center of mass" of all the accesses, i.e. the average timestamp
            array[id].ts = (array[id].acc*array[id].ts + timestamp)/(array[id].acc + 1);
            array[id].acc++;
            timestamp += 1000; //have larger steps to avoid losing too much resolution over successive divisions
        }

        void recordCandidate(uint32_t id) override {
            Rank candRank = {array[id], cc? cc->numSharers(id) : 0, cc->isValid(id)};

            if (bestCandidate == -1 || candRank.lessThan(bestRank, timestamp)) {
                bestRank = candRank;
                bestCandidate = id;
            }
        }

        uint32_t getBestCandidate() override {
            assert(bestCandidate != -1);
            return (uint32_t)bestCandidate;
        }

        void replaced(uint32_t id) override {
            bestCandidate = -1;
            bestRank.reset();
            array[id].acc = 0;
        }
};

#endif  // REPL_POLICIES_H_
