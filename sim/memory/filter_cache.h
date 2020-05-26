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

#ifndef FILTER_CACHE_H_
#define FILTER_CACHE_H_

#include <malloc.h>
#include <string>
#include "sim/bithacks.h"
#include "sim/memory/cache.h"
#include "sim/task.h"

using std::string;

#undef DEBUG
#define DEBUG(args...) //info(args)

/* Extends Cache with an L0 direct-mapped cache, very much optimized for hits
 *
 * L1 lookups are dominated by several kinds of overhead (grab the cache locks,
 * several virtual functions for the replacement policy, etc.). This
 * specialization of Cache solves these issues by having a filter array that
 * holds the most recently used line in each set. Accesses check the filter array,
 * and then go through the normal access path. Because there is one line per set,
 * it is fine to do this without grabbing a lock.
 */

class FilterCache : public Cache {
    private:
        struct FilterEntry {
            // [mcj] This simulator is sequential, so volatile is unnecessary
            Address rdAddr;
            Address wrAddr;
            uint64_t availCycle;

            void clear() {wrAddr = 0ul; rdAddr = 0ul; availCycle = 0ul;}
        };

        //Replicates the most accessed line of each set in the cache
        FilterEntry* filterArray;
        const uint32_t lineBits;
        const uint32_t numSets;
        const Address setMask;
        uint32_t srcId; //should match the core
        uint32_t reqFlags;

        uint64_t fGETSHit, fGETXHit;

    public:
        FilterCache(uint32_t _lineBits, uint32_t _numSets, uint32_t _numLines, CC* _cc, CacheArray* _array,
                ReplPolicy* _rp, uint32_t _accLat, uint32_t _invLat, const string& _name)
            : Cache(_numLines, _cc, _array, _rp, _accLat, _invLat, _name),
              lineBits(_lineBits), numSets(_numSets), setMask(numSets - 1)
        {
            assert(!(numSets & setMask));  // must be a power of 2
            filterArray = static_cast<FilterEntry*>(memalign(CACHE_LINE_BYTES, numSets * sizeof(FilterEntry)));
            for (uint32_t i = 0; i < numSets; i++) filterArray[i].clear();
            fGETSHit = fGETXHit = 0;
            srcId = -1;
            reqFlags = 0;
        }

        void setSourceId(uint32_t id) {
            srcId = id;
        }

        inline uint32_t getSourceId() const { return srcId; }
        inline uint32_t getLineBits() const { return lineBits; }

        void setFlags(uint32_t flags) {
            reqFlags = flags;
        }

        void initStats(AggregateStat* parentStat) override {
            AggregateStat* cacheStat = new AggregateStat();
            cacheStat->init(name(), "Filter cache stats");

            auto fgetsLambda = [this]() { return fGETSHit; };
            auto fgetsStat = makeLambdaStat(fgetsLambda);
            fgetsStat->init("fhGETS", "Filtered GETS hits");
            auto fgetxLambda = [this]() { return fGETXHit; };
            auto fgetxStat = makeLambdaStat(fgetxLambda);
            fgetxStat->init("fhGETX", "Filtered GETX hits");
            cacheStat->append(fgetsStat);
            cacheStat->append(fgetxStat);

            initCacheStats(cacheStat);
            parentStat->append(cacheStat);
        }

        inline uint64_t load(Address vAddr, uint64_t curCycle, const TaskPtr& task) {
            Address vLineAddr = vAddr >> lineBits;
            uint32_t idx = vLineAddr & setMask;
            uint64_t availCycle = filterArray[idx].availCycle; //read before, careful with ordering to avoid timing races
            if ((vLineAddr == filterArray[idx].rdAddr)
                    && (vLineAddr != 0)
                    // [mcj] All conflict-checked addresses must proceed to the
                    // coherence controller which is what notifies conflict
                    // detection
                    // FIXME (dsm): Clear the FilterCache after every task instead (and add a valid bitset to make that fast)
                    && (!task)) {
                fGETSHit++;
                DEBUG("[%s] Filter cache hit, idx: %d, addr: 0x%lx (0x%lx), "
                      "curCycle: %lu, availCycle: %lu",
                      name(), idx, vLineAddr, vAddr, curCycle, availCycle);
                return std::max(curCycle, availCycle);
            } else {
                return replace(vLineAddr, idx, true, curCycle, task);
            }
        }

        inline uint64_t store(Address vAddr, uint64_t curCycle, const TaskPtr& task) {
            Address vLineAddr = vAddr >> lineBits;
            uint32_t idx = vLineAddr & setMask;
            uint64_t availCycle = filterArray[idx].availCycle; //read before, careful with ordering to avoid timing races
            if ((vLineAddr == filterArray[idx].wrAddr)
                    && (vLineAddr != 0)
                    && (!task)) {
                fGETXHit++;
                DEBUG("[%s] Filter cache hit, idx: %d, addr: 0x%lx (0x%lx), "
                      "curCycle: %lu, availCycle: %lu",
                      name(), idx, vLineAddr, vAddr, curCycle, availCycle);
                //NOTE: Stores don't modify availCycle; we'll catch matches in the core
                //filterArray[idx].availCycle = curCycle; //do optimistic store-load forwarding
                return std::max(curCycle, availCycle);
            } else {
                uint64_t startCycle = curCycle;
                // [maleen] upgrade miss should start only after any previous read-miss completes.
                if ((vLineAddr == filterArray[idx].rdAddr)
                        && (vLineAddr != 0) ){
                    startCycle = std::max(startCycle, availCycle);
                }
                return replace(vLineAddr, idx, false, startCycle, task);
            }
        }

        uint64_t replace(Address vLineAddr, uint32_t idx, bool isLoad,
                         uint64_t curCycle, const TaskPtr& task) {
            Address pLineAddr = vLineAddr;
            const TimeStamp* ts = (task) ? &task->cts() : nullptr;
            MemReq req = {pLineAddr, isLoad? GETS : GETX, 0, MESIState::I,
                          srcId, reqFlags, ts, task};
            MemResp resp;
            DEBUG("[%s] Filter cache miss, idx: %d, addr: 0x%lx, curCycle: %lu",
                  name(), idx, pLineAddr, curCycle);
            uint64_t respCycle = Cache::access(req, resp, curCycle);

            //Careful with this order
            Address oldAddr = filterArray[idx].rdAddr;
            filterArray[idx].wrAddr = isLoad? -1L : vLineAddr;
            filterArray[idx].rdAddr = vLineAddr;

            //For LSU simulation purposes, loads bypass stores even to the same line if there is no conflict,
            //(e.g., st to x, ld from x+8) and we implement store-load forwarding at the core.
            //So if this is a load, it always sets availCycle; if it is a store hit, it doesn't
            if (oldAddr != vLineAddr) {
                filterArray[idx].availCycle = respCycle;
            } else {
                // With Scoreboarding, replace() could be called for the same
                // address, before the respCycle for a prev access
                assert(ossinfo->isScoreboard ||
                       (respCycle >= filterArray[idx].availCycle));
            }
            DEBUG("[%s] idx: %d, %s availCycle=%lu",
                  name(), idx,
                  (oldAddr != vLineAddr) ? "setting" : "retaining old",
                  filterArray[idx].availCycle);
            return std::max(respCycle, filterArray[idx].availCycle);
        }

        uint64_t access(const MemReq&, MemResp&, uint64_t) override {
            panic("FilterCache's MemPort cannot be accessed directly");
        }

        uint64_t access(const InvReq& req, InvResp& resp, uint64_t cycle) override {
            Cache::startInvalidate();  // grabs cache's downLock
            uint32_t idx = req.lineAddr & setMask; //works because of how virtual<->physical is done...
            if ((filterArray[idx].rdAddr) == req.lineAddr) {
                filterArray[idx].wrAddr = -1L;
                filterArray[idx].rdAddr = -1L;
            }
            uint64_t respCycle = Cache::finishInvalidate(req, resp, cycle);
            return respCycle;
        }

        void contextSwitch() {
            for (uint32_t i = 0; i < numSets; i++) filterArray[i].clear();
        }

        void flush(const vector<bool>& flushLineIds, uint64_t cycle) {
            contextSwitch();  // no need to keep filterArray around, it's a performance optimization
            Cache::flush(flushLineIds, srcId, cycle);
        }
};

#endif  // FILTER_CACHE_H_
