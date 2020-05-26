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

#include "sim/memory/cache.h"
#include <string>
#include <vector>
#include "sim/memory/hash.h"
#include "sim/task.h"

using std::string;
using std::vector;

#undef DEBUG
#define DEBUG(args...) //info(args)

Cache::Cache(uint32_t _numLines, CC* _cc, CacheArray* _array, ReplPolicy* _rp, uint32_t _accLat, uint32_t _invLat, const string& _name)
    : SimObject(_name), cc(_cc), array(_array), rp(_rp), numLines_(_numLines), accLat(_accLat), invLat(_invLat) {}

void Cache::setParents(uint32_t childId, const vector<MemPort*>& parents) {
    cc->setParents(childId, parents);
}

void Cache::setChildren(const vector<InvPort*>& children) {
    cc->setChildren(children);
}

void Cache::setConflictDetectionTop(ConflictDetectionTop* tcd) {
    cc->setConflictDetectionTop(tcd);
}

void Cache::setConflictDetectionBottom(ConflictDetectionBottom* bcd) {
    cc->setConflictDetectionBottom(bcd);
}


void Cache::initStats(AggregateStat* parentStat) {
    AggregateStat* cacheStat = new AggregateStat();
    cacheStat->init(name(), "Cache stats");
    initCacheStats(cacheStat);
    parentStat->append(cacheStat);
}

void Cache::initCacheStats(AggregateStat* cacheStat) {
    cc->initStats(cacheStat);
    array->initStats(cacheStat);
    rp->initStats(cacheStat);
}

int32_t Cache::probe(Address lineAddr) const {
    //Do lookup like in access, updateRepl = true (or make it configurable)
    int32_t lineId = array->lookup(lineAddr, nullptr, false);
    DEBUG("[%s] probe: %lx -> %d", name(), lineAddr, lineId);
    if (lineId == -1 || !cc->isValid(lineId)) {
        return -1;
    } else {
        return lineId;
    }
}

uint64_t Cache::access(const MemReq& req, MemResp& resp, uint64_t cycle) {
    DEBUG("[%s] Request received: %s BEGIN", name(), req.toString().c_str());
    assert((!req.ts && !req.task) || 
        (req.ts && req.task && req.ts == &req.task->cts()));

    uint64_t respCycle = cycle;
    bool skipAccess = cc->startAccess(req); //may need to skip access due to races (NOTE: may change req.type!)

    // [mcj] ordspecsim should not have races
    // 1) because the simulator is sequential
    // 2) because hopefully when breaking up accesses into non-atomic events
    //    we still don't introduce these races
    // dsm: Haha, (2) would cause so many more races than these. It'd be insane.
    assert(!skipAccess);

    bool updateReplacement = IsGet(req.type);
    int32_t lineId = array->lookup(req.lineAddr, &req, updateReplacement);
    respCycle += accLat;

    if (lineId == -1 && cc->shouldAllocate(req)) {
        //Make space for new line
        Address wbLineAddr;
        lineId = array->preinsert(req.lineAddr, &req, &wbLineAddr); //find the lineId to replace
        DEBUG("[%s] Evicting 0x%lx", name(), wbLineAddr);

        //Evictions are not in the critical path in any sane implementation -- we do not include their delays
        //NOTE: We might be "evicting" an invalid line for all we know. Coherence controllers will know what to do
        cc->processEviction(req, wbLineAddr, lineId, respCycle); //1. if needed, send invalidates/downgrades to lower level

        array->postinsert(req.lineAddr, &req, lineId); //do the actual insertion. NOTE: Now we must split insert into a 2-phase thing because cc unlocks us.
    }

    respCycle = cc->processAccess(req, resp, lineId, respCycle);

    assert_msg(respCycle >= cycle, "[%s] resp < req? %s respCycle %ld",
            name(), req.toString().c_str(), respCycle);

    DEBUG ("[%s] Response cycle: %lu. DONE. [final state %s]", name(), respCycle, MESIStateName(cc->getState(lineId)));
    assert(respCycle >= cycle);
    return respCycle;
}

void Cache::startInvalidate() {
    cc->startInv(); //note we don't grab tcc; tcc serializes multiple up accesses, down accesses don't see it
}

uint64_t Cache::finishInvalidate(const InvReq& req, InvResp& resp, uint64_t cycle) {
    int32_t lineId = array->lookup(req.lineAddr, nullptr, false);

    assert_msg(lineId != -1 || req.timestamped(),
            "[%s] Invalidate on non-existing lineId %d %s",
            name(), lineId, req.toString().c_str());

    uint64_t respCycle = cycle + invLat;

    DEBUG("[%s] Invalidate start lineId %d %s",
            name(), lineId, req.toString().c_str());

    // Send invalidates or downgrades to children, and adjust our own state
    respCycle = cc->processInv(req, resp, lineId, respCycle);

    DEBUG("[%s] Invalidate end lineId %d %s resp %s latency %ld",
            name(), lineId, req.toString().c_str(),
            resp.toString().c_str(),
            respCycle - cycle);
    return respCycle;
}
