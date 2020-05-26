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

#ifndef CACHE_H_
#define CACHE_H_

#include <string>
#include <vector>
#include "sim/memory/cache_arrays.h"
#include "sim/memory/coherence_ctrls.h"
#include "sim/memory/memory_hierarchy.h"
#include "sim/memory/repl_policies.h"
#include "sim/object.h"
#include "sim/stats/stats.h"

class ConflictDetectionTop;
class ConflictDetectionBottom;

/* General coherent modular cache. The replacement policy and cache array are
 * pretty much mix and match. The coherence controller interfaces are general
 * too, but to avoid virtual function call overheads we work with MESI
 * controllers, since for now we only have MESI controllers
 */
class Cache : public SimObject, public MemPort, public InvPort {
    protected:
        CC* cc;
        CacheArray* array;
        ReplPolicy* rp;
        const uint32_t numLines_;

        // Latencies
        const uint32_t accLat;  // latency of a normal access (could split in get/put, probably not needed)
        const uint32_t invLat;  // latency of an invalidation

    public:
        Cache(uint32_t _numLines, CC* _cc, CacheArray* _array, ReplPolicy* _rp, uint32_t _accLat, uint32_t _invLat, const std::string& _name);

        virtual void setParents(uint32_t childId, const std::vector<MemPort*>& parents);
        virtual void setChildren(const std::vector<InvPort*>& children);
        virtual void initStats(AggregateStat* parentStat);

        // [mcj] In the original design, these were passed directly to the CC,
        // so that the Cache could remain oblivious. The only way to go back to
        // that approach is to build ConflictDetection with each Cache within
        // BuildCacheBank, and return a pair<Cache*, AtomicPort*> or something.
        void setConflictDetectionTop(ConflictDetectionTop*);
        void setConflictDetectionBottom(ConflictDetectionBottom*);

        uint32_t numLines() const { return numLines_; }
        virtual int32_t probe(Address lineAddr) const;
        uint64_t access(const MemReq& req, MemResp& resp,
                                uint64_t cycle) override;

        // Selectively flush lineIds
        // flushLines is a numLines-sized vector, has true on every lineId we need to flush
        void flush(const vector<bool>& flushLineIds, uint32_t srcId, uint64_t cycle) {
            assert(flushLineIds.size() <= numLines());
            // This simply uses cc's eviction machinery, which for interface
            // reasons, wants the request that triggers the eviction. This
            // request is only used for cycle and srcId
            MemReq triggerReq = {0x42 /* doesn't matter */, GETS, 0,
                    MESIState::I, srcId, 0, nullptr, nullptr};
            for (uint32_t lineId = 0; lineId < flushLineIds.size(); lineId++) {
                if (!flushLineIds[lineId]) continue;
                Address lineAddr = array->reverseLookup(lineId);
                cc->processEviction(triggerReq, lineAddr, lineId, cycle);
                assert(!cc->isValid(lineId)); // cc should have invalidated
            }
        }

        // InvPort
        // NOTE: WRITEBACK is pulled up to true, but not pulled down to false.
        uint64_t access(const InvReq& req, InvResp& resp,
                uint64_t cycle) override {
            startInvalidate();
            return finishInvalidate(req, resp, cycle);
        }

        inline MESIState getState(int32_t lineId) const {
            if (lineId == -1) return I;
            return cc->getState(lineId);
        }

        // Transitional interface (REMOVE ASAP)
        std::bitset<MAX_CACHE_CHILDREN> hackGetSharers(Address lineAddr, bool checkReaders) {
            int32_t lineId = probe(lineAddr);
            if (lineId == -1) return 0;
            return cc->hackGetSharers(lineId, checkReaders);
        }

    protected:
        void initCacheStats(AggregateStat* cacheStat);

        void startInvalidate(); // grabs cc's downLock
        // performs inv and releases downLock
        uint64_t finishInvalidate(const InvReq& req, InvResp& resp, uint64_t cycle);
};

#endif  // CACHE_H_
