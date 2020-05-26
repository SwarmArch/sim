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

#include "sim/memory/coherence_ctrls.h"
#include <algorithm>
#include "sim/memory/cache.h"
#include "sim/timestamp.h"
#include "sim/memory/memory_partition.h"

#undef DEBUG
#define DEBUG(args...) //info(args)

class ConflictDetectionBottom;
class ConflictDetectionTop;

// TODO: we should probably have a class that deals with this
uint32_t getCacheBankId(Address lineAddr, const MemoryPartition* memPart,
                        const H3HashFamily* hashFn, size_t numParents) {
    if (memPart) {
        int32_t partId = memPart->getPartId(lineAddr, numParents);
        if (partId >= 0) return partId;
    }

    // Hash things a bit
    uint32_t res = hashFn->hash(0, lineAddr);
    uint32_t parent = (res % numParents);
    return parent;
}


void MESIBottomCC::init(uint32_t _selfId, const vector<MemPort*>& _parents) {
    selfId = _selfId;
    parents = _parents;
}

void MESIBottomCC::setConflictDetection(ConflictDetectionBottom* bcd) {
    assert(bcd);
    assert(!cd);
    cd = bcd;
}


uint64_t MESIBottomCC::processEviction(Address wbLineAddr, uint32_t lineId, bool lowerLevelWriteback, uint64_t cycle, uint32_t srcId) {
    MESIState* state = &array[lineId];
    if (lowerLevelWriteback) {
        //If this happens, when tcc issued the invalidations, it got a writeback. This means we have to do a PUTX, i.e. we have to transition to M if we are in E
        assert(*state == M || *state == E); //Must have exclusive permission!
        *state = M; //Silent E->M transition (at eviction); now we'll do a PUTX
    }
    uint64_t respCycle = cycle;
    switch (*state) {
        case I:
            break; //Nothing to do
        case S:
        case E:
            {
                MemReq req = {wbLineAddr, PUTS, selfId, *state, srcId,
                              0, nullptr, nullptr};
                MemResp resp;
                cd->eviction(req, lineId);
                respCycle = parents[getParentId(wbLineAddr)]->access(req, resp, cycle);
                *state = resp.newState;
            }
            break;
        case M:
            {
                MemReq req = {wbLineAddr, PUTX, selfId, *state, srcId,
                              0, nullptr, nullptr};
                MemResp resp;
                cd->eviction(req, lineId);
                respCycle = parents[getParentId(wbLineAddr)]->access(req, resp, cycle);
                *state = resp.newState;
            }
            break;

        default: panic("!?");
    }
    assert_msg(*state == I, "Wrong final state %s on eviction", MESIStateName(*state));
    return respCycle;
}

uint64_t MESIBottomCC::processAccess(const MemReq& req, uint32_t lineId, uint64_t cycle, bool invalidCD) {
    uint64_t respCycle = cycle;
    MESIState* state = &array[lineId];

    // Concurrent with sending the memory access (if that happens),
    // the CD unit searches for and resolves local conflicts.
    const uint64_t cdConcurrentCycle = cd->access(req, cycle);

    Address lineAddr = req.lineAddr;
    switch (req.type) {
        // A PUTS/PUTX does nothing w.r.t. higher coherence levels --- it dies here
        case PUTS: //Clean writeback, nothing to do (except profiling)
            assert(*state != I);
            profPUTS.inc();
            break;
        case PUTX: //Dirty writeback
            assert(*state == M || *state == E);
            if (*state == E) {
                //Silent transition, record that block was written to
                *state = M;
            }
            profPUTX.inc();
            break;
        case GETS:
            if (*state == I) {
                profGETSMiss.inc();
            } else {
                profGETSHit.inc();
            }
            // Forward on the request if a timestamp remains piggy-backed.
            if (*state == I || invalidCD) {
                MemReq parentReq = {req.lineAddr, req.type, selfId, *state, req.srcId, req.flags, req.ts, req.task};
                // GETS_WB is needed if we need to relinquish exclusivity on a GETS
                if (invalidCD && IsExclusive(*state)) {
                    parentReq.flags |= MemReq::GETS_WB;
                    cd->eviction(parentReq, lineId);
                }
                MemResp resp;
                respCycle = parents[getParentId(lineAddr)]->access(parentReq, resp, cycle);
                *state = resp.newState;
                profGETNextLevelLat.inc(respCycle - cycle);
                // NOTE(dsm): Final states for GETS are ALWAYS S or E; that's why, if we have M, we issue a GETS_WB
                assert_msg(*state == S || *state == E,
                           "A conflict-checked GETS may have originated from "
                           "a cache with M permission. Req %s state=%s",
                           parentReq.toString().c_str(),
                           MESIStateName(*state));
                cd->lineInstalled(req, resp, lineId, invalidCD);
            }
            break;
        case GETX:
            // Profile before access, state changes
            // [mcj] Because of the timestamp check, profile miss/hit counters
            // before sending messages up (or not).
            switch(*state) {
                case I: profGETXMissIM.inc(); break;
                case S: profGETXMissSM.inc(); break;
                default: profGETXHit.inc(); break;
            }

            // Forward on the request if a timestamp remains piggy-backed.
            if (*state == I || *state == S || invalidCD) {
                MemReq parentReq = {req.lineAddr, req.type, selfId, *state, req.srcId, req.flags, req.ts, req.task};
                MemResp resp;
                respCycle = parents[getParentId(lineAddr)]->access(parentReq, resp, cycle);
                *state = resp.newState;
                profGETNextLevelLat.inc(respCycle - cycle);
                cd->lineInstalled(req, resp, lineId, invalidCD);
            } else {
                if (*state == E) {
                    // Silent transition
                    // NOTE: When do we silent-transition E->M on an ML hierarchy... on a GETX, or on a PUTX?
                    /* Actually, on both: on a GETX b/c line's going to be modified anyway, and must do it if it is the L1 (it's OK not
                     * to transition if L2+, we'll TX on the PUTX or invalidate, but doing it this way minimizes the differences between
                     * L1 and L2+ controllers); and on a PUTX, because receiving a PUTX while we're in E indicates the child did a silent
                     * transition and now that it is evictiong, it's our turn to maintain M info.
                     */
                    *state = M;
                }
            }
            assert_msg(*state == M, "Wrong final state on GETX, lineId %d numLines %d, finalState %s", lineId, numLines, MESIStateName(*state));
            break;

        default: panic("!?");
    }
    respCycle = std::max(respCycle, cdConcurrentCycle);

    assert(respCycle >= cycle);
    return respCycle;
}

void MESIBottomCC::processWritebackOnAccess(Address lineAddr, uint32_t lineId, AccessType type) {
    MESIState* state = &array[lineId];
    assert_msg(*state == M || *state == E,
            "addr=0x%lx state=%s type=%s",
            lineAddr, MESIStateName(*state), AccessTypeName(type));
    if (*state == E) {
        //Silent transition to M if in E
        *state = M;
    }
}

uint64_t MESIBottomCC::processInval(const InvReq& req, InvResp& resp,
                                    int32_t lineId, uint64_t cycle) {
    // [mcj] The conflict detection module can stall, as it checks for conflicts
    // and resolves them, perhaps by injecting GETX's into the network.
    // TODO(mcj) Should this run in parallel with other events?
    uint64_t invCycle = cd->invalidate(req, resp, lineId, cycle);

    if (lineId < 0) {
        assert(lineId == -1);
        assert(req.timestamped());
        return invCycle;
    }

    MESIState* state = &array[lineId];

    if (*state == I) {
        // (mcj) is it legal for the lineId to be valid, but the state to
        // be invalid? [dsm] Absolutely
        assert(req.timestamped());
        return invCycle;
    }

    switch (req.type) {
        case INVX: //lose exclusivity
            //Hmmm, do we have to propagate loss of exclusivity down the tree? (nah, topcc will do this automatically -- it knows the final state, always!)
            assert_msg(*state == E || *state == M ||
                       (*state == S && req.timestamped()),
                       "Invalid state %s", MESIStateName(*state));
            if (*state == M) {
                resp.set(InvResp::WRITEBACK);
                profINVWB.inc();
            }
            *state = S;
            profINVX.inc();
            break;
        case INV: //invalidate
            assert(*state != I);
            if (*state == M) {
                resp.set(InvResp::WRITEBACK);
                profINVWB.inc();
            }
            *state = I;
            profINV.inc();
            break;
        case FWD: //forward
            assert_msg(*state == S, "Invalid state %s on FWD", MESIStateName(*state));
            profFWD.inc();
            break;
        default: panic("!?");
    }
    //NOTE: BottomCC never calls up on an invalidate, so it adds no extra latency

    return invCycle;
}


uint64_t MESIBottomCC::processNonInclusiveWriteback(const MemReq& req, uint64_t cycle) {
    assert_msg(nonInclusiveHack,
            "Non-inclusive %s, this cache should be inclusive",
             req.toString().c_str());

    //info("Non-inclusive wback, forwarding");
    MemReq fwdReq = req;
    fwdReq.childId = selfId;
    fwdReq.set(MemReq::NONINCLWB);
    fwdReq.ts = nullptr;
    MemResp resp;
    uint64_t respCycle = parents[getParentId(req.lineAddr)]->access(fwdReq, resp, cycle);
    return respCycle;
}


/* MESITopCC implementation */

MESITopCC::MESITopCC(uint32_t _numLines, bool _nonInclusiveHack)
  : numLines(_numLines), nonInclusiveHack(_nonInclusiveHack),
    cd(nullptr) {
    array = new Entry[numLines] ();
    for (uint32_t i = 0; i < numLines; i++) {
        array[i].clear();
    }
}

void MESITopCC::init(const vector<InvPort*>& _children) {
    if (_children.size() > MAX_CACHE_CHILDREN) {
        panic("Children size (%d) > MAX_CACHE_CHILDREN (%d)", (uint32_t)_children.size(), MAX_CACHE_CHILDREN);
    }
    children = _children;
}

void MESITopCC::setConflictDetection(ConflictDetectionTop* tcd) {
    assert(tcd);
    assert(!cd);
    cd = tcd;
}

// Helper method
static void propagateLastAccTS(const TimeStamp* const& from, const TimeStamp*& to) {
    if (!to || (from && (*from > *to))) to = from;
}

uint64_t MESITopCC::sendInvalidates(const InvReq& req, InvResp& resp,
                                    uint32_t lineId, uint64_t cycle) {
    //Send down downgrades/invalidates
    Entry* e = &array[lineId];

    // Don't propagate downgrades if sharers are not exclusive,
    // and there isn't a valid piggy-backed timestamp to propagate.
    if (req.type == INVX && !e->isExclusive() && !req.timestamped()) {
        return cycle;
    }

    uint64_t maxCycle = cycle; //keep maximum cycle only, we assume all invals are sent in parallel

    // Invalidation targets include:
    // - For INV, all children who have the line installed
    // - For INVX, the child with exclusive permission, if any (non-exclusive
    //   sharers are NOT included)
    // - Tiles that have speculatively accessed (INV) or written (INVX)  the
    //   address if the request is timestamped
    std::bitset<MAX_CACHE_CHILDREN> invTargets;
    if (req.type == INV || e->isExclusive()) invTargets = e->sharers;
    if (req.timestamped()) invTargets |= cd->sharers(req.lineAddr, req.type == INV);
    // FIXME(mcj) I'd rather see this field used only inside Conflict Detection
    if (req.skipChildId < invTargets.size()) invTargets[req.skipChildId] = false;
    if (invTargets.any()) {
        uint32_t sentInvs = 0;

        // Randomize iteration of children to avoid systemic bias. This is
        // probably important both for conflict-checking invalidations and
        // regular data accesses. The simulator may otherwise always schedule
        // messages for lower-indexed children before higher-indexed children
        std::vector<uint32_t> ids(children.size());
        std::iota(ids.begin(), ids.end(), 0ul);
        std::random_shuffle(ids.begin(), ids.end());
        for (uint32_t c : ids) {
            if (invTargets[c]) {
                InvReq creq = req;
                // The child's invalidation shouldn't skip any of its children;
                // the invalidation is coming from their grandparent/above.
                // [mcj] setting this value here seems wrong.
                creq.skipChildId = UINT32_MAX;
                InvResp cresp;
                uint64_t respCycle = children[c]->access(creq, cresp, cycle);
                respCycle = cd->invalidated(c, creq, cresp, respCycle);
                maxCycle = std::max(respCycle, maxCycle);
                if (req.type == INV) e->sharers[c] = false;
                if (cresp.is(InvResp::WRITEBACK)) resp.set(InvResp::WRITEBACK);
                if (cresp.is(InvResp::TRACKREAD)) resp.set(InvResp::TRACKREAD);
                if (cresp.is(InvResp::TRACKWRITE)) resp.set(InvResp::TRACKWRITE);
                propagateLastAccTS(cresp.lastAccTS, resp.lastAccTS);

                sentInvs++;
            }
        }
        if (req.type == INV) {
            assert(sentInvs >= e->numSharers);
            e->numSharers = 0;
        } else {
            //TODO: This is kludgy -- once the sharers format is more sophisticated, handle downgrades with a different codepath
            assert(e->isExclusive() || req.timestamped());
            e->exclusive = false;
        }
    }
    return maxCycle;
}

uint64_t MESITopCC::processEviction(Address wbLineAddr, uint32_t lineId,
        bool* reqWriteback, uint64_t cycle, uint32_t srcId) {
    uint64_t respCycle = cycle;
    if (nonInclusiveHack) {
        // Don't invalidate anything, just clear our entry
        array[lineId].clear();
    } else {
        //Send down invalidates
        // [mcj] No need to abort future tasks due to an eviction
        InvReq req = {wbLineAddr, INV, srcId, nullptr, UINT32_MAX};
        InvResp resp;
        respCycle = sendInvalidates(req, resp, lineId, cycle);
        assert(reqWriteback);
        *reqWriteback = resp.is(InvResp::WRITEBACK);
    }
    return respCycle;
}

uint64_t MESITopCC::processAccess(const MemReq& req, MemResp& resp,
        uint32_t lineId,
        bool haveExclusive, bool* inducedWriteback, uint64_t cycle) {
    Entry* e = &array[lineId];

    // We shouldn't track exclusivity when the requester is a sharer.
    assert(!e->isExclusive() || req.state != S);

    uint64_t respCycle = cycle;
    switch (req.type) {
        case PUTX:
            assert(e->isExclusive());
            if (req.flags & MemReq::PUTX_KEEPEXCL) {
                assert(e->sharers[req.childId]);
                assert(req.state == M);
                resp.newState = E; //they don't hold dirty data anymore
                break; //don't remove from sharer set. It'll keep exclusive perms.
            }
            //note NO break in general
        case PUTS:
            assert(e->sharers[req.childId]);
            e->sharers[req.childId] = false;
            e->numSharers--;
            resp.newState = I;
            break;
        case GETS:
            {
            DEBUG("[CC] Setting directory sharer child: %u", req.childId);
            bool giveE = e->isEmpty() && haveExclusive && !req.is(MemReq::NOEXCL);
            if (giveE) {
                // We can only give exclusive if no other children have
                // speculative reads or writes outstanding
                giveE = cd->giveExclusive(req);
            }

            if (giveE) {
                //Give in E state
                e->exclusive = true;
                e->sharers[req.childId] = true;
                e->numSharers = 1;
                resp.newState = E;
            } else {
                //Give in S state

                // If child is in sharers list (this is a conflict check),
                // take it out
                if (e->sharers[req.childId]) {
                    assert(req.timestamped());
                    assert(req.state != I);

                    e->sharers[req.childId] = false;
                    e->numSharers--;
                }

                // Downgrade the exclusive or speculative sharers, if any
                InvReq invreq = {req.lineAddr, INVX, req.srcId, req.ts, req.childId};
                InvResp invresp;
                respCycle = sendInvalidates(invreq, invresp, lineId, cycle);
                *inducedWriteback = invresp.is(InvResp::WRITEBACK);
                propagateLastAccTS(invresp.lastAccTS, resp.lastAccTS);

                assert_msg(!e->isExclusive(), "Can't have exclusivity here. isExcl=%d excl=%d numSharers=%d", e->isExclusive(), e->exclusive, e->numSharers);
                assert(e->sharers[req.childId] == false);

                e->sharers[req.childId] = true;
                e->numSharers++;
                e->exclusive = false; //dsm: Must set, we're explicitly non-exclusive
                resp.newState = S;
            }
            }
            break;
        case GETX:
            assert(haveExclusive); //the current cache better have exclusive access to this line

            // If child is in sharers list,
            // (this is an upgrade miss or conflict check), take it out
            if (e->sharers[req.childId]) {
                assert_msg(!e->isExclusive() || req.timestamped(),
                           "Spurious GETX without timestamp, %s "
                           "numSharers=%d isExcl=%d excl=%d",
                           req.toString().c_str(),
                           e->numSharers, e->isExclusive(), e->exclusive);
                e->sharers[req.childId] = false;
                e->numSharers--;
            }

            // Invalidate all other copies and speculative sharers
            {
                InvReq invreq = {req.lineAddr, INV, req.srcId, req.ts, req.childId};
                InvResp invresp;
                respCycle = sendInvalidates(invreq, invresp, lineId, cycle);
                *inducedWriteback = invresp.is(InvResp::WRITEBACK);
                propagateLastAccTS(invresp.lastAccTS, resp.lastAccTS);
            }

            // Set current sharer, mark exclusive
            e->sharers[req.childId] = true;
            e->numSharers++;
            e->exclusive = true;

            assert(e->numSharers == 1);

            resp.newState = M; //give in M directly
            break;

        default: panic("!?");
    }

    // TileCD needs to send the right timestamp to the L1
    if (IsGet(req.type)) {
        bool checkReaders = (req.type == GETX || resp.newState == E);
        // May modify resp.lastAccTS
        cd->propagateLastAccTS(req.lineAddr, req.ts, lineId, resp.lastAccTS, checkReaders);
    }

    return respCycle;
}

uint64_t MESITopCC::processInval(const InvReq& req, InvResp& resp,
                                 int32_t lineId, uint64_t cycle) {
    // if it's a FWD, we should be inclusive for now, so we must have the line,
    // just invLat works
    if (req.type == FWD) {
        assert(!nonInclusiveHack); //dsm: ask me if you see this failing and don't know why
        return cycle;
    } else {
        //Just invalidate or downgrade down to children as needed
        if (lineId >= 0) {
            return sendInvalidates(req, resp, lineId, cycle);
        } else {
            // Nothing to do, L2 is inclusive so L1s won't have any lines
            assert(req.timestamped());
            return cycle;
        }
    }
}
