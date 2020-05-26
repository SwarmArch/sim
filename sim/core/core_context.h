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
 * thread_context.h
 *
 *  Created on: Feb 8, 2016
 *      Author: maleen
 */

#ifndef CORE_CONTEXT_H_
#define CORE_CONTEXT_H_

#include "sim/assert.h"
#include "sim/memory/filter_cache.h"
#include "sim/memory/cache.h"
#include "sim/tsarray.h"
#include "sim/task.h"
#include "sim/collections/enum.h"
#include "sim/log.h"
#include "swarm/hooks.h"
#include "sim/core/decoder.h"
#include "core_structures.h"
#include "sim/match.h"
#include <deque>
#include <queue>
#include <vector>

extern "C" {
#include "xed-interface.h"
}
#undef DEBUG
#define DEBUG(args...) //info(args)

class TerminalCD;

// FIXME(dsm): Defined here b/c of getNextIssueCycle() dep, but this is an SbCore structure
typedef std::priority_queue<uint64_t, std::vector<uint64_t>, std::greater<uint64_t>> CycleQueue;

class CoreContext {
  public:
    // [victory] Our scripts to collect and process cycle breakdowns depend on
    // these enum values.  The scripts have unfortunately become fragmented so
    // it'd be a pain to update them.  So, let's keep these enum values fixed.
    enum class State {
        // Thread not scheduled, or thread uncaptured due to syscall.
        IDLE=0,
        // Issuing a uop from this current thread/task.
        ISSUED=1,
        // For multithreading, issuing a uop from a different thread.
        OTHER_CTX=2,
        // Have a thread/task to run, but not issuing a uop because of:
        STALL_SB=3,      // ordinary stalls for data or structural hazards
        BUSY=10,         // some particular structural hazards
        STALL_TQ=4,      // task enqueue stalls due to full queue/buffer
        NACKED=9,        // memory access stalls to avoid task aborts
        // The thread/task is mispeculating:
        EXEC_TOABORT=5,  // executing task with pending abort
        WRONG_PATH=11,   // for OoOCore, suffering a branch misprediction
        // Not able to run a task because of:
        STALL_CQ=6,      // full task commit queue
        STALL_EMPTY=7,   // no tasks available to run
        STALL_THROTTLE=8,// heuristics say available tasks should not run
    };
    enum class TaskType {NONE, WORKER, REQUEUER, SPILLER, IRREVOCABLE,
                         /*Hack:For iterating*/LAST, FIRST=NONE};

    CoreContext(uint32_t i, const uint64_t issueWidth, const CycleQueue* loadBuffer,
                const CycleQueue* storeBuffer);

    void initStats(AggregateStat*);

    void startTask(const TaskPtr&);
    void finishTask() { assert(curTask); curTask = nullptr; }

    void abortTask(uint64_t cycle) {
        curTask = nullptr;
        if (!curBbl && nextBbl) {
            // Corner case where task is aborted, after finishing
            // all its instructions but before calling finish_task()
            // Under normal circumstances after an abort, recordBbl
            // will be called. But it might not be so if the task
            // was aborted after finishing all instrs.
            // This is actually a result of mustReturn logic in SwitchThread()
            // It uses RIP to figure out if the thread has been aborted.
            // This is not accurate when RIP is already at FinishPtr (==DequePtr)
        } else {
            curBbl = nullptr;
            nextBbl = nullptr;
        }
        curIndex=0;
        accessLatency = 0;
        curInsLoads = 0;
        curInsStores = 0;
    }

    bool hasNextUop() const { return (curBbl || nextBbl); }

    virtual uint64_t getNextIssueCycle() const;
    // Returns if advance is successful. (i.e. a uop remains)
    bool advance(uint64_t commitCycle, uint32_t uops, bool isOoOCore = false);

    void bbl(BblInfo* bblInfo);
    void updateCurBbl() {
        assert(nextBbl);
        curBbl = nextBbl;
        nextBbl = nullptr;

        ctxInstrs.inc(curBbl->instrs);
    }

    DynUop* nextUop() const;
    virtual void computeNextIssueCycle();

    void checkIfBusy(uint8_t portMask, uint64_t);

    static inline TaskType taskType(const TaskPtr& t) {
        return (t.get() == nullptr) ? TaskType::NONE
             : t->isRequeuer() ? TaskType::REQUEUER
             : t->isSpiller() ? TaskType::SPILLER
             : t->isIrrevocable() ? TaskType::IRREVOCABLE
             : TaskType::WORKER;
    }
    void updateCycleBreakdown(State, uint64_t count);
    void drainEventsIntoCycleBreakdown(uint64_t lowerbound,
                                       uint64_t upperbound,
                                       // FIXME(mcj) awful hack
                                       bool addOne = false
                                       );

    uint64_t nextIssueCycle; // Only relevant if greater than Core::curCycle
    uint64_t runWrongPathUntil;

    // Task state
    TaskPtr curTask;  // not const b/c stores add to the log
    TimeStamp lastTaskTs;
    uint64_t taskStartCycle;

    BblInfo* curBbl;
    BblInfo* nextBbl;

    Address branchPc;  //0 if last bbl was not a conditional branch
    bool branchTaken;

    uint64_t magicOp;

    uint64_t regScoreboard[MAX_REGISTERS]; //contains timestamp of next issue cycles where each reg can be sourced
    uint32_t curIndex; // Index of the next uop within current Bbl.

    uint64_t accessLatency; // is >0 as long as mem access is pending
    //bool storeAfterLoad = false;
    int curInsLoads = 0;
    int curInsStores = 0;
    bool isBlocked;

    ThreadID thread;
    const uint32_t id;
    const std::string name;

    uint64_t abortRespCycle;
    uint64_t lastIssuedCycle;
    uint32_t priority;

    TableStat breakdown;

    // For Stats breakdown
    // FIXME(mcj) should the following be a pair? A struct?
    const uint64_t issueWidth;
    State stallReason; // state since last stats update
    uint64_t lastStallEventCycle = 0;
    // history since last stats update
    struct StallEvent {
        uint64_t cycle;
        State reason;
        CoreContext::TaskType tt;
    };
    std::deque<StallEvent> stallEvents;

    uint64_t busyCycle = 0;

    std::priority_queue<unsigned long,  std::vector<unsigned long>, std::greater<unsigned long> > completion_times;

    // Whereas the simple Core is able to "transition" among states and use the
    // BreakdownStat for allCycles, in the SbCore, the allCycles table is
    // manually modified. No transitions; I'm not sure if that's inherent, or a
    // current implementation detail.
    TableStat allCycles;
    TableStat committedCycles;
    TableStat abortedDirectCycles;
    TableStat abortedIndirectCycles;

    Counter ctxInstrs;
    Counter ctxBranches;
    Counter ctxMispredicts;

  private:
    void updateCycleBreakdown(CoreContext::TaskType, State, uint64_t count);

    // HACK(dsm): Load/store buffers should not be here... at least they're const
    const CycleQueue* loadBuffer;
    const CycleQueue* storeBuffer;
};


class CoreContextOOO : public CoreContext {
    public:
        CoreContextOOO(uint32_t i, const uint64_t issueWidth, ReorderBuffer* rob, ReorderBuffer* lsq, bool);

        void computeNextIssueCycle() override;
        uint64_t getNextIssueCycle() const override;

    private:
        ReorderBuffer* rob;
        ReorderBuffer* lsq;
        // If '1', a load/store is issued only if there is space on LSQ,
        // zsim implicitly assumed its 0, but this flag could be used to prevent starvation
        bool stallIssueOnLSQ;

};

#endif /* THREAD_CONTEXT_H_ */
