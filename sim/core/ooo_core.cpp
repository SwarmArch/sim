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

#include "ooo_core.h"
#include "sim/conflicts/terminal_cd.h" // FIXME(mcj)

extern "C" {
#include "xed-interface.h"
}
#undef DEBUG
#define DEBUG(args...) //info(args)

#undef DEBUG_THROTTLE
#define DEBUG_THROTTLE(args...) //info(args)

#undef DEBUG_STATS
#define DEBUG_STATS(args...) //info(args)

#undef DEBUG_BRANCH
#define DEBUG_BRANCH(args...) //info(args)

// Stages --- more or less matched to Westmere, but have not seen detailed pipe diagrams anywhare
// NOTE: Changed for KNL (shorter 2-way pipe)
#define FETCH_STAGE 1
#define DECODE_STAGE 3  // NOTE: Decoder adds predecode delays to decode
#define ISSUE_STAGE 5
#define DISPATCH_STAGE 8  // RAT + ROB + RS, each is easily 1-2 cycles

#define L1D_LAT 3  // fixed, and FilterCache does not include L1 delay
#define FETCH_BYTES_PER_CYCLE 8
#define ISSUES_PER_CYCLE 2
#define RF_READS_PER_CYCLE 2

OoOCore::OoOCore(const std::string& _name, uint32_t _cid,
               uint32_t numctxts, FilterCache* _l1d,
               IssuePriority priority,
               uint64_t issueWidth,
               uint64_t mispredictPenalty,
               uint32_t loadBufferEntries,
               uint32_t storeBufferEntries, uint32_t startThreads,
               bool stallIssueOnLSQ)
    : SbCore(_name, _cid, numctxts, _l1d, priority, issueWidth,
             mispredictPenalty, loadBufferEntries, storeBufferEntries,
             startThreads),
      stallIssueOnLSQ(stallIssueOnLSQ)
{
    rob = new ReorderBuffer(72, numctxts);
    loadStoreQueue = new ReorderBuffer(32, numctxts);

    ctxts_.clear(); // HACK: Sbcore constructor already initializes these

    for (uint32_t i = 0; i < numctxts; i++)
        ctxts_.push_back(new CoreContextOOO(i, issueWidth, rob, loadStoreQueue, stallIssueOnLSQ));
}

void OoOCore::initStats(AggregateStat* parentStat) {
    SbCore::initStats(parentStat);

    extraSlots.init("extraSlots", "Extra Slots");
    coreStats->append(&extraSlots);
}

void OoOCore::issueHead(ThreadID tid) {
    CoreContext* ctx = context(tid);
    advanceCurCycle(ctx);
    SbCore::issueHead(tid);
}

uint8_t OoOCore::selectIssuePort(DynUop* uop, uint64_t& issueCycle) {
    return uop->portMask;
}

void OoOCore::issueOne(CoreContext* issuectx) {

    DynUop* uop = issuectx->nextUop();
	assert(!issuectx->isBlocked);

    // Not modelling anything before issue.
    // We assume replicated non-empty uop queues. (Same as in SbCore)
	curCycleIssuedUops++;

	// Not modelling RF Read Stalls. TODO

	auto& regScoreboard = (issuectx->regScoreboard);
	uint64_t cOps;
        //FIXME(victory): Deduplicate this logic with that in CoreContext::computeNextIssueCycle()
	if (uop->type == UOP_MAGIC_OP && issuectx->magicOp >= MAGIC_OP_TASK_ENQUEUE_BEGIN && issuectx->magicOp < MAGIC_OP_TASK_ENQUEUE_END){
		// TODO Can these go in ROB? Should we wait until the pipeline drains?
		REG regs[] = {REG_RDI, REG_RSI, REG_RDX, REG_R8, REG_R9, REG_R10, REG_R11, REG_R12};
		const bool noHint = issuectx->magicOp & EnqFlags::NOHINT;
		const bool sameHint = issuectx->magicOp & EnqFlags::SAMEHINT;
		const bool sameTask = issuectx->magicOp & EnqFlags::SAMETASK;

		uint32_t numArgs = issuectx->magicOp & 0x0f;
		uint32_t numRegs = 1 + numArgs + (!sameTask) + !(sameHint || noHint);
		uint64_t max = 0;
		//DEBUG("\tenqueue regs: %d",numRegs);
		for (uint32_t i=0;i<numRegs;i++){
			max = MAX(max,regScoreboard[regs[i]]);
		}
		cOps = max;
	} else {
		uint64_t c0 = uop->rs[0] ? regScoreboard[uop->rs[0]] : 0;
		uint64_t c1 = uop->rs[1] ? regScoreboard[uop->rs[1]] : 0;
		cOps = MAX(c0, c1);
	}

	uint64_t c2 = rob->minAllocCycle(issuectx->id);
	uint64_t c3 = curCycle;

	// Model RAT + ROB + RS delay between issue and dispatch
	uint64_t dispatchCycle = MAX(cOps, MAX(c2, c3) + (DISPATCH_STAGE - ISSUE_STAGE));

    insWindow.schedule(curCycle, dispatchCycle, uop->portMask, uop->extraSlots);
    extraSlots.inc(uop->extraSlots);
    assert(curCycle == c3);

    uint64_t commitCycle;
    uint64_t lat = uop->lat;
    uint32_t uopsRead = 1;

    if (curTask && !curTask->ts.hasTieBreaker()) unsetTbIssues.inc();

    issuectx->lastIssuedCycle = curCycle;
    bool wrongPathIns = curCycle < issuectx->runWrongPathUntil;
    if (wrongPathIns) {
        // Does not simulate LSQ pressure
        commitCycle = dispatchCycle + 1; // Rough estimate. Doesn't know for sure unless we do wrong path
        // ROB entry can be freed when the branch is resolved
        rob->markRetire(std::min(issuectx->runWrongPathUntil, commitCycle), issuectx->id);
        wrongPathIssues.inc();
    } else {
        // LSU simulation
        // NOTE: Ever-so-slightly faster than if-else if-else if-else
        switch (uop->type) {
            case UOP_GENERAL:
                commitCycle = dispatchCycle + uop->lat;
                break;

            case UOP_LOAD:
                {
                    lat = issuectx->accessLatency;
                    if (issuectx->curInsLoads && (issuectx->curInsStores==1)) {
                        // In a situation where an ins does a store and a load,
                        // both accesses take the full latency due to the different permissions reqd.
                        // But in a real implementation the load could be done with the write perms
                        // Hence coalesce the two.
                        // Assumes the actual op carried out between the two does not have structural conflicts
                        DEBUG("\tst after ld");
                        DynUop* nextUop = nullptr;
                        do {
                            nextUop = &(issuectx->curBbl->oooBbl->uop[issuectx->curIndex + uopsRead]);
                            uopsRead++;
                            if (nextUop->type != UOP_STORE) {
                                lat+= nextUop->lat;
                            } else {
                                // accessLatency already adjusted
                            }
                        } while (nextUop->type != UOP_STORE &&
                                issuectx->curIndex+uopsRead < issuectx->curBbl->oooBbl->uops);
                    }
                    issuectx->accessLatency = 0;
                    issuectx->curInsLoads = 0;
                    issuectx->curInsStores = 0;

                    // Wait for all previous store addresses to be resolved
                    dispatchCycle = MAX(lastStoreAddrCommitCycle+1, dispatchCycle);
                    if (stallIssueOnLSQ) {
                        assert(curCycle >= loadStoreQueue->minAllocCycle(issuectx->id));
                    } else {
                        dispatchCycle = MAX(loadStoreQueue->minAllocCycle(issuectx->id), dispatchCycle);
                    }

                    // Not modelling st-ld forwarding. TODO
                    commitCycle = dispatchCycle + lat;
                    loadStoreQueue->markRetire(commitCycle, issuectx->id);
                }
                break;

            case UOP_STORE:
                {
                    lat = issuectx->accessLatency;
                    issuectx->accessLatency = 0;
                    issuectx->curInsLoads = 0;
                    issuectx->curInsStores = 0;
                    // Wait for all previous store addresses to be resolved (not just ours :))
                    dispatchCycle = MAX(lastStoreAddrCommitCycle+1, dispatchCycle);
                    if (stallIssueOnLSQ) {
                        assert(curCycle >= loadStoreQueue->minAllocCycle(issuectx->id));
                    } else {
                        dispatchCycle = MAX(loadStoreQueue->minAllocCycle(issuectx->id), dispatchCycle);
                    }

                    commitCycle = dispatchCycle + lat;;
                    lastStoreCommitCycle = MAX(lastStoreCommitCycle, commitCycle);
                    loadStoreQueue->markRetire(commitCycle, issuectx->id);
                }
                break;

            case UOP_STORE_ADDR:
                commitCycle = dispatchCycle + uop->lat;
                lastStoreAddrCommitCycle = MAX(lastStoreAddrCommitCycle, commitCycle);
                break;
            case UOP_MAGIC_OP:
                commitCycle = curCycle + lat;
                break;
            case UOP_DEQUEUE: // Note: IPC1 core does not count dequeue cycles
                lat = 0;
                commitCycle = curCycle + lat;
                break;
            //case UOP_FENCE:  //make gcc happy
            default:
                assert((UopType) uop->type == UOP_FENCE);
                commitCycle = dispatchCycle + uop->lat;
                // info("%d %ld %ld", uop->lat, lastStoreAddrCommitCycle, lastStoreCommitCycle);
                // force future load serialization
                lastStoreAddrCommitCycle = MAX(commitCycle, MAX(lastStoreAddrCommitCycle, lastStoreCommitCycle + uop->lat));
                // info("%d %ld %ld X", uop->lat, lastStoreAddrCommitCycle, lastStoreCommitCycle);
        }

        regScoreboard[uop->rd[0]] = commitCycle;
        regScoreboard[uop->rd[1]] = commitCycle;

        bool BblInsRemaining = issuectx->advance(commitCycle, uopsRead, true);
        if (!BblInsRemaining){
            // Run Branch Predictor
            if (issuectx->branchPc){
                branches.inc();
                bool correct = branchPred.predict(issuectx->branchPc, issuectx->branchTaken);
                issuectx->ctxBranches.inc();
                if (!correct) {
                    // Ideally this should depend on when the branch direction is actually known. i.e regScoreboard[branchReg]
                    issuectx->runWrongPathUntil = curCycle + mispredictPenalty;
                    issuectx->ctxMispredicts.inc();
                    mispredicts.inc();
                }
                DEBUG_BRANCH("[%d] issue branch %lx", cid, branchPc);
                issuectx->branchPc = 0;
            }
        }
        rob->markRetire(commitCycle, issuectx->id);
    }

    if (priority != FIXED)
        issuectx->completion_times.push(commitCycle);

    issuectx->abortRespCycle = MAX(issuectx->abortRespCycle, commitCycle); // when can the task finish

	if (wrongPathIns) {
	    DEBUG("[core-%d][ctx-%d] [prio-%d] wrong path issue:%5ld dispatch:%5ld commit:%5ld | cOps:%5ld c2:%5ld %d",
	                     cid, issuectx->id, issuectx->priority, curCycle, dispatchCycle, commitCycle,
	                     cOps, c2, uop->extraSlots);
	} else {
        DEBUG("[core-%d][ctx-%d] [prio-%d] %d %d (%3d %3d) <- (%3d %3d) lat:%4ld | issue:%5ld dispatch:%5ld commit:%5ld | cOps:%5ld c2:%5ld %d",
                cid, issuectx->id, issuectx->priority, uop->type, uop->portMask, uop->rd[0], uop->rd[1], uop->rs[0], uop->rs[1], lat,curCycle, dispatchCycle, commitCycle,
                cOps, c2, uop->extraSlots);
	}

    issuectx->nextIssueCycle = std::max(issuectx->nextIssueCycle, rob->minAllocCycle(issuectx->id));

	updateStatsOnIssue(issuectx, curCycle, curCycleIssuedUops - 1, wrongPathIns);

	if (priority != FIXED) {
		recalculatePriorities(curCycle);
	}

    lastPortMask = portMask;
}

void OoOCore::advanceCurCycle(CoreContext* ctx) {
    if (curCycle < ctx->getNextIssueCycle()) {
//        if (ctx->getNextIssueCycle()-curCycle > 20) {
//            DEBUG("[core-%d][ctx-%d] Head longAdvance curcycle:%ld cycle:%ld ",cid, ctx->id, curCycle, ctx->getNextIssueCycle());
//            for (CoreContext* cc: ctxts_){
//                DEBUG("next issue %d %ld",ctx->id,cc->getNextIssueCycle());
//            }
//        }
        insWindow.longAdvance(curCycle, ctx->getNextIssueCycle());

        if (curCycleIssuedUops) {
            for (CoreContext* c : ctxts_)
                c->updateCycleBreakdown(CoreContext::State::BUSY,
                                        issueWidth - curCycleIssuedUops);
        }
        curCycleIssuedUops = 0;
    }
}


void OoOCore::join(uint64_t cycle, ThreadID tid) { // Unchanged from SbCore so far // TODO remove if continues to be so
    CoreContext* ctx = context(tid);

    ctx->isBlocked = false;
    DEBUG("[core-%d][ctx-%d] join curcycle:%ld cycle:%ld %ld",cid, ctx->id, curCycle, cycle, ctx->nextBbl? ctx->nextBbl->addr : 0);
    if ( ctx->nextIssueCycle < cycle) ctx->nextIssueCycle = cycle;
    assert(cycle >= statsCycle);
    // events must be increasing in time
    assert(cycle >= ctx->lastStallEventCycle);
    if (ctx->nextIssueCycle>0) {
        // A join or a leave at cycle x means event occurs in the boundary of x-1/x
        // i.e An issue can occur on x. update should only be till x-1
        ctx->stallEvents.push_back({cycle - 1,
                                    CoreContext::State::STALL_SB,
                                    CoreContext::taskType(ctx->curTask)});
        ctx->lastStallEventCycle = cycle;
    }

    // advanceCurCycle();
    updateActiveThread();
}

void OoOCore::leave(Core::State nextState, ThreadID tid) {
    CoreContext* ctx = context(tid);
    DEBUG("[core-%d][ctx-%d] blocked due to %d, curCycle:%ld, nextIssueCycle:%d, %ld",
          cid, ctx->id, nextState, curCycle, ctx->nextIssueCycle, ctx->nextBbl? ctx->nextBbl->addr : 0);
    ctx->isBlocked = true;
    //ctx->blockedSince = ctx->nextIssueCycle;
    CoreContext::State newState;
    switch(nextState){
        case Core::State::STALL_EMPTY: newState = CoreContext::State::STALL_EMPTY; break;
        case Core::State::STALL_RESOURCE: newState = ctx->curTask ? CoreContext::State::STALL_TQ : CoreContext::State::STALL_CQ; break;
        case Core::State::STALL_THROTTLE: newState = CoreContext::State::STALL_THROTTLE; break;
        case Core::State::EXEC_TOABORT: newState = CoreContext::State::EXEC_TOABORT; break;
        case Core::State::IDLE: newState = CoreContext::State::IDLE; break;
        case Core::State::STALL_NACK: newState = CoreContext::State::NACKED; break;
        default: assert(false);
    }

    // advanceCurCycle();

    assert(ctx->nextIssueCycle >= ctx->lastStallEventCycle);
    if (ctx->nextIssueCycle > 0) {
        // When we join() this thread in the future, we will increase
        // ctx->nextIssueCycle to at least statsCycle.
        // Increasing it now has no effect while this thread is blocked and
        // allows simpler assertions in drainEventsIntoCycleBreakdown().
        ctx->nextIssueCycle = std::max(statsCycle, ctx->nextIssueCycle);
        ctx->stallEvents.push_back({ctx->nextIssueCycle - 1,
                                    newState,
                                    CoreContext::taskType(ctx->curTask)});
        ctx->lastStallEventCycle = ctx->nextIssueCycle;
    }

    updateActiveThread();
    return;
}

bool OoOCore::access(Address addr, uint32_t size, bool isLoad, uint64_t* latency, ThreadID tid) {
    CoreContext* ctx = context(tid);
    curTask = ctx->curTask;
    curCtx = ctx;
    cd->setContext(ctx->id);
    uint64_t cycleData = curCycle;
    uint64_t cycleConflicts;
    bool ret = (this->*(isLoad? &Core::access<true, true> : &Core::access<false, true>))(addr, size, ctx->thread, &cycleData, &cycleConflicts);
    cd->clearContext();

    *latency = std::max(cycleData, cycleConflicts) - curCycle;
    return ret;
}

void OoOCore::finishTask(ThreadID tid) {
    CoreContext* ctx = context(tid);
    ctx->nextIssueCycle = std::max(curCycle, ctx->nextIssueCycle);
    DEBUG("[core-%d][ctx-%d] finish task curcycle:%ld abort:%ld ",cid, ctx->id, curCycle, ctx->abortRespCycle);
    SbCore::finishTask(tid);
}

std::tuple<CoreContext*, bool> OoOCore::getNextIssueCtx(){
    // advance cycle because we ran out of issue slots
    if (curCycleIssuedUops >= issueWidth) {
        // no wasted slots, yay
        curCycleIssuedUops = 0;
        insWindow.advancePos(curCycle);
    }

    // advance cycle because instruction window became full
    if (insWindow.advanceTillNotFull(curCycle)) {
        if (curCycleIssuedUops) {
            for (CoreContext* c : ctxts_)
                c->updateCycleBreakdown(CoreContext::State::BUSY,
                                        issueWidth - curCycleIssuedUops);
        }
        curCycleIssuedUops = 0;
    }
    CoreContext* ctx;
    bool canIssue;
    std::tie(ctx, canIssue) = SbCore::getNextIssueCtx();

    if (canIssue)
        advanceCurCycle(ctx);

    return std::tuple<CoreContext*, bool>(ctx, canIssue);
}

void OoOCore::updateStatsOnIssue(CoreContext *ctx, uint64_t cycle, uint64_t slot, bool wrongPath) {
    DEBUG_STATS("Updating ooo_core stats on issue from ctx %d, cycle %ld, slot %ld, wrongPath %d",
                ctx->id, cycle, slot, wrongPath);

    assert(cycle >= statsCycle); // monotonically increasing

    for (CoreContext* c : ctxts_) {
        // process join() leave() events only at the start of a complete cycle
        if (slot == 0)
            c->drainEventsIntoCycleBreakdown(statsCycle, cycle);

        if (c != ctx)
            c->updateCycleBreakdown(CoreContext::State::OTHER_CTX, 1);
        else if (wrongPath)
            c->updateCycleBreakdown(CoreContext::State::WRONG_PATH, 1);
        else
            c->updateCycleBreakdown(CoreContext::State::ISSUED, 1);
    }

    statsCycle = cycle;
}

void OoOCore::updateActiveThread(){
    CoreContext* ctx;
    bool canIssue;
    std::tie(ctx, canIssue) = getNextIssueCtx();
    if (canIssue) advanceCurCycle(ctx);
    if (!ctx) return; // all threads blocked
    SbCore::updateActiveThread(ctx->thread);
}
