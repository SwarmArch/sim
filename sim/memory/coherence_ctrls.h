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

#ifndef COHERENCE_CTRLS_H_
#define COHERENCE_CTRLS_H_

#include <bitset>
#include <string>
#include <vector>
#include "sim/locks.h"
#include "sim/log.h"
#include "sim/conflicts/conflict_detection.h"
#include "sim/memory/constants.h"
#include "sim/memory/hash.h"
#include "sim/memory/memory_hierarchy.h"
#include "sim/pad.h"
#include "sim/stats/stats.h"

#undef DEBUG
#define DEBUG(args...) //info(args)

using std::vector;
using std::string;

class MemoryPartition;

//TODO: Now that we have a pure CC interface, the MESI controllers should go on different files.

/* Generic, integrated controller interface */
class CC  {
    public:
        //Initialization
        virtual void setParents(uint32_t childId, const vector<MemPort*>& parents) = 0;
        virtual void setChildren(const vector<InvPort*>& children) = 0;
        virtual void setConflictDetectionTop(ConflictDetectionTop*) = 0;
        virtual void setConflictDetectionBottom(ConflictDetectionBottom*) = 0;
        virtual void initStats(AggregateStat* cacheStat) = 0;

        //Access methods; see Cache for call sequence
        virtual bool startAccess(const MemReq& req) = 0; //initial locking, address races; returns true if access should be skipped; may change req!
        virtual bool shouldAllocate(const MemReq& req) const = 0; //called when we don't find req's lineAddr in the array
        virtual uint64_t processEviction(const MemReq& triggerReq, Address wbLineAddr, int32_t lineId, uint64_t startCycle) = 0; //called iff shouldAllocate returns true
        virtual uint64_t processAccess(const MemReq& req, MemResp& resp, int32_t lineId, uint64_t startCycle, uint64_t* getDoneCycle = nullptr) = 0;

        //Inv methods
        virtual void startInv() = 0;
        virtual uint64_t processInv(const InvReq& req, InvResp& resp,
                                    int32_t lineId, uint64_t startCycle) = 0;

        //Repl policy interface
        virtual uint32_t numSharers(uint32_t lineId) const = 0;
        virtual bool isValid(uint32_t lineId) const = 0;
        virtual MESIState getState(uint32_t lineId) const = 0;

        // Transitional interface (REMOVE ASAP)
        virtual std::bitset<MAX_CACHE_CHILDREN> hackGetSharers(uint32_t lineId, bool checkReaders) { panic("hack"); }
};


/* A MESI coherence controller is decoupled in two:
 *  - The BOTTOM controller, which deals with keeping coherence state with respect to the upper level and issues
 *    requests (accesses) to upper levels.
 *  - The TOP controller, which keeps state of lines w.r.t. lower levels of the hierarchy (e.g. sharer lists),
 *    and issues requests (invalidates) to lower levels.
 * The naming scheme is PROTOCOL-CENTRIC, i.e. if you draw a multi-level hierarchy, between each pair of levels
 * there is a top CC at the top and a bottom CC at the bottom. Unfortunately, if you look at the caches, the
 * bottom CC is at the top is at the bottom. So the cache class may seem a bit weird at times, but the controller
 * classes make more sense.
 */

class Cache;

/* NOTE: To avoid virtual function overheads, there is no BottomCC interface, since we only have a MESI controller for now */

uint32_t getCacheBankId(Address lineAddr, const MemoryPartition* mp,
                        const H3HashFamily* hf, size_t numParents);

class MESIBottomCC {
    private:
        const H3HashFamily* hashFn;
        const MemoryPartition* memPart;
        MESIState* array;
        vector<MemPort*> parents;
        const uint32_t numLines;
        uint32_t selfId;

        ConflictDetectionBottom* cd;

        //Profiling counters
        Counter profGETSHit, profGETSMiss, profGETXHit, profGETXMissIM /*from invalid*/, profGETXMissSM /*from S, i.e. upgrade misses*/;
        Counter profPUTS, profPUTX /*received from downstream*/;
        Counter profINV, profINVX, profFWD, profINVWB /*received from upstream*/;
        //Counter profWBIncl, profWBCoh /* writebacks due to inclusion or coherence, received from downstream, does not include PUTS */;
        // TODO: Measuring writebacks is messy, do if needed
        Counter profGETNextLevelLat;

        const bool nonInclusiveHack;

    public:
        MESIBottomCC(uint32_t _numLines, bool _nonInclusiveHack,
                   const H3HashFamily* _hashFn, const MemoryPartition* _memPart)
          : hashFn(_hashFn),
            memPart(_memPart),
            numLines(_numLines),
            selfId(UINT32_MAX),
            cd(nullptr),
            nonInclusiveHack(_nonInclusiveHack) {
            array = new MESIState[numLines] ();
            for (uint32_t i = 0; i < numLines; i++) {
                array[i] = I;
            }
        }

        // [mcj] I would prefer that selfId were constant, but it's very
        // annoying to set up cache relationships with Conflict Detection when
        // the Top and Bottom CC's aren't constructed.
        void init(uint32_t selfId, const vector<MemPort*>& _parents);

        void setConflictDetection(ConflictDetectionBottom* bcd);

        inline bool isExclusive(uint32_t lineId) const {
            MESIState state = array[lineId];
            return (state == E) || (state == M);
        }

        void initStats(AggregateStat* parentStat) {
            profGETSHit.init("hGETS", "GETS hits");
            profGETXHit.init("hGETX", "GETX hits");
            profGETSMiss.init("mGETS", "GETS misses");
            profGETXMissIM.init("mGETXIM", "GETX I->M misses");
            profGETXMissSM.init("mGETXSM", "GETX S->M misses (upgrade misses)");
            profPUTS.init("PUTS", "Clean evictions (from lower level)");
            profPUTX.init("PUTX", "Dirty evictions (from lower level)");
            profINV.init("INV", "Invalidates (from upper level)");
            profINVX.init("INVX", "Downgrades (from upper level)");
            profFWD.init("FWD", "Forwards (from upper level)");
            profINVWB.init("INVWB",
                           "Writebacks (to upper level) triggered by "
                           "invalidations (from upper level)");
            profGETNextLevelLat.init("latGETnl", "GET request latency on next level");

            parentStat->append(&profGETSHit);
            parentStat->append(&profGETXHit);
            parentStat->append(&profGETSMiss);
            parentStat->append(&profGETXMissIM);
            parentStat->append(&profGETXMissSM);
            parentStat->append(&profPUTS);
            parentStat->append(&profPUTX);
            parentStat->append(&profINV);
            parentStat->append(&profINVX);
            parentStat->append(&profFWD);
            parentStat->append(&profINVWB);
            parentStat->append(&profGETNextLevelLat);

            cd->initStats(parentStat);
        }

        uint64_t processEviction(Address wbLineAddr, uint32_t lineId, bool lowerLevelWriteback, uint64_t cycle, uint32_t srcId);

        uint64_t processAccess(const MemReq& req, uint32_t lineId, uint64_t cycle, bool invalidCD);

        void processWritebackOnAccess(Address lineAddr, uint32_t lineId, AccessType type);

        uint64_t processInval(const InvReq& req, InvResp& resp,
                          int32_t lineId, uint64_t cycle);

        uint64_t processNonInclusiveWriteback(const MemReq& req, uint64_t cycle);

        /* Replacement policy query interface */
        inline bool isValid(uint32_t lineId) const {
            return array[lineId] != I;
        }

        MESIState getState(uint32_t lineId) const {return array[lineId];}

        //Could extend with isExclusive, isDirty, etc, but not needed for now.

    private:
        uint32_t getParentId(Address lineAddr) const {
            return getCacheBankId(lineAddr, memPart, hashFn, parents.size());
        }
};



//Implements the "top" part: Keeps directory information, handles downgrades and invalidates
class MESITopCC {
    private:
        struct Entry {
            uint32_t numSharers;
            std::bitset<MAX_CACHE_CHILDREN> sharers;
            bool exclusive;

            void clear() {
                exclusive = false;
                numSharers = 0;
                sharers.reset();
            }

            bool isEmpty() const {
                return numSharers == 0;
            }

            bool isExclusive() const {
                return (numSharers == 1) && (exclusive);
            }
        };

        Entry* array;
        vector<InvPort*> children;
        const uint32_t numLines;

        const bool nonInclusiveHack;
        ConflictDetectionTop* cd;

    public:
        MESITopCC(uint32_t _numLines, bool _nonInclusiveHack);

        void init(const vector<InvPort*>& _children);

        void setConflictDetection(ConflictDetectionTop* _tcd);

        uint64_t processEviction(Address wbLineAddr, uint32_t lineId, bool* reqWriteback, uint64_t cycle, uint32_t srcId);

        uint64_t processAccess(const MemReq& req, MemResp& resp,
                uint32_t lineId, bool haveExclusive, bool* inducedWriteback,
                uint64_t cycle);

        uint64_t processInval(const InvReq&,
                              InvResp&,
                              int32_t lineId, uint64_t cycle);

        /* Replacement policy query interface */
        inline uint32_t numSharers(uint32_t lineId) const {
            return array[lineId].numSharers;
        }

        // Transitional interface (REMOVE ASAP)
        std::bitset<MAX_CACHE_CHILDREN> hackGetSharers(uint32_t lineId, bool checkReaders) {
            assert(lineId < numLines);
            if (!checkReaders && !array[lineId].isExclusive()) return 0;
            return array[lineId].sharers;
        }

    private:
        uint64_t sendInvalidates(const InvReq&, InvResp&, uint32_t lineId, uint64_t cycle);
};

static inline bool CheckForMESIRace(AccessType& type, MESIState* state, MESIState initialState) {
    //NOTE: THIS IS THE ONLY CODE THAT SHOULD DEAL WITH RACES. tcc, bcc et al should be written as if they were race-free.
    bool skipAccess = false;
    assert(*state == initialState);
    if (*state != initialState) {
        //info("[%s] Race on line 0x%lx, %s by childId %d, was state %s, now %s", name.c_str(), lineAddr, accessTypeNames[type], childId, mesiStateNames[initialState], mesiStateNames[*state]);
        //An intervening invalidate happened! Two types of races:
        if (IsPut(type)) { //either it is a PUT...
            //We want to get rid of this line
            if (*state == I) {
                //If it was already invalidated (INV), just skip access altogether, we're already done
                skipAccess = true;
            } else {
                //We were downgraded (INVX), still need to do the PUT
                assert(*state == S);
                //If we wanted to do a PUTX, just change it to a PUTS b/c now the line is not exclusive anymore
                if (type == PUTX) type = PUTS;
            }
        } else if (type == GETX) { //...or it is a GETX
            //In this case, the line MUST have been in S and have been INValidated
            assert(initialState == S);
            assert(*state == I);
            //Do nothing. This is still a valid GETX, only it is not an upgrade miss anymore
        } else { //no GETSs can race with INVs, if we are doing a GETS it's because the line was invalid to begin with!
            panic("Invalid true race happened (?)");
        }
    }
    return skipAccess;
}

// Non-terminal CC; accepts GETS/X and PUTS/X accesses
class MESICC : public CC {
    private:
        MESITopCC* const tcc;
        MESIBottomCC* bcc;
        const bool nonInclusiveHack;
        const string name;

        // Needed for permission-relinquishing GETS_WB
        ConflictDetectionBottom* bcd;

    public:
        //Initialization
      MESICC(uint32_t _numLines, bool _nonInclusiveHack, const string& _name,
             const H3HashFamily* _parentHashFn, const MemoryPartition* _memPart)
          : tcc(new MESITopCC(_numLines, _nonInclusiveHack)),
            bcc(new MESIBottomCC(_numLines, _nonInclusiveHack, _parentHashFn,
                                 _memPart)),
            nonInclusiveHack(_nonInclusiveHack),
            name(_name) {}

        void setParents(uint32_t childId, const vector<MemPort*>& parents) override {
            bcc->init(childId, parents);
        }

        void setChildren(const vector<InvPort*>& children) override {
            tcc->init(children);
        }

        void setConflictDetectionTop(ConflictDetectionTop* tcd) override {
            tcc->setConflictDetection(tcd);
        }

        void setConflictDetectionBottom(ConflictDetectionBottom* bcd_) override {
            bcd = bcd_;
            bcc->setConflictDetection(bcd);
        }

        void initStats(AggregateStat* cacheStat) override {
            //no tcc stats
            bcc->initStats(cacheStat);
        }

        //Access methods
        bool startAccess(const MemReq& req) override {
            assert(IsGet(req.type) || IsPut(req.type));

            // [mcj] I removed the (now-redundant) call to CheckForMESIRace. Can
            // races happen in atomic-memory, single-threaded ordspecsim? I
            // don't think so.
            return false;
        }

        bool shouldAllocate(const MemReq& req) const override {
            if (IsGet(req.type)) {
                return true;
            } else {
                assert(IsPut(req.type));
                if (!nonInclusiveHack) {
                    panic("[%s] We lost inclusion on this line! %s",
                          name.c_str(), req.toString().c_str());
                }
                return false;
            }
        }

        uint64_t processEviction(const MemReq& triggerReq, Address wbLineAddr, int32_t lineId, uint64_t startCycle) override {
            bool lowerLevelWriteback = false;
            uint64_t evCycle = tcc->processEviction(wbLineAddr, lineId, &lowerLevelWriteback, startCycle, triggerReq.srcId); //1. if needed, send invalidates/downgrades to lower level
            evCycle = bcc->processEviction(wbLineAddr, lineId, lowerLevelWriteback, evCycle, triggerReq.srcId); //2. if needed, write back line to upper level
            return evCycle;
        }

        uint64_t processAccess(const MemReq& req, MemResp& resp, int32_t lineId,
                               uint64_t startCycle, uint64_t* getDoneCycle = nullptr) override {
            uint64_t respCycle = startCycle;

            //Handle non-inclusive writebacks by bypassing
            //NOTE: Most of the time, these are due to evictions, so the line is not there. But the second condition can trigger in NUCA-initiated
            //invalidations. The alternative with this would be to capture these blocks, since we have space anyway. This is so rare is doesn't matter,
            //but if we do proper NI/EX mid-level caches backed by directories, this may start becoming more common (and it is perfectly acceptable to
            //upgrade without any interaction with the parent... the child had the permissions!)
            if (lineId == -1 || (IsPut(req.type) && !bcc->isValid(lineId))) { //can only be a non-inclusive wback
                assert(nonInclusiveHack);
                assert(IsPut(req.type));
                // [mcj] The request/response approach doesn't handle this case
                // right now.
                assert(false);
                respCycle = bcc->processNonInclusiveWriteback(req, startCycle);
            } else {
                //Prefetches are side requests and get handled a bit differently
                bool isPrefetch = req.flags & MemReq::PREFETCH;
                assert(!isPrefetch || req.type == GETS);

                //Always clear PREFETCH and GETS_WB, these flags cannot propagate up
                MemReq nofetchReq = req;
                nofetchReq.flags = req.flags & ~MemReq::PREFETCH & ~MemReq::GETS_WB;

                //GETS_WB is simply treated as a PUTS/PUTX followed by a normal GETS from I
                if (req.flags & MemReq::GETS_WB) {
                    DEBUG("%s GETS_WB", name.c_str());
                    assert(req.type == GETS);
                    assert(IsExclusive(req.state));
                    MemReq putReq = req;
                    putReq.type = (req.state == M)? PUTX : PUTS;
                    putReq.flags = req.flags &
                            (MemReq::TRACKREAD | MemReq::TRACKWRITE);
                    MemResp putResp;
                    processAccess(putReq, putResp, lineId, startCycle, nullptr);
                    assert(putResp.newState == I);
                    nofetchReq.state = I;
                }

                MESIState state = bcc->getState(lineId);
                bool invalidCD = bcd->shouldSendUp(req.type, req.ts, lineId, state);
                if (req.type == GETS && IsExclusive(state) && invalidCD) {
                    // This will be a GETS_WB, which will relinquish exclusive
                    // and will do a writeback, so issue a fake downgrade to
                    // tcc to make our children non-exclusive if needed
                    InvReq downReq = {req.lineAddr, INVX, req.srcId, nullptr, UINT32_MAX};
                    InvResp downResp;
                    // NOTE: Serialized with GET below, because it needs to get
                    // data from child if they have it dirty
                    respCycle = tcc->processInval(downReq, downResp, lineId, respCycle);
                    if (downResp.is(InvResp::WRITEBACK)) {
                        bcc->processWritebackOnAccess(req.lineAddr, lineId, GETX);
                    }
                }

                //if needed, fetch line or upgrade miss from upper level
                respCycle = bcc->processAccess(nofetchReq, lineId, respCycle, invalidCD);
                if (getDoneCycle) *getDoneCycle = respCycle;
                if (!isPrefetch) { //prefetches only touch bcc; the demand request from the core will pull the line to lower level
                    //At this point, the line is in a good state w.r.t. upper levels
                    bool lowerLevelWriteback = false;
                    //change directory info, invalidate other children if needed, tell requester about its state
                    respCycle = tcc->processAccess(nofetchReq, resp, lineId,
                            bcc->isExclusive(lineId), &lowerLevelWriteback,
                            respCycle);
                    if (lowerLevelWriteback) {
                        //Essentially, if tcc induced a writeback, bcc may need to do an E->M transition to reflect that the cache now has dirty data
                        DEBUG("[%s] process writeback on %s",
                                name.c_str(), req.toString().c_str());
                        bcc->processWritebackOnAccess(req.lineAddr, lineId, req.type);
                    }
                }
            }
            return respCycle;
        }

        void endAccess(const MemReq& req) {
            // [mcj] no locks
        }

        //Inv methods
        void startInv() override {
            // [mcj] no locks
        }

        uint64_t processInv(const InvReq& req, InvResp& resp,
                            int32_t lineId, uint64_t startCycle) override {
            // Send invalidates or downgrades to children
            uint64_t tCycle = tcc->processInval(req, resp, lineId, startCycle);
            // Adjust our own state
            uint64_t bCycle = bcc->processInval(req, resp, lineId, startCycle);
            uint64_t respCycle = std::max(tCycle, bCycle);
            assert(respCycle >= startCycle);
            return respCycle;
        }

        //Repl policy interface
        uint32_t numSharers(uint32_t lineId) const override {
            return tcc->numSharers(lineId);
        }
        bool isValid(uint32_t lineId) const override {
            return bcc->isValid(lineId);
        }
        MESIState getState(uint32_t lineId) const override {
            return bcc->getState(lineId);
        }

        // Transitional interface (REMOVE ASAP)
        std::bitset<MAX_CACHE_CHILDREN> hackGetSharers(uint32_t lineId, bool checkReaders) override { return tcc->hackGetSharers(lineId, checkReaders); }
};

// Terminal CC, i.e., without children --- accepts GETS/X, but not PUTS/X
class MESITerminalCC : public CC {
    private:
        MESIBottomCC* bcc;
        const string name;
        ConflictDetectionBottom* bcd;

    public:
        //Initialization
      MESITerminalCC(uint32_t _numLines, const string& _name,
                     const H3HashFamily* _parentHashFn,
                     const MemoryPartition* _memPart)
          : bcc(new MESIBottomCC(_numLines, false /*inclusive*/, _parentHashFn,
                                 _memPart)),
            name(_name) {}

        void setParents(uint32_t childId, const vector<MemPort*>& parents) override {
            bcc->init(childId, parents);
        }

        void setChildren(const vector<InvPort*>& children) override {
            panic("[%s] MESITerminalCC::setChildren cannot be called -- terminal caches cannot have children!", name.c_str());
        }

        void setConflictDetectionTop(ConflictDetectionTop*) override {}

        void setConflictDetectionBottom(ConflictDetectionBottom* _bcd) override {
            bcd = _bcd;
            bcc->setConflictDetection(bcd);
        }

        void initStats(AggregateStat* cacheStat) override {
            bcc->initStats(cacheStat);
        }

        //Access methods
        bool startAccess(const MemReq& req) override {
            assert(IsGet(req.type)); //no puts!
            return false;
        }

        bool shouldAllocate(const MemReq& req) const override {
            return true;
        }

        uint64_t processEviction(const MemReq& triggerReq, Address wbLineAddr, int32_t lineId, uint64_t startCycle) override {
            DEBUG("[termCC] process eviction. wbLineAddr 0x%lx line %d triggered by %s",
                  wbLineAddr, lineId, triggerReq.toString().c_str());
            bool lowerLevelWriteback = false;
            // If needed, write back line to upper level
            uint64_t endCycle = bcc->processEviction(
                    wbLineAddr, lineId, lowerLevelWriteback, startCycle,
                    triggerReq.srcId);
            return endCycle;  // critical path unaffected, but TimingCache needs it
        }

        uint64_t processAccess(const MemReq& req, MemResp& resp,
                int32_t lineId, uint64_t startCycle,
                uint64_t* getDoneCycle = nullptr) override {
            assert(lineId != -1);
            assert(!getDoneCycle);
            //if needed, fetch line or upgrade miss from upper level
            bool invalidCD = bcd->shouldSendUp(req.type, req.ts, lineId, bcc->getState(lineId));
            uint64_t respCycle = bcc->processAccess(req, lineId, startCycle, invalidCD);
            //at this point, the line is in a good state w.r.t. upper levels
            return respCycle;
        }

        void endAccess(const MemReq& req) {
            // [mcj] no locks
        }

        //Inv methods
        void startInv() override {
            // [mcj] no locks
        }

        uint64_t processInv(const InvReq& req, InvResp& resp,
                            int32_t lineId, uint64_t startCycle) override {
            // Adjust our own state
            uint64_t rCycle = bcc->processInval(req, resp, lineId, startCycle);
            // [mcj] No extra delay in terminal caches for
            // non-conflicted-checked addresses, but with conflict detection,
            // BCC may stall for some time as it aborts local speculative tasks.
            assert(rCycle == startCycle
                   || (req.timestamped() && rCycle >= startCycle));
            return rCycle;
        }

        //Repl policy interface
        //no sharers
        uint32_t numSharers(uint32_t lineId) const override { return 0; }

        bool isValid(uint32_t lid) const override { return bcc->isValid(lid); }

        MESIState getState(uint32_t lineId) const override {
            return bcc->getState(lineId);
        }
};

#endif  // COHERENCE_CTRLS_H_
