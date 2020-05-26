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

/*
 * core_context.cpp
 *
 *  Created on: Feb 8, 2016
 *      Author: maleen
 */

#include "core_context.h"
#include "task_to_core_stats_profiler.h"
#include "sim/match.h"

#include "sim/sim.h" // Only to weaken the assert below

#undef DEBUG
#define DEBUG(args...) //info(args)

CoreContext::CoreContext(uint32_t i, const uint64_t issueWidth, const CycleQueue* loadBuffer, const CycleQueue* storeBuffer) :
    nextIssueCycle(0),
    runWrongPathUntil(0),
    curTask(nullptr),
    lastTaskTs(INFINITY_TS),
    curIndex(0),
    isBlocked(false),
    thread(MAX32_T),
    id(i),
    name("ctx-" + std::to_string(i)),
    priority(i),
    issueWidth(issueWidth),
    stallReason(State::STALL_SB),
    loadBuffer(loadBuffer),
    storeBuffer(storeBuffer)
{}

void CoreContext::initStats(AggregateStat* parentStat) {
    AggregateStat* contextStats = new AggregateStat();
    contextStats->init(name.c_str(), "Core context stats");

    // FIXME(mcj) de-duplicate this from Core. There is zero reason for the
    // SbCore states not to be a superset of Core's states.
    std::vector<std::string> taskNames =
            {"notask", "worker", "requeuer", "spiller", "irrevocable"};
    std::vector<std::string> stateNames =
                {"idle", "issued", "otherCtxIssue", "stallSb", "tq", "toabort",
                 "cq", "empty", "throttle", "nack", "busy", "wrongPath"
                };
    allCycles.init("cycles", "Core cycle breakdown",
            taskNames, stateNames);
    contextStats->append(&allCycles);
    committedCycles.init("committedCycles",
            "Cycles for committed tasks", taskNames, stateNames);
    contextStats->append(&committedCycles);
    abortedDirectCycles.init("abortedDirectCycles",
            "Cycles for direct aborted tasks", taskNames, stateNames);
    contextStats->append(&abortedDirectCycles);
    abortedIndirectCycles.init("abortedIndirectCycles",
            "Cycles for indirect (parent->child) aborted tasks",
            taskNames, stateNames);
    contextStats->append(&abortedIndirectCycles);

    ctxInstrs.init("instrs", "Instructions issued from this context");
    contextStats->append(&ctxInstrs);

    ctxBranches.init("branches", "Branches encountered in this context");
    contextStats->append(&ctxBranches);

    ctxMispredicts.init("mispredicts", "Mispredicted branches in this context");
    contextStats->append(&ctxMispredicts);

    parentStat->append(contextStats);
}

void CoreContext::startTask(const TaskPtr& task) {
    curTask = task;
    lastTaskTs = task.get()->ts;
    taskStartCycle = nextIssueCycle;
    TaskType tt = taskType(task);
    curTask->registerObserver(std::make_unique<TaskToCoreStatsProfiler>(
                    *curTask.get(), allCycles, committedCycles,
                    abortedDirectCycles, abortedIndirectCycles,
                    asUInt(tt)));
    if (tt == TaskType::WORKER) {
        // FIXME(mcj) HACK: worker tasks can transition into irrevocable mid-run
        // By registering a second observer, we can capture any cycles after
        // that transition.
        curTask->registerObserver(std::make_unique<TaskToCoreStatsProfiler>(
                *curTask.get(), allCycles, committedCycles,
                abortedDirectCycles, abortedIndirectCycles,
                asUInt(TaskType::IRREVOCABLE)));
    }
}


uint64_t CoreContext::getNextIssueCycle() const {
    assert(curBbl || nextBbl);
    DynUop* uop = &((curBbl?curBbl:nextBbl)->oooBbl->uop[curIndex]);
    if (uop->type == UOP_LOAD && loadBuffer) {
        return std::max(nextIssueCycle, loadBuffer->top());
    } else if (uop->type == UOP_STORE && storeBuffer) {
        return std::max(nextIssueCycle, storeBuffer->top());
    } else {
        return nextIssueCycle;
    }
}

void CoreContext::bbl(BblInfo* BblInfo){
    nextBbl = BblInfo;
    if (!curBbl) computeNextIssueCycle();
}


void CoreContext::computeNextIssueCycle(){
    assert (curBbl || nextBbl);
    assert(!isBlocked);
    DynUop* uop = &(  (curBbl?curBbl:nextBbl)->oooBbl->uop[curIndex]);
    uint64_t newIssueCycle;
    if (uop->type == UOP_MAGIC_OP && magicOp >= MAGIC_OP_TASK_ENQUEUE_BEGIN && magicOp < MAGIC_OP_TASK_ENQUEUE_END){
        REG regs[] = {REG_RDI, REG_RSI, REG_RDX, REG_R8, REG_R9, REG_R10, REG_R11, REG_R12};
        const bool noHint = magicOp & EnqFlags::NOHINT;
        const bool sameHint = magicOp & EnqFlags::SAMEHINT;
        const bool sameTask = magicOp & EnqFlags::SAMETASK;

        uint32_t numArgs = magicOp & 0x0f;
        uint32_t numRegs = 1 + numArgs + (!sameTask) + !(sameHint || noHint);
        uint64_t max = 0;
        //DEBUG("\tenqueue regs: %d",numRegs);
        for (uint32_t i=0;i<numRegs;i++){
            max = MAX(max,regScoreboard[regs[i]]);
        }
        newIssueCycle = max;
    } else {
        uint64_t c0 = uop->rs[0] ? regScoreboard[uop->rs[0]] : 0;
        uint64_t c1 = uop->rs[1] ? regScoreboard[uop->rs[1]] : 0;
        newIssueCycle = MAX(c0,c1);
    }
    nextIssueCycle = MAX(newIssueCycle, nextIssueCycle);
}

bool CoreContext::advance(uint64_t commitCycle, uint32_t uops, bool isOoOCore){
    DynUop* uop = &(curBbl->oooBbl->uop[curIndex]);
    curIndex+=uops;
	assert(curIndex <= curBbl->oooBbl->uops);
    regScoreboard[uop->rd[0]] = commitCycle;
    regScoreboard[uop->rd[1]] = commitCycle;

    if (curIndex == curBbl->oooBbl->uops ){
        curBbl = nullptr;
        curIndex =0;
        if (nextBbl) computeNextIssueCycle();
        return false;
    }
    computeNextIssueCycle();
    return true;

}

DynUop* CoreContext::nextUop() const {
    return &(curBbl->oooBbl->uop[curIndex]);
}

void CoreContext::checkIfBusy(uint8_t portMask, uint64_t issueCycle){
    bool busy;
    if (!curBbl && !nextBbl) {
        // If does not know the next instruction, assume busy.
        // May not entirely be accurate. But fixing this would be a pain
        busy = true;
    } else {
        DynUop* uop = &(  (curBbl?curBbl:nextBbl)->oooBbl->uop[curIndex]);
        busy = ((uop->portMask & portMask) ==0);
    }
    if (busy) busyCycle = issueCycle;
    nextIssueCycle = MAX(nextIssueCycle, issueCycle + (busy? 1u : 0u));
}

void CoreContext::updateCycleBreakdown(CoreContext::TaskType tt,
                                       State state, uint64_t count) {
    // For [core] sum(sum(allCycles.row)) should be equal over all cores
    // For [sbcore] sum(allCycles.row) should be equal over all rows and all cores
    // IMPORTANT: we currently don't expect a task's type to change from when it
    // starts to aborts/ends/commits.
    switch (state) {
        case CoreContext::State::STALL_CQ:
        case CoreContext::State::STALL_EMPTY:
        case CoreContext::State::STALL_THROTTLE:
            // [mcj] Apparently we can't accurately count cycles per task type
            // with multithreading. Sigh why does this have to be so hard.
            // FIXME(victory): It would be nice if the simulator handled task
            // functions containing only xchg %rdx, %rdx, rather than crashing.
            assert_msg(tt == CoreContext::TaskType::NONE
                       || ossinfo->numThreadsPerCore > 1
                       || issueWidth > 1,
                       "[%s] state %d tt %d, task function immediately dequeued?",
                       name.c_str(), state, tt);
            break;
        case CoreContext::State::EXEC_TOABORT:
            assert(tt == CoreContext::TaskType::WORKER
                   || ossinfo->numThreadsPerCore > 1
                   || issueWidth > 1);
            break;
        case CoreContext::State::STALL_TQ:
            assert(tt != CoreContext::TaskType::NONE
                   || ossinfo->numThreadsPerCore > 1
                   || issueWidth > 1);
            break;
        default: break;
    }
    allCycles.inc(asUInt(tt), asUInt(state), count);
}


void CoreContext::updateCycleBreakdown(State state, uint64_t count) {
    updateCycleBreakdown(taskType(curTask), state, count);
}


// Breakdowns for issueWidth>1 cores count in terms of (uop) issue slots, not
// core-cycles.
// We can interpret the stall events queue like a timecard in which we clock-in
// which states a core enters as it leave()s and join()s. This also means that
// events only occur on full cycle boundaries. When a core issues the first uop
// of a given cycle, accumulate the time accrued by each event in this timecard
// into the breakdown.
void CoreContext::drainEventsIntoCycleBreakdown(uint64_t lowerbound, uint64_t upperbound, bool addOne) {
    assert(lowerbound <= upperbound);
    DEBUG("[%s] Drain stall events from cycle %ld to %ld",
          name.c_str(), lowerbound, upperbound);

    while (!stallEvents.empty() && stallEvents.front().cycle < upperbound) {
        auto& stall = stallEvents.front();
        DEBUG("[%s] Stall at cycle %ld, reason %d, tt %d",
              name.c_str(), stall.cycle, stall.reason, ev.tt);

        // On superscalar or OoO cores, we might issue a uop and then leave()
        // on the same cycle, resulting in ev.cycle == statsCycle - 1.
        assert(lowerbound <= stall.cycle + 1);
        if (lowerbound < stall.cycle) {
            uint64_t cycleSlots = (stall.cycle - lowerbound) * issueWidth;
            updateCycleBreakdown(stall.tt, stallReason, cycleSlots);
            lowerbound = stall.cycle;
        }
        stallReason = stall.reason;
        stallEvents.pop_front();
    }
    assert(lowerbound <= upperbound);

    // Accrue the last event's contribution to the breakdown, up to upperbound
    if (lowerbound < upperbound) {
        uint64_t cycleSlots = (upperbound - lowerbound - 1 + addOne) * issueWidth;
        updateCycleBreakdown(stallReason, cycleSlots);
    }
}

CoreContextOOO::CoreContextOOO(uint32_t i, const uint64_t issueWidth, ReorderBuffer* rob, ReorderBuffer* lsq, bool stallonlsq) :
    CoreContext(i, issueWidth, nullptr, nullptr),
    rob(rob),
    lsq(lsq),
    stallIssueOnLSQ(stallonlsq)
{}

void CoreContextOOO::computeNextIssueCycle() {
    // Do nothing. getNextIssueCycle will do all the work
    // TODO can optimize to memoize the last computed value. Might be hard to reason
    return;
}

uint64_t CoreContextOOO::getNextIssueCycle() const {
    assert(curBbl || nextBbl)
    DynUop* uop = &((curBbl ? curBbl : nextBbl)->oooBbl->uop[curIndex]);
    uint64_t ret = std::max(nextIssueCycle, rob->minAllocCycle(id));
    if (stallIssueOnLSQ && matchAny(uop->type, UOP_LOAD, UOP_STORE) && lsq) {
        ret = std::max(ret, lsq->minAllocCycle(id));
    }
    return ret;
}
