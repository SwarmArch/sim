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

#include "sim/assert.h"
#include "sim/memory/filter_cache.h"
#include "sim/memory/cache.h"
#include "sim/tsarray.h"
#include "sim/task.h"
#include "sim/collections/enum.h"
#include "sim/log.h"
#include "swarm/hooks.h"
#include "sim/conflicts/terminal_cd.h" // FIXME(mcj)

#include "sbcore.h"
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

#define PORT_0 (0x1)
#define PORT_1 (0x2)

#define OOO_PORT_0 (0x1)
#define OOO_PORT_1 (0x2)
#define OOO_PORT_2 (0x4)
#define OOO_PORT_3 (0x8)
#define OOO_PORT_4 (0x10)
#define OOO_PORT_5 (0x20)

SbCore::SbCore(const std::string& _name, uint32_t _cid,
               uint32_t numctxts, FilterCache* _l1d,
               IssuePriority priority,
               uint64_t issueWidth,
               uint64_t mispredictPenalty,
               uint32_t loadBufferEntries,
               uint32_t storeBufferEntries, uint32_t startThreads)
    : Core(_name, _cid, _l1d),
      startAvailableThreads(startThreads),
      priority(priority),
      issueWidth(issueWidth),
      statsCycle(0),
      lastPortMask(0),
      mispredictPenalty(mispredictPenalty){
    fpOnTS = false;

    switch (priority) {
        case IssuePriority::ROUND_ROBIN:
            timestampPriority = false;
            taskStartPriority = false;
            break;
        case IssuePriority::TIMESTAMP:
            timestampPriority = true;
            taskStartPriority = false;
            break;
        case IssuePriority::START_ORDER:
            timestampPriority = false;
            taskStartPriority = true;
            break;
        case IssuePriority::SAI:
            timestampPriority = true;
            taskStartPriority = true;
            break;
        case IssuePriority::TS_FP:
            timestampPriority = true;
            taskStartPriority = false;
            fpOnTS = true;
            break;
        case IssuePriority::ICOUNT:
            timestampPriority = false;
            taskStartPriority = false;
            icount = true;
            break;
        case IssuePriority::TS_ICOUNT:
            timestampPriority = true;
            taskStartPriority = false;
            icount = true;
            break;
        case IssuePriority::FIXED:
            break;  // satisfy compiler
    }

    auto initBuffer = [](CycleQueue*& buf, uint32_t entries) {
        if (entries) {
            buf = new CycleQueue();
            for (uint32_t i = 0; i < entries; i++) buf->push(0);
        } else {
           buf = nullptr;
        }
    };

    initBuffer(loadBuffer, loadBufferEntries);
    initBuffer(storeBuffer, storeBufferEntries);

    for (uint32_t i = 0; i < numctxts; i++) {
        ctxts_.push_back(new CoreContext(i, issueWidth, loadBuffer, storeBuffer));
    }
    thread2ctxt_.resize(ctxts_.size() + 1, nullptr);

    switch (issueWidth) {
    case 1: fullPortMask = PORT_0; break;
    case 2: fullPortMask = PORT_0 | PORT_1; break;
    case 4: fullPortMask = OOO_PORT_0 | OOO_PORT_1 | OOO_PORT_2 | OOO_PORT_3 | OOO_PORT_4 | OOO_PORT_5; break;
    }

    portMask = fullPortMask;
}

bool SbCore::load(Address addr, uint32_t size, ThreadID tid) {
    CoreContext* ctx = context(tid);
    DEBUG("\tld addr:0x%lx", addr);
    bool isAck = access(addr, size, true, &(ctx->accessLatency), ctx);
    if (!isAck) return false;
    if (ctx->accessLatency ==0) ctx->accessLatency = 2; // Should use config.get
    ctx->curInsLoads++;
    return true;
}

bool SbCore::load2(Address addr, uint32_t size, ThreadID tid) {
    CoreContext* ctx = context(tid);
    DEBUG("\tld2 addr:0x%lx", addr);
    uint64_t latency;
    bool isAck = access(addr, size, true, &latency, ctx);
    if (!isAck) return false;
    // two loads go in parallel
    ctx->accessLatency = std::max(ctx->accessLatency, latency);
    ctx->curInsLoads++;
    return true;
}

bool SbCore::store(Address addr, uint32_t size, ThreadID tid) {
    CoreContext* ctx = context(tid);
    ctx->curInsStores++;
    DEBUG("\tst addr:0x%lx %d %d", addr, ctx->curInsLoads, ctx->curInsStores);
    // FIXME. if st after load, then curCycle for store should be after
    // but here I just assume its the same.
    uint64_t latency;
    bool isAck = access(addr, size, false, &latency, ctx);
    if (!isAck) return false;
    // Store goes after load. Compensate for starting at the same time
    ctx->accessLatency += latency;
    if (ctx->accessLatency ==0) {
        assert(ctx->curInsLoads==0); // just a store
        ctx->accessLatency = 2; // Should use config.get
    }
    return true;
}

void SbCore::handleMagicOp(uint32_t op, ThreadID tid) {
    context(tid)->magicOp = op;
}

void SbCore::join(uint64_t cycle, ThreadID tid) {
    CoreContext* ctx = context(tid);

    ctx->isBlocked = false;
    DEBUG("[core-%d][ctx-%d] join curcycle:%ld cycle:%ld %ld",cid, ctx->id, ctx->nextIssueCycle, cycle, ctx->nextBbl? ctx->nextBbl->addr : 0);
    if ( ctx->nextIssueCycle < cycle) ctx->nextIssueCycle = cycle;
    assert(cycle >= statsCycle);
    // events must be increasing in time
    assert(cycle >= ctx->lastStallEventCycle);
    if (ctx->nextIssueCycle>0) {
        // A join or a leave at cycle x means event occurs in the boundary of x-1/x
        // i.e An issue can occur on x. update should only be till x-1
        ctx->stallEvents.push_back({cycle-1,
                                    CoreContext::State::STALL_SB,
                                    CoreContext::taskType(ctx->curTask)});
        ctx->lastStallEventCycle = cycle;
    }
    updateActiveThread();
    return;
}

void SbCore::leave(Core::State nextState, ThreadID tid) {
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

    assert(ctx->nextIssueCycle >= ctx->lastStallEventCycle);
    if (ctx->nextIssueCycle>0) {
        assert(ctx->nextIssueCycle >= statsCycle);
        ctx->stallEvents.push_back({ctx->nextIssueCycle - 1,
                                    newState,
                                    CoreContext::taskType(ctx->curTask)});
        ctx->lastStallEventCycle = ctx->nextIssueCycle;
    }
    updateActiveThread();
}

void SbCore::abortTask(uint64_t cycle, ThreadID tid, bool isPriv) {
    DEBUG("[%s] abort task cycle before: %lu, cycle after: %lu", name.c_str(), getCycle(), cycle);

    CoreContext* ctx = context(tid);
    if (isPriv) {
        // The task is gone, but the core continues executing priv code
        ctx->finishTask();
    } else {
        // The notion of an epoch does not make sense with multithreading,
        // since ins from other threads could have been issued in the intervening period.
        if (ctx->nextIssueCycle < cycle) ctx->nextIssueCycle = cycle;
        // Receiving an abort message does not complete the cycle. An ins can be issued the same cycle.
        updateStatsOnFinishedTask(cycle-1);
        ctx->abortTask(cycle);
        //recalculatePriorities();
        ctx->branchPc = 0;
    }

    runningTaskAborts.inc();
    cd->setContext(ctx->id);
    cd->abortTask();
    cd->clearContext();
}

void SbCore::closeEpoch() {
    return;
}

void SbCore::initStats(AggregateStat* parentStat) {
    coreStats = new AggregateStat();
    coreStats->init(name.c_str(), "Core stats");

    AggregateStat* contextStats = new AggregateStat(true);
    contextStats->init("ctx", "Core Context stats");
    coreStats->append(contextStats);
    for (CoreContext* ctx : ctxts_) ctx->initStats(contextStats);

    auto addStat = [&](Counter& cs, const char* name, const char* desc) {
        cs.init(name, desc);
        coreStats->append(&cs);
    };

    addStat(taskStarts, "taskStarts", "Tasks started");
    addStat(taskFinishes, "taskFinishes", "Tasks finished");
    addStat(runningTaskAborts, "runningTaskAborts", "Tasks aborted while running");

    addStat(unsetTbIssues, "unsetTbIssues", "issues from tasks with unset tb");
    addStat(dualIssueCycles, "dualIssueCycles", "cycles where both issue slots were used");

    addStat(branches, "branches", "Total branches");
    addStat(mispredicts, "mispredicts", "Mispredicted branches");

    wrongPathIssues.init("wrongPathIssues", "Wrong path issues");
    coreStats->append(&wrongPathIssues);

    addStat(sbInstrs, "instrs", "Total instructions");

    parentStat->append(coreStats);
}

void SbCore::waitTillMemComplete(ThreadID tid){
    CoreContext* ctx = context(tid);
    // Block ctx until abortRespCycle ( effectively treat the difference as sb stall)
    if ( ctx->nextIssueCycle < ctx->abortRespCycle) {
        DEBUG("[core-%d] [ctx-%d] wait advance %ld %ld",cid, ctx->id, ctx->abortRespCycle, curCycle);
        ctx->nextIssueCycle = ctx->abortRespCycle;
    }
    updateActiveThread();
}


bool SbCore::access(Address addr, uint32_t size, bool isLoad, uint64_t* latency,
                    CoreContext* ctx) {
    curTask = ctx->curTask;
    curCtx = ctx;
    uint64_t issueCycle = MAX(getCycle(), ctx->getNextIssueCycle());
    // [mcj] FIXME(mcj) a sign the interface to cd is borked
    cd->setContext(ctx->id);
    uint64_t cycleData = issueCycle;
    uint64_t cycleConflicts;
    bool ret = (this->*(isLoad? &Core::access<true, true> : &Core::access<false, true>))(addr, size, ctx->thread, &cycleData, &cycleConflicts);
    cd->clearContext();
    curCtx->abortRespCycle = MAX(curCtx->abortRespCycle, cycleConflicts);
    *latency = std::max(cycleData, cycleConflicts) - issueCycle;
    return ret;
}

// TODO(dsm): Why a tuple? Just return nullptr if you can't issue?
// [maleen] Because you still need to know which thread to unblock
std::tuple<CoreContext*, bool> SbCore::getNextIssueCtx(){
    CoreContext* minContext = nullptr;
    uint64_t minIssueCycle = UINT64_MAX;
    for (CoreContext* ctx : ctxts_){
        if (ctx->isBlocked || ctx->thread == MAX32_T) continue;
        // peek at the next ins of all threads before deciding which one to issue from
        if (!ctx->hasNextUop()) {
            return std::make_tuple(ctx, false);
        }
        // If control reaches here, it means at least one thread could be issued from
        // If all of them has TS infinity, the loop would not produce an output.
        if (!minContext) minContext = ctx;

        uint64_t c = ctx->getNextIssueCycle();
        if (c <= curCycle) c = curCycle;

        DEBUG("next issue %d %ld %d",ctx->id,c,ctx->thread);
        if (c < minIssueCycle) {
            minContext = ctx;
            minIssueCycle = c;
        }
    }
    // All blocked
    if (!minContext) return std::make_tuple(nullptr, false);
    // All ins from current bbl have been issued, Although the nextBBl is known,
    // cannot issue until it becomes curBBl. Therefore let the thread run
    if (!minContext->curBbl) return std::make_tuple(minContext, false);
    return std::make_tuple(minContext, true);
}

bool SbCore::recordBbl(BblInfo* bblInfo, ThreadID tid) {
    if (tid == activeThread) {
        DEBUG("[%s][ctx-%d] NextBbl Addr 0x%lx instr %d",
              name.c_str(), context(tid)->id,
              bblInfo->addr, bblInfo->instrs);
        context(tid)->bbl(bblInfo);
        return true;
    } else {
        addWaiter(tid);
        return false;
    }
}

bool SbCore::simulate() {
    // Issue ins until
    // 1) ins beyond switch point is not known OR
    // 2) next issue is beyond switch point
    // Returns true if the active thread changed so that the current one can be blocked
    DEBUG("[%s] simulate %ld %ld", name.c_str(),
            (ctxts_[0]->curBbl || ctxts_[0]->nextBbl) ?
                ctxts_[0]->getNextIssueCycle() : -1,
            ((ctxts_.size()>1) && (ctxts_[1]->curBbl || ctxts_[1]->nextBbl)) ?
                ctxts_[1]->getNextIssueCycle() : -1);

    while (true) {
        CoreContext* issuectx;
        bool canIssue;
        std::tie(issuectx, canIssue) = getNextIssueCtx();
        if (!canIssue) {
            ThreadID oldActiveThread = activeThread;
            // Knows which thread to unblock. No need to go through getNextIssueCtx again.
            updateActiveThread(issuectx->thread);
            if (activeThread != oldActiveThread) {
                addWaiter(oldActiveThread);
                return false;
            } else {
                return true;
            }
        }
        issueOne(issuectx);
    }
    return true;
}

void SbCore::branch(Address pc, bool taken, ThreadID tid) {
    CoreContext* ctx = context(tid);
    DEBUG_BRANCH("[%d] branch %lx %d", cid,pc, taken);
    assert_msg(!ctx->branchPc, "setting branchpc 0x%lx old: 0x%lx", ctx->branchPc, pc );
    ctx->branchPc = pc;
    ctx->branchTaken = taken;
}

void SbCore::updateActiveThread(){
    CoreContext* issuectx = std::get<0>(getNextIssueCtx());
    if (!issuectx) return; // all threads blocked
    updateActiveThread(issuectx->thread);
}

void SbCore::updateActiveThread(ThreadID tid){
    //DEBUG("[core-%d] next issue from %d",getCid(), tid);
    activeThread = tid;
    assert(!context(activeThread)->isBlocked);
    nonActiveThreads.notify(activeThread);
}

// select a port to issue from, based on ports from the OoO-land decoder
// this should be one-hot, as portMask is masked based on the return value
uint8_t SbCore::selectIssuePort(DynUop* uop, uint64_t& issueCycle) {
    assert(issueWidth == 1 || issueWidth == 2 || issueWidth == 4);

    // using uop->portMask, and the issue width,
    // determine which of the ports we can issue this uop from
    uint8_t availPorts = 0;
    if (issueWidth == 1) {
        availPorts = PORT_0;
    } else if (issueWidth == 2) {
        if (uop->portMask & (OOO_PORT_0 | OOO_PORT_1 | OOO_PORT_3 | OOO_PORT_5))
            availPorts |= PORT_0;
        if (uop->portMask & (OOO_PORT_1 | OOO_PORT_2 | OOO_PORT_3 | OOO_PORT_4 | OOO_PORT_5))
            availPorts |= PORT_1;
    } else if (issueWidth == 4) {
        availPorts = uop->portMask;
    }

    // check whether we should change issueCycle due to
    // contention at ports or issue slots
    if (lastIssueCycle != issueCycle) {
        portMask = fullPortMask;
        curCycleIssuedUops = 0;
    } else if (!(portMask & availPorts) || (curCycleIssuedUops >= issueWidth)) {
        issueCycle++;
        portMask = fullPortMask;
        curCycleIssuedUops = 0;
    }

    // select a port, preferring PORT_0 for this reason from maleen:
    // "choose a port. If can issue from both chose PORT_0 because loads can
    // only go on PORT_1"
    // ffs() captures this behavior
    assert(availPorts & portMask);
    uint8_t firstAvail = __builtin_ffs(availPorts & portMask) - 1;
    return 1UL << firstAvail;
}

void SbCore::issueOne(CoreContext* issuectx) {
    DynUop* uop = issuectx->nextUop();
    assert(!issuectx->isBlocked);

    uint64_t issueCycle = MAX(getCycle(),issuectx->getNextIssueCycle());
    uint8_t selectedPort = selectIssuePort(uop, issueCycle);

    curCycleIssuedUops++;
    assert(curCycleIssuedUops <= issueWidth);

    curCtx = issuectx;
    uint64_t commitCycle = 0;
    uint64_t lat = uop->lat;
    uint32_t uopsRead = 1;

    if (curCtx->curTask && !curCtx->curTask->ts.hasTieBreaker()) {
        unsetTbIssues.inc();
    }

    issuectx->lastIssuedCycle = issueCycle;
    // core::access calls getCycle(). Hence sbcore::getCycle() should return curCycle
    curCycle = issueCycle;
    switch (uop->type) {
        case UOP_GENERAL:
        case UOP_STORE_ADDR:
            commitCycle = issueCycle + uop->lat;
            break;
        case UOP_LOAD:
            lat = issuectx->accessLatency;
            if (issuectx->curInsLoads && (issuectx->curInsStores==1)) {
                // In a situation where an ins does a store and a load,
                // both accesses take the full latency due to the different permissions reqd.
                // But in a real implementation the load could be done with the write perms
                // Hence coalesce the two.
                // Assumes the actual op carried out between the two does not have structural conflicts

                // HACK(This should not be triggered if the stores are caused by a syscall, in which case,
                // all the thousand different stores will be attached with the current ins. Filter out this
                // case by checking if (curInsStores==1))
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
            commitCycle = issueCycle + lat;
            if (loadBuffer) {
                assert(issueCycle >= loadBuffer->top());
                loadBuffer->pop();
                loadBuffer->push(commitCycle);
            }
            break;
        case UOP_STORE:
            lat = issuectx->accessLatency;
            issuectx->accessLatency = 0;
            issuectx->curInsLoads = 0;
            issuectx->curInsStores = 0;
            commitCycle = issueCycle + lat;
            if (storeBuffer) {
                assert(issueCycle >= storeBuffer->top());
                storeBuffer->pop();
                storeBuffer->push(commitCycle);
            }
            break;
        case UOP_MAGIC_OP:
            commitCycle = issueCycle + lat;
            break;
        case UOP_DEQUEUE: // Note: IPC1 core does not count dequeue cycles
            lat = 0;
            commitCycle = issueCycle + lat;
            break;
         default:
            assert((UopType) uop->type == UOP_FENCE);
            commitCycle = issueCycle + uop->lat;
    }

    if (priority != FIXED)
        issuectx->completion_times.push(commitCycle);

    lastIssueCycle = issueCycle;
    DEBUG(" [ctx-%d] [prio-%d] %d %d (%3d %3d) <- (%3d %3d) lat:%4ld | issue:%5ld commit:%5ld",issuectx->id, issuectx->priority, uop->type, uop->portMask, uop->rd[0], uop->rd[1], uop->rs[0], uop->rs[1], lat,issueCycle, commitCycle);

    updateStatsOnIssue(issuectx, issueCycle, curCycleIssuedUops - 1);

    assert(portMask & selectedPort);
    portMask &= ~selectedPort;

    bool BblInsRemaining = issuectx->advance(commitCycle, uopsRead);

    if (!BblInsRemaining){
        // Run Branch Predictor

        if (issuectx->branchPc){
            branches.inc();
            bool correct = branchPred.predict(issuectx->branchPc, issuectx->branchTaken);
            issuectx->ctxBranches.inc();
            if (!correct) {
                mispredicts.inc();
                issuectx->ctxMispredicts.inc();
                issuectx->nextIssueCycle = std::max(issuectx->nextIssueCycle, issueCycle+mispredictPenalty);
            }
            DEBUG_BRANCH("[%d] issue branch %lx", cid, branchPc);
            issuectx->branchPc = 0;
        }

    }

    for (CoreContext* ctx : ctxts_) {
        ctx->checkIfBusy(portMask, issueCycle);
    }
    if (priority != FIXED) {
        recalculatePriorities(curCycle);
    }

    lastPortMask = portMask;
    lastCycleIssuedUops = curCycleIssuedUops;
}

void SbCore::issueHead(ThreadID tid) {
    CoreContext* issuectx = context(tid);
    issuectx->updateCurBbl();
    DEBUG("[core-%d][ctx-%d] IssueHead Addr 0x%lx uops:%d mem:%d %d",
            getCid(), issuectx->id, issuectx->curBbl->addr,
            issuectx->curBbl->oooBbl->uops,
            issuectx->curInsLoads,
            issuectx->curInsStores);
    instrs += issuectx->curBbl->instrs;
    sbInstrs.inc(issuectx->curBbl->instrs);
    // Need not tid == getNextIssueCtx(),
    // Ex. When an enqueue has unblocked a ctx. getNextIssue would most likely
    // return the unblocked one, but the enqueue op needs to be issued first.

    // FIXME(dsm): Should not happen anymore; exceptions are normal aborts.
    // Added a very broad assertion below.
    //if (issuectx->curTask && issuectx->curTask->exception) return;
    assert(!issuectx->curTask || issuectx->curTask->running());
    // Since Core::access() is called through RecordLoad() ( and not through
    // issueOne()), it is safe not to issue micro-ops of the same memory
    // instruction atomically
    issueOne(issuectx);
}

void SbCore::setUnassignedCtx(ThreadID tid) {
    if (availableThreads.size() < startAvailableThreads) {
        DEBUG_THROTTLE("Thread %u available on core %u", tid, cid);
        availableThreads.push_back(tid);
    } else {
        DEBUG_THROTTLE("Thread %u unavailable on core %u", tid, cid);
        unavailableThreads.push_back(tid);
    }

    size_t base = cid * ctxts_.size();
    assert(thread2ctxt_.at(tid - base) == nullptr);
    for (CoreContext* ctx : ctxts_) {
        if (__sync_bool_compare_and_swap(&ctx->thread, MAX32_T, tid)) {
            thread2ctxt_.at(tid - base) = ctx;
            return;
        }
    }
    assert(false);
}

std::pair<bool, ThreadID> SbCore::increaseAvailableThreads() {
    DEBUG_THROTTLE("Attempting to increase number of threads for core %u", cid);
    if (unavailableThreads.size() == 0) return std::make_pair(false, INVALID_TID);
    else {
        ThreadID tid = unavailableThreads.front();
        unavailableThreads.pop_front();
        availableThreads.push_back(tid);
        return std::make_pair(true, tid);
    }
}

std::pair<bool, ThreadID> SbCore::decreaseAvailableThreads() {
    DEBUG_THROTTLE("Attempting to decrease number of threads for core %u", cid);
    assert(availableThreads.size() >= 1);
    if (availableThreads.size() == 1) return std::make_pair(false, INVALID_TID);
    else {
        assert(availableThreads.size() > 1);
        ThreadID tid = availableThreads.front();
        availableThreads.pop_front();
        unavailableThreads.push_back(tid);
        return std::make_pair(true, tid);
    }
}


uint64_t SbCore::getInstructionCount() const {
    uint64_t sum = 0;
    for (const CoreContext* ctx : ctxts_) {
        for (CoreContext::TaskType tt : get_range<CoreContext::TaskType>()) {
            auto val = ctx
                    ->allCycles
                    .element(asUInt(tt), asUInt(CoreContext::State::ISSUED));
            sum += val;
        }
    }
    return sum;
}

uint64_t SbCore::getCommittedInstrs() const {
    uint64_t sum = 0;
    for (const CoreContext* ctx : ctxts_) {
        // [mcj] Callers of this probably don't want spiller and requeuer
        // instructions
        sum += ctx->committedCycles.element(
                asUInt(CoreContext::TaskType::WORKER),
                asUInt(CoreContext::State::ISSUED));
    }
    return sum;
}

uint64_t SbCore::getAbortedInstrs() const {
    uint32_t r = asUInt(CoreContext::TaskType::WORKER);
    uint32_t c = asUInt(CoreContext::State::ISSUED);

    uint64_t sum = 0;
    for (const CoreContext* ctx : ctxts_) {
        sum += (ctx->abortedDirectCycles.element(r,c) +
                ctx->abortedIndirectCycles.element(r,c));
    }
    return sum;
}

uint64_t SbCore::getStallSbCycles() const {
    uint64_t sum = 0;
    for (const CoreContext* ctx : ctxts_) {
        for (CoreContext::TaskType tt : get_range<CoreContext::TaskType>()) {
            auto val = ctx
                    ->allCycles
                    .element(asUInt(tt), asUInt(CoreContext::State::STALL_SB));
            // The "none" worker type shouldn't have any SB stalls
            assert(tt != CoreContext::TaskType::NONE || val == 0);
            sum += val;
        }
    }
    return sum;
}

void SbCore::startTask(const TaskPtr& task, ThreadID tid) {
    CoreContext* ctx = context(tid);
    DEBUG("[core-%d][ctx-%d] starttask %s",cid, ctx->id, task->toString().c_str());
    ctx->startTask(task);
    if (priority != FIXED) {
        recalculatePriorities(curCycle);
    }
    cd->setContext(ctx->id);
    cd->startTask(task.get(), getCycle());
    cd->clearContext();
    taskStarts.inc();
}

void SbCore::finishTask(ThreadID tid) {
    CoreContext* ctx = context(tid);
    updateStatsOnFinishedTask(ctx->nextIssueCycle-1);
    ctx->finishTask();
    //recalculatePriorities();
    taskFinishes.inc();
    cd->setContext(ctx->id);
    cd->endTask(getCycle());
    cd->clearContext();
}


void SbCore::setTerminalCD(TerminalCD* _cd)  {
    Core::setTerminalCD(_cd);
    cd->clearContext();  // Core sets to 0, we leave it unset except on accesses
}


void SbCore::addWaiter(ThreadID tid) {
    DEBUG("adding waiter %d",tid);
    nonActiveThreads.addWaiter(tid);
}

void SbCore::unblockWaiter(ThreadID tid) {
    nonActiveThreads.notify(tid);
}

void SbCore::unblockWaiter() {
    nonActiveThreads.notifyOne();
}

void SbCore::recalculatePriorities(uint64_t cycle) {
    for (CoreContext* ctx : ctxts_) {
        while (!ctx->completion_times.empty()) {
            uint64_t last_commit = ctx->completion_times.top();
            if (last_commit < cycle)
                ctx->completion_times.pop();
            else
                break;
        }
    }

    std::sort(ctxts_.begin(), ctxts_.end(),
                [=](const CoreContext* a, const CoreContext* b) -> bool
                {
                    if (!a->curTask) return false;
                    if (!b->curTask) return true;
                    if (timestampPriority) {
                        if (b->curTask->ts != a->curTask->ts) {
                            return b->curTask->ts > a->curTask->ts;
                        }
                    }
                    if (taskStartPriority) {
                        return b->taskStartCycle > a->taskStartCycle;
                    }
                    if (fpOnTS){
                        return b->id > a->id;
                    }
                    if (priority==ICOUNT){
                        return b->completion_times.size() > a->completion_times.size();
                    }
                    return b->lastIssuedCycle > a->lastIssuedCycle;
                });
    //DEBUG("recalculating priorities");
    for (uint32_t i=0;i<ctxts_.size();i++){
        ctxts_[i]->priority = i;
        //DEBUG("%d %s %ld %d",i,
        //        ctxts_[i]->curTask ? ctxts_[i]->curTask->toString().c_str() : "noTask",
        //        ctxts_[i]->lastIssuedCycle,
        //        ctxts_[i]->isBlocked);
    }

}

void SbCore::updateStatsOnIssue(CoreContext* ctx, uint64_t cycle, uint64_t slot, bool wrongPath) {
    // DEBUG_STATS("Updating sbcore stats on issue from ctx %d, cycle %ld",
    //             ctx->id, cycle);

    // Only update stats at issue. If we need to keep track of the priority level of each task at any moment of interest,
    // we should only update stats when the priority level changes, and that happens on each issue. (True it also happens on start task,
    // but start task should immediately followed by the first issue, so we can afford not to do it)

    assert(cycle >= statsCycle); // monotonically increasing

    //FIXME(victory): Ugh, why is the majority of this function's code duplicated
    //                between OoO and SbCore, and what does it mean that dualIssuesCycles
    //                is only incremented for SbCores and not OoO cores?
    if (issueWidth == 2 && curCycleIssuedUops == 2)
        dualIssueCycles.inc();

    for (CoreContext* c : ctxts_) {
        // process join() leave() events only at the start of a complete cycle
        if (slot == 0) {
            // complete partially recorded cycles
            if (lastPortMask != 0)
                c->updateCycleBreakdown(CoreContext::State::BUSY, issueWidth - lastCycleIssuedUops);

            // [mcj] This DEBUG was borked before; what if events was empty?
            // DEBUG_STATS(" [ctx-%d] event %ld %ld", c->id,
            //             c->events.empty(), c->events.front().first);
            c->drainEventsIntoCycleBreakdown(statsCycle, cycle);
        }

        if (c ==ctx)
            c->updateCycleBreakdown(CoreContext::State::ISSUED, 1);
        else
            c->updateCycleBreakdown(CoreContext::State::OTHER_CTX, 1);
    }

    statsCycle = cycle;
    // [mcj] There has to be another way to infer totalStatsCycles that doesn't
    // require a variable exclusively used for DEBUG.
    // DEBUG_STATS("[%d] stats %ld %ld %ld", cid,(statsCycle+1)*issueWidth,
    //             totalStatsCycles,
    //             ((statsCycle+1)*issueWidth - totalStatsCycles));
}

void SbCore::updateStatsOnFinishedTask(uint64_t cycle){
    DEBUG_STATS("Updating sbcore stats on finish in cycle %ld", cycle);
    // There is a fundamental reason why updating stats on every task finish is impossible with multithreading.
    // tasks can be aborted in the future. When that happens, it is impossible to know what priority level the aborted task is
    // at the moment of its abort.
    // With MT, this would be committed/aborted breakdown is only valid for issues. (and yes, there can be committed to-abort)
    if (ctxts_.size() > 1) return;

    // Not implemented yet
    if (issueWidth != 1) return;

    assert(statsCycle <= cycle);
    for (CoreContext* c : ctxts_){
        //if (portMask == (PORT_0 | PORT_1)) { // process join() leave() events only at the start of a complete cycle
            if (lastPortMask != 0) {
                // complete the partially recorded cycle
                c->updateCycleBreakdown((c->busyCycle == statsCycle)
                                            ? CoreContext::State::BUSY
                                            : c->stallReason,
                                        1);
            }
            c->drainEventsIntoCycleBreakdown(statsCycle, cycle, true);
        //}
    }
    statsCycle = cycle;
    DEBUG_STATS("[%d] update stats %d", cid, statsCycle);
}

bool SbCore::isAvailableThread(ThreadID tid) const {
    return (std::find(availableThreads.begin(), availableThreads.end(), tid) !=
            availableThreads.end());
}
