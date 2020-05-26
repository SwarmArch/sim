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

#include "stream_prefetcher.h"
#include "sim/conflicts/abort_handler.h"
#include "sim/stack.h"

#undef DEBUG
#define DEBUG(args...) //info(args)

// TODO: [mha21] duplicates some logic from Core::needConflictCheck
// should de-duplicate in future
bool addrNeedsConflictCheck(Address addr) {
    if (!IsInSwarmROI()) {
        return false;
    }
    if (isUntrackedAddress((void*)addr)) return false;
    ThreadID addressOwner = ossinfo->stackTracker->owner(addr);
    if (addressOwner != INVALID_TID) {
        Address stackStart = GetStackStart(addressOwner);
        if (unlikely(!stackStart)) {
            return false;
        }

        bool isBelowStack = addr > stackStart;
        return isBelowStack;
    }
    return true;
}

bool lineNeedsConflictCheck(Address lineAddr) {
    Address startAddr = lineAddr << ossinfo->lineBits;
    return addrNeedsConflictCheck(startAddr);
}

bool nonCCAccessWithCCPrefetch(const MemReq& triggerReq, const MemReq& req) {
    bool triggerReqNeedConflictCheck = (triggerReq.ts && triggerReq.task);
    return !triggerReqNeedConflictCheck &&
        lineNeedsConflictCheck(req.lineAddr);
}

void StreamPrefetcher::setParents(uint32_t _childId, const std::vector<MemPort*>& _parents) {
    childId = _childId;
    parents = _parents;
    DEBUG("[%s] setParents: childId: %u, numParents:%zu", name(), childId, parents.size());
}

void StreamPrefetcher::setChildren(const std::vector<InvPort*>& _children) {
    children = _children;
}

void StreamPrefetcher::initStats(AggregateStat* parentStat) {
    AggregateStat* s = new AggregateStat();
    s->init(name(), "Prefetcher stats");
    profAccesses.init("acc", "Accesses"); s->append(&profAccesses);
    profPrefetches.init("pf", "Issued prefetches"); s->append(&profPrefetches);
    profDoublePrefetches.init("dpf", "Issued double prefetches"); s->append(&profDoublePrefetches);
    profPageHits.init("pghit", "Page/entry hit"); s->append(&profPageHits);
    profHits.init("hit", "Prefetch buffer hits, short and full"); s->append(&profHits);
    profShortHits.init("shortHit", "Prefetch buffer short hits"); s->append(&profShortHits);
    profStrideSwitches.init("strideSwitches", "Predicted stride switches"); s->append(&profStrideSwitches);
    profLowConfAccs.init("lcAccs", "Low-confidence accesses with no prefetches"); s->append(&profLowConfAccs);
    profNonCCAccessWithCCPrefetches.init("nonCCAccessWithCCPrefetches",
        "Triggering accesses that did not need conflict checking leading to prefetches that need conflict checking"); s->append(&profNonCCAccessWithCCPrefetches);
    parentStat->append(s);
}

uint64_t StreamPrefetcher::makePrefetchAccess(MemPort* parent,
                                              const MemReq& triggerReq,
                                              const MemReq& req,
                                              MemResp& resp,
                                              uint64_t reqCycle) {
    // Should not be making a prefetch that requires a conflict check
    // if the prefetch was triggered by a non-conflict-checked access
    assert(!nonCCAccessWithCCPrefetch(triggerReq, req));
    bool conflictCheckNeeded = lineNeedsConflictCheck(req.lineAddr);
    bool stall = false;
    uint64_t conflictCycle = reqCycle;
    if (conflictCheckNeeded) {
        AbortHandler* ah = ossinfo->abortHandler;
        switch(mode) {
        case(Mode::L2L3_MULTIPLE):
            conflictCycle = ah->checkGlobal(*req.task, req.lineAddr, reqCycle, true, stall);
            break;
        case(Mode::L1L2_MULTIPLE):
            conflictCycle = ah->checkL2(*req.task, req.lineAddr, reqCycle, true, stall);
            break;
        default:
            panic("Unexpected mode?");
        }
        assert(conflictCycle >= reqCycle);
    }

    DEBUG("[%s] making prefetch req: %s", name(), req.toString().c_str());
    uint64_t pfRespCycle = parent->access(req, resp, reqCycle);
    return std::max(conflictCycle, pfRespCycle);
}

uint64_t StreamPrefetcher::access(const MemReq& req, MemResp& resp, uint64_t cycle) {
    assert((!req.ts && !req.task) || 
        (req.ts && req.task && req.ts == &req.task->cts()));
    if (req.task != nullptr) {
        DEBUG("[%s] Request received: %s, task: %s, BEGIN", name(), req.toString().c_str(), req.task->toString().c_str());
    } else {
        DEBUG("[%s] Request received: %s BEGIN", name(), req.toString().c_str());
    }

    MemReq forwardReq = {req.lineAddr, req.type, childId, req.state, req.srcId, req.flags, req.ts, req.task};

    uint32_t parentId = getCacheBankId(req.lineAddr, ossinfo->memoryPartition,
        hashFn, parents.size());
    MemPort* parent = parents[parentId];

    //other reqs ignored, including stores    
    if (req.type != GETS) 
        return parent->access(forwardReq, resp, cycle);

    profAccesses.inc();

    uint64_t reqCycle = cycle;

    uint64_t respCycle = parent->access(forwardReq, resp, cycle);

    Address pageAddr = req.lineAddr >> 6;
    uint32_t pos = req.lineAddr & (64-1);
    uint32_t idx = 16;

    // get the highest index with a tag match
    // This loop gets unrolled and there are no control dependences. Way faster than a break (but should watch for the avoidable loop-carried dep)
    for (uint32_t i = 0; i < 16; i++) {
        bool match = (pageAddr == tag[i]) && (array[i].allocated);
        idx = match?  i : idx;  // ccmov, no branch
    }

    DEBUG("[%s] accessing 0x%lx page %lx pos %d", name(), req.lineAddr, pageAddr, pos);

    // no matching tag
    if (idx == 16) {  // entry miss
        uint32_t cand = 16;
        uint64_t candScore = -1;
        //uint64_t candScore = 0;
        for (uint32_t i = 0; i < 16; i++) {
            // warm prefetches, not even a candidate
            if (array[i].lastCycle > reqCycle + 500)
                continue;

            if (array[i].ts < candScore) {  // just LRU
                cand = i;
                candScore = array[i].ts;
            }
        }
        // get rid of the least recently used prefetched address
        if (cand < 16) {
            idx = cand;
            array[idx].alloc(reqCycle);
            array[idx].lastPos = pos;
            array[idx].ts = timestamp++;
            tag[idx] = pageAddr;
        }
        DEBUG("[%s] MISS alloc idx %d", name(), idx);
    } else {  // entry hit
        profPageHits.inc();
        Entry& e = array[idx];
        array[idx].ts = timestamp++;
        DEBUG("[%s] PAGE HIT idx %d", name(), idx);

        // 1. Did we prefetch-hit?
        bool shortPrefetch = false;
        if (e.valid[pos]) {
            uint64_t pfRespCycle = e.times[pos].respCycle;
            shortPrefetch = pfRespCycle > respCycle;
            e.valid[pos] = false;  // close, will help with long-lived transactions
            respCycle = MAX(pfRespCycle, respCycle);
            e.lastCycle = MAX(respCycle, e.lastCycle);
            profHits.inc();
            if (shortPrefetch)
                profShortHits.inc();
            DEBUG("[%s] pos %d prefetched on %ld, pf resp %ld, demand resp %ld, short %d", name(), pos, e.times[pos].startCycle, pfRespCycle, respCycle, shortPrefetch);
        }

        int32_t stride = pos - e.lastPos;
        DEBUG("[%s] pos %d lastPos %d lastLastPost %d lastCycle: %zu e.stride %d, e idx: %zu, stride: %i", name(), pos, e.lastPos, e.lastLastPos, e.lastCycle, e.stride, idx, stride);
        // 2. Update predictors, issue prefetches
        if (e.stride == stride) {
            e.conf.inc();
            if (e.conf.pred()) {  // do prefetches
                int32_t fetchDepth = (e.lastPrefetchPos - e.lastPos)/stride;
                uint32_t prefetchPos = e.lastPrefetchPos + stride;
                if (fetchDepth < 1) {
                    prefetchPos = pos + stride;
                    fetchDepth = 1;
                }
                DEBUG("[%s]: pos %d stride %d conf %d lastPrefetchPos %d prefetchPos %d fetchDepth %d", name(), pos, stride, e.conf.counter(), e.lastPrefetchPos, prefetchPos, fetchDepth);

                if (prefetchPos < 64 && !e.valid[prefetchPos]) {
                    MESIState state = I;
                    MemReq pfReq = {req.lineAddr + prefetchPos - pos, GETS, childId, state, req.srcId, MemReq::PREFETCH, req.ts, req.task};
                    MemResp pfResp;

                    if (nonCCAccessWithCCPrefetch(req, pfReq)) {
                        // TODO: [mha21] Should we clear the entry here (e.alloc())
                        // to try to prevent this pattern from happening again?
                        // or maybe update e.lastPrefetchPos?
                        profNonCCAccessWithCCPrefetches.inc();
                        return respCycle;
                    }
                    uint64_t pfRespCycle = makePrefetchAccess(parent, req, pfReq, pfResp, reqCycle);
                    e.valid[prefetchPos] = true;
                    e.times[prefetchPos].fill(reqCycle, pfRespCycle);
                    profPrefetches.inc();

                    if (shortPrefetch && fetchDepth < 8 && prefetchPos + stride < 64 && !e.valid[prefetchPos + stride]) {
                        prefetchPos += stride;
                        pfReq.lineAddr += stride;

                        if (nonCCAccessWithCCPrefetch(req, pfReq)){
                            profNonCCAccessWithCCPrefetches.inc();
                            return respCycle;
                        }
                        pfRespCycle = makePrefetchAccess(parent, req, pfReq, pfResp, reqCycle);
                        e.valid[prefetchPos] = true;
                        e.times[prefetchPos].fill(reqCycle, pfRespCycle);
                        profPrefetches.inc();
                        profDoublePrefetches.inc();
                    }
                    e.lastPrefetchPos = prefetchPos;
                    assert(state == I);  // prefetch access should not give us any permissions
                }
            } else {
                profLowConfAccs.inc();
            }
        } else {
            e.conf.dec();
            // See if we need to switch strides
            if (!e.conf.pred()) {
                int32_t lastStride = e.lastPos - e.lastLastPos;

                if (stride && stride != e.stride && stride == lastStride) {
                    e.conf.reset();
                    e.stride = stride;
                    profStrideSwitches.inc();
                    DEBUG("[%s]: setting entry: %zu stride to %i", name(), idx, stride);
                }
            }
            e.lastPrefetchPos = pos;
        }

        e.lastLastPos = e.lastPos;
        e.lastPos = pos;
    }

    DEBUG("[%s] Response cycle: %lu. DONE.", name(), respCycle);
    assert(respCycle >= cycle);
    return respCycle;
}

// TODO: [mha21] If the InvReq is for an Eviction, might want to invalidate the corresponding
// Entry in array to prevent incorrectly incrementing hit counters
uint64_t StreamPrefetcher::access(const InvReq& req, InvResp& resp, uint64_t cycle){
    DEBUG("[%s], invalidate, req: %s", name(), req.toString().c_str());
    
    uint64_t maxCycle = cycle;
    for (const auto& invPort : children) {
        InvReq creq = req;
        maxCycle = std::max(maxCycle, invPort->access(creq, resp, cycle));
    }
    return maxCycle;
}
