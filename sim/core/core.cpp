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

#include "sim/core/core.h"

#include "sim/assert.h"
#include "sim/collections/enum.h"
#include "sim/conflicts/abort_handler.h"
#include "sim/conflicts/conflict_detection.h"
#include "sim/conflicts/terminal_cd.h" // FIXME(mcj) TMI
#include "sim/core/task_to_core_stats_profiler.h"
#include "sim/log.h"
#include "sim/memory/cache.h"
#include "sim/memory/filter_cache.h"
#include "sim/stack.h"
#include "sim/rob.h" //FIXME(victory) Just for rob::Stall enum definition
#include "sim/task.h"

#undef DEBUG
#define DEBUG(args...) //info(args)

#undef DEBUG_EVENTQ
#define DEBUG_EVENTQ(args...) //info(args)

Core::Core(const std::string& _name, uint32_t _cid, FilterCache* _l1d)
    : name(_name),
      cid(_cid),
      l1d(_l1d),
      cd(nullptr),
      curCycle(0),
      instrs(0),
      epochMemCycles(0),
      epochInstrs(0),
      taskType(TaskType::NONE),
      state(State::IDLE),
      curTask(nullptr),
      allCycles([this] { return std::max(getCycle(), getCurCycle()); }),
      allInstrs([this] { return instrs + epochInstrs; }) {}

void Core::bbl(uint32_t instrs) {
    if (state == State::IDLE) return;  // Guard against instr calls while in a syscall (FIXME: our instr is wrong or libspin shoould not allow this)
    epochInstrs += instrs;
}

Core::State Core::stallToState(rob::Stall stall) {
    switch(stall) {
    case rob::Stall::EMPTY:
        return Core::State::STALL_EMPTY;
    case rob::Stall::THROTTLE:
        return Core::State::STALL_THROTTLE;
    case rob::Stall::CQ:
        return Core::State::STALL_RESOURCE;
    default:
        panic("Invalid rob::Stall");
    }
}

template <bool isLoad, bool lineCrossingsGoParallel>
bool Core::access(Address addr, uint32_t size, ThreadID tid, uint64_t* cycleData, uint64_t* cycleConflicts) {
    DEBUG_EVENTQ("task %s lineAddr: %lu, r/w: %s",
            curTask ? curTask->toString().c_str() : "none",
            addr >> ossinfo->lineBits,
            isLoad ? "read" : "write");
    assert(!curTask || curTask->runningTid == tid);

    // Handle line-crossing accs, breaking them up into multiple accs
    Address lineAddr = addr >> l1d->getLineBits();
    assert_msg(size > 0U && size <= 64U, "Unexpected memory access size: %d", size);
    const Address endLineAddr = (addr+size-1) >> l1d->getLineBits();
    if (unlikely(endLineAddr != lineAddr)) {
        assert(endLineAddr > lineAddr);
        // [maleen] IPC-1 : go sequentially; Sbcore, OoO: go in parallel
        // I'm not sure if this even occurs in our benchmarks, but seen this trigger in fluidanimate
        // victory: It certainly does happen if you choose a small line size, e.g., per-word cache lines.
        Address nextAddr = (lineAddr + 1u) << l1d->getLineBits();
        uint32_t sz1 = nextAddr - addr;
        assert(sz1 && sz1 < size);
        uint64_t cycleDataAcc = *cycleData;
        uint64_t cycleConflictsAcc = *cycleConflicts;
        bool success = access<isLoad, lineCrossingsGoParallel>(
            addr, sz1, tid, &cycleDataAcc, &cycleConflictsAcc);
        if (!success) return false; // should we set cycle* in this case? I guess not since we retry
        lineAddr++;
        while (lineAddr != endLineAddr) {
            if (lineCrossingsGoParallel) {
                cycleDataAcc = *cycleData;
                cycleConflictsAcc = *cycleConflicts;
            }
            success = access<isLoad, lineCrossingsGoParallel>(
                lineAddr << l1d->getLineBits(), 1u << l1d->getLineBits(), tid,
                &cycleDataAcc, &cycleConflictsAcc);
            if (!success) return false; // should we set cycle* in this case? I guess not since we retry
            lineAddr++;
        }
        Address finalAddr = endLineAddr << l1d->getLineBits();
        uint32_t sz2 = addr + size - finalAddr;
        assert(sz2 && sz1 + sz2 <= size);
        uint64_t cycleDataAcc2 = lineCrossingsGoParallel ? *cycleData : cycleDataAcc;
        uint64_t cycleConflictsAcc2 = lineCrossingsGoParallel ? *cycleConflicts : cycleConflictsAcc;
        success = access<isLoad, lineCrossingsGoParallel>(
            finalAddr, sz2, tid, &cycleDataAcc2, &cycleConflictsAcc2);
        *cycleData = cycleDataAcc2;
        *cycleConflicts = cycleConflictsAcc2;
        return success;
    }

    uint64_t startCycle = *cycleData;

    // Conflict-check task load/stores and for non-stack accesses
    const bool conflictCheck = needConflictCheck(addr, tid);
    assert(!conflictCheck || curTask || IsPriv(tid));
    *cycleConflicts = startCycle;
    if (conflictCheck) {
        assert_msg(!curTask || (!curTask->isSpiller() && !curTask->isRequeuer()),
                   "Spillers and requeuers shouldn't access tracked memory");
        if (IsPrivDoomed(tid)) {
            // This used to emit warns for both loads and stores. Now we let
            // the loads be silent, but panic on stores. The reason is that we
            // consider a lot of read-only regions (e.g., program code) as
            // tracked, so doomed code does several of these loads, e.g., when
            // taking indirect jumps. At the same time, a store to tracked data
            // is clearly wrong given our use cases, so we don't let it go
            // through.
            if (!isLoad) panic(
                "[%s] Not simulating access to conflict-checked 0x%lx due to "
                "priv-mode doomed tid %d",
                name.c_str(), addr, tid);
            return true;
        }
        assert(curTask);
        DEBUG("[%s] Conflict-checking %s 0x%lx size %d  by %s", name.c_str(),
              isLoad? "Ld" : "St", addr, size, curTask->toString().c_str());
        bool stall = false;
        *cycleConflicts = ossinfo->abortHandler->checkL1(
            *curTask, addr, startCycle, isLoad, stall);
        if (stall) return false;
    }

    // In parallel, access the data
    // TODO(mcj) there is a performance optimization available here: don't send
    // an all-zero timestamp, just a bit that indicates an irrevocable task is
    // telling you to abort.
    
    // Passing the task making a non-conflict-checked access
    // is not necessary because the task in a memory request
    // is only used for conflict checking, in the StreamPrefetcher
    const TaskPtr& requesterTask = conflictCheck ? curTask : nullptr;
    *cycleData = isLoad ?
            l1d->load(addr, startCycle, requesterTask) :
            l1d->store(addr, startCycle, requesterTask);
    // Once we have the line with the right permissions,
    // add it to rwsets and log it
    if (conflictCheck) {
        *cycleData = cd->recordAccess(addr, size, isLoad, *cycleData);
    }
    assert(*cycleData >= startCycle);

    DEBUG("[%s] Acc 0x%lx returns at %lu (task %p)",
          name.c_str(), addr, respCycleData, curTask.get());

    // Record timing (TODO: break abort cycles over mem cycles)
    uint64_t doneCycle = std::max(*cycleData, *cycleConflicts);

    // In SbCore, a task cannot finish until its conflict-checked
    // memory accesses have finished. The way real Swarm should work is that
    // upon a conflict, the issuer is NACKed (this has nothing to do with
    // adaptive stalling). Once conflicts are resolved, the issuer core is ACKed
    // and **the core retries the memory access**. Therefore a task cannot
    // finish until its conflict-checked accesses have finished.
    // SbCore assumes that respCycleAbort represents the true latency of a
    // conflict-checked memory access. However respCycleData might be larger
    // than respCycleAbort (since the former actually uses network contention),
    // so we should reset respCycleAbort to the max of the two, when conflict
    // checking.
    if (conflictCheck) *cycleConflicts = std::max(*cycleData, *cycleConflicts);

    epochMemCycles += doneCycle - startCycle;
    DEBUG_EVENTQ("[%s] Acc done, cycle: %lu, state: %u", name.c_str(), getCycle(), taskType);
    return true;
}

// Intantiate all variants (this file only uses two)
template bool Core::access<false, false>(Address addr, uint32_t size, ThreadID tid, uint64_t* cycleData, uint64_t* cycleConflicts);
template bool Core::access<true, false>(Address addr, uint32_t size, ThreadID tid, uint64_t* cycleData, uint64_t* cycleConflicts);
template bool Core::access<false, true>(Address addr, uint32_t size, ThreadID tid, uint64_t* cycleData, uint64_t* cycleConflicts);
template bool Core::access<true, true>(Address addr, uint32_t size, ThreadID tid, uint64_t* cycleData, uint64_t* cycleConflicts);


bool Core::needConflictCheck(Address addr, ThreadID tid) const {
    if (!IsInSwarmROI()) {
        assert(!curTask && !IsPriv(tid));
        return false;
    }
    if (isUntrackedAddress((void*)addr)) return false;

    // Stack address filtering: no conflict checks when a Swarm worker thread
    // accesses its private stack space.  This is safe because tasks are not
    // allowed to share data in this stack space (which is deallocated when
    // each task finishes).
    // Note: stackTracker only tracks the space allocated for Swarm worker
    // threads: it returns INVALID_TID for all other addresses, including in
    // ordinary stack space the main thread allocated outside of swarm::run().
    // tests/overwrite_undologs.cpp demonstrates how tasks are allowed to
    // access such pre-allocated stack space (e.g., correct_buffer).
    ThreadID addressOwner = ossinfo->stackTracker->owner(addr);
    if (addressOwner != INVALID_TID) {
        // dsm: Accesses to other thread stacks are invalid and would have
        // triggered an exception earlier on.
        assert(addressOwner == tid);

        Address stackStart = GetStackStart(addressOwner);
        if (unlikely(!stackStart))  {
            // Not inside dequeue loop
            assert(!curTask && !IsPriv(tid));
            return false;
        }
        // We need to conflict-check the data "below" the bottom of the stack,
        // which is used by pthreads for "thread-specific data" (a.k.a.
        // thread_local storage in C/C++).  Conflict checking is needed
        // because this thread-specific data may be shared among tasks.
        // Note: In x86, the stack grows toward lower addresses.
        bool isBelowStack = addr > stackStart;
        return isBelowStack;
    }

    return true;
}

bool Core::load(Address addr, uint32_t size, ThreadID tid) {
    uint64_t cycleData = getCycle();
    uint64_t cycleConflicts = getCycle();
    return access<true, false>(addr, size, tid, &cycleData, &cycleConflicts);
}
bool Core::load2(Address addr, uint32_t size, ThreadID tid) {
    uint64_t cycleData = getCycle();
    uint64_t cycleConflicts = getCycle();
    return access<true, false>(addr, size, tid, &cycleData, &cycleConflicts);
}

bool Core::store(Address addr, uint32_t size, ThreadID tid) {
    uint64_t cycleData = getCycle();
    uint64_t cycleConflicts = getCycle();
    return access<false, false>(addr, size, tid, &cycleData, &cycleConflicts);
}

void Core::closeEpoch() {
    curCycle = getCycle();
    instrs += epochInstrs;
    epochMemCycles = 0;
    epochInstrs = 0;
    DEBUG_EVENTQ("[%s] closing epoch, curCycle: %lu, instrs: %lu", name.c_str(), curCycle, instrs);
}


void Core::join(uint64_t cycle, ThreadID tid) {
    DEBUG_EVENTQ("[%s] Joining", name.c_str());
    assert(matchAny(state, State::IDLE,
                                  State::STALL_EMPTY,
                                  State::STALL_RESOURCE,
                                  State::STALL_THROTTLE,
                                  State::EXEC_TOABORT,
                                  State::STALL_NACK));
    assert(cycle >= getCycle());

    curCycle = cycle;
    // FIXME [mcj] I would like to see EXEC_TOABORT removed, which likely means
    // an overhaul of when/how abort events take place. Hopefully making
    // coherence actions events will lead to an elegant solution
    if (state == State::EXEC_TOABORT) transition(TaskType::NONE, State::EXEC);
    else transition(taskType, State::EXEC);
}

void Core::leave(Core::State nextState, ThreadID tid) {
    DEBUG_EVENTQ("[%s] Leaving", name.c_str());
    assert(state == State::EXEC);
    assert(nextState != State::EXEC);
    transition(taskType, nextState);
    closeEpoch();
}

void Core::startTask(const TaskPtr& task, ThreadID tid) {
    //assert(taskType==NONE && state==State::EXEC && !curTask);
    //assert(state==State::EXEC && !curTask);
    DEBUG("start task %s at cycle %ld", task->ts.toString().c_str(), getCycle());
    curTask = task;
    CoreContext::TaskType tt = CoreContext::taskType(task);
    transition(tt, State::EXEC);

    curTask->registerObserver(std::make_unique<TaskToCoreStatsProfiler>(
            *curTask.get(), allCycles, committedCycles,
            abortedDirectCycles, abortedIndirectCycles,
            asUInt(tt)));
    curTask->registerObserver(std::make_unique<TaskToCoreStatsProfiler>(
            *curTask.get(), allInstrs, committedInstrs,
            abortedDirectInstrs, abortedIndirectInstrs,
            asUInt(tt)));
    if (tt == TaskType::WORKER) {
        // FIXME(mcj) HACK: worker tasks can transition into irrevocable mid-run
        // By registering a second observer, we can capture any cycles after
        // that transition.
        curTask->registerObserver(std::make_unique<TaskToCoreStatsProfiler>(
                *curTask.get(), allCycles, committedCycles,
                abortedDirectCycles, abortedIndirectCycles,
                asUInt(TaskType::IRREVOCABLE)));
        curTask->registerObserver(std::make_unique<TaskToCoreStatsProfiler>(
                *curTask.get(), allInstrs, committedInstrs,
                abortedDirectInstrs, abortedIndirectInstrs,
                asUInt(TaskType::IRREVOCABLE)));
    }


    cd->startTask(curTask.get(), getCycle());

    taskStarts.inc();
}

void Core::finishTask(ThreadID tid) {
    //assert(taskType != TaskType::NONE && state == State::EXEC);
    assert(curTask);
    curTask = nullptr;
    transition(TaskType::NONE, State::EXEC);
    taskFinishes.inc();
    cd->endTask(getCycle());
}

void Core::abortTask(uint64_t cycle, ThreadID tid, bool isPriv) {
    DEBUG_EVENTQ("[%s] abort task cycle before: %lu, cycle after: %lu", name.c_str(), getCycle(), cycle);
    assert(taskType != TaskType::NONE);
    assert(curTask);
    assert(cycle >= getCycle());

    if (isPriv) {
        // Though the task is aborted, priv code continues execution as usual
        assert_msg(state == State::EXEC || state == State::STALL_RESOURCE, "Invalid state %d", state);
        transition(TaskType::NONE, state);
        curTask = nullptr;
    } else {
        // Normal abort (FIXME(dsm): This code is horrific)
        assert_msg(state == State::EXEC || state == State::EXEC_TOABORT, "Invalid state %d", state);
        // fast-forward
        uint64_t diff = cycle - getCycle();
        DEBUG_EVENTQ("[%s] epochInstrs: %lu, epochMemCycles: %lu, diff: %lu, instrs: %lu", name.c_str(), epochInstrs, epochMemCycles, diff, instrs);
        epochMemCycles += diff;          // this is not strictly precise...but ok
        assert(cycle == getCycle());

        curTask = nullptr;
        if(state != State::EXEC_TOABORT) // core will join on unblock and transition then...
            transition(TaskType::NONE, State::EXEC);
        else
            closeEpoch();                       // don't transition, but you still have to close epoch
    }

    runningTaskAborts.inc();
    cd->abortTask();
}

// TODO: Recover old stats & add more
void Core::initStats(AggregateStat* parentStat) {
    coreStats = new AggregateStat();
    coreStats->init(name.c_str(), "Core stats");
    const std::vector<std::string> taskNames =
            {"notask", "worker", "requeuer", "spiller", "irrevocable"};
    const std::vector<std::string> stateNames =
            {"idle", "exec", "execToAbort", "stallEmpty", "stallResource",
             "stallThrottle", "stallConflict"};
    allCycles.init("cycles", "Core cycle breakdown",
            taskNames, stateNames);
    coreStats->append(&allCycles);
    allInstrs.init("instrs", "Instruction breakdown",
            taskNames, stateNames);
    coreStats->append(&allInstrs);
    committedCycles.init("committedCycles",
            "Cycles for committed tasks", taskNames, stateNames);
    coreStats->append(&committedCycles);
    committedInstrs.init("committedInstrs",
            "Instrs for committed tasks", taskNames, stateNames);
    coreStats->append(&committedInstrs);
    abortedDirectCycles.init("abortedDirectCycles",
            "Cycles for direct aborted tasks", taskNames, stateNames);
    coreStats->append(&abortedDirectCycles);
    abortedIndirectCycles.init("abortedIndirectCycles",
            "Cycles for indirect (parent->child) aborted tasks", taskNames, stateNames);
    coreStats->append(&abortedIndirectCycles);
    abortedDirectInstrs.init("abortedDirectInstrs",
            "Instrs for direct aborted tasks", taskNames, stateNames);
    abortedIndirectInstrs.init("abortedIndirectInstrs",
            "Instrs for indirect aborted tasks", taskNames, stateNames);


    auto addStat = [&](Counter& cs, const char* name, const char* desc) {
        cs.init(name, desc);
        coreStats->append(&cs);
    };

    addStat(taskStarts, "taskStarts", "Tasks started");
    addStat(taskFinishes, "taskFinishes", "Tasks finished");
    addStat(runningTaskAborts, "runningTaskAborts", "Tasks aborted while running");

    parentStat->append(coreStats);
}

void Core::transition(CoreContext::TaskType nextTask, Core::State nextState) {
    assert(taskType == TaskType::NONE || nextTask == TaskType::NONE ||
           taskType == nextTask);
    switch (state) {
    case State::IDLE: assert(nextState == State::EXEC); break;
    case State::EXEC:
        assert(taskType != TaskType::NONE || (
               (nextTask != TaskType::NONE && nextState == State::EXEC) ||
               (nextTask != TaskType::NONE && nextState == State::EXEC_TOABORT) ||
               (nextTask != TaskType::NONE && nextState == State::STALL_NACK) ||
               (nextTask == TaskType::NONE &&
                matchAny(nextState, State::IDLE, State::STALL_EMPTY,
                                     State::STALL_RESOURCE, State::STALL_THROTTLE))
               ));
        assert_msg(taskType == TaskType::NONE ||
               (nextTask == TaskType::NONE && nextState == State::EXEC) ||
               (nextTask != TaskType::NONE && matchAny(nextState, State::IDLE, State::STALL_RESOURCE, State::EXEC_TOABORT, State::STALL_NACK)),
                "Invalid transition: taskType %d nextTask %d nextState %d",
                taskType, nextTask, nextState);
        break;
    case State::STALL_EMPTY:
        assert(taskType == TaskType::NONE
               && nextTask == TaskType::NONE
               && nextState == State::EXEC);
        break;
    case State::STALL_THROTTLE:
        // TODO(mcj) STALL_THROTTLE entirely replicates STALL_EMPTY conditions
        // We should de-duplicate them, while recognizing they are different
        // states for cycle-counting purposes.
        assert(taskType == TaskType::NONE
               && nextTask == TaskType::NONE
               && nextState == State::EXEC);
        break;
    case State::STALL_RESOURCE:
        if (!(taskType != TaskType::NONE && nextTask == TaskType::NONE)) {
            assert(nextState == State::EXEC);
            assert(taskType == nextTask);
        } else {
            // This is an abort of a task that was in priv-mode and the thread
            // is stalled on an enqueue. We lose the task but keep executing in
            // priv mode, and the thread stays stalled.
            assert(nextState == State::STALL_RESOURCE);
        }
        break;
    case State::EXEC_TOABORT:
        assert(nextState == State::EXEC);
        assert(taskType != TaskType::NONE);
        break;
    case State::STALL_NACK:
        assert(nextState == State::EXEC);
        assert(taskType == nextTask);
        break;
    default:
        panic("No policy yet for %d", state);
        break;
    }
    taskType = nextTask;
    state = nextState;

    // This is required because we could go through multiple transitions
    // without closing the epoch and if we get aborted in the middle, we
    // could end up setting epochInstrs = 0, even though we have transitioned
    // and updated the instruction count at the boundary of the transitions.
    // eg: EXEC_NOTASK --> EXEC_TASK : instr count is updated by 1 (say 47--48)
    //     EXEC_TASK   --> ABORT: if we didn't close the epoch at the prev.
    //                            transition, we could incorrectly set epochInstrs
    //                            to 0, even though it needs to be atleast 1.
    // Since we stall cores on pending aborts on a running task, this will
    // not cause any incorrect behavior.
    closeEpoch();

    allCycles.transition(asUInt(taskType), asUInt(state));
    allInstrs.transition(asUInt(taskType), asUInt(state));
}

void Core::setTerminalCD(TerminalCD* _cd) {
    assert(!cd);
    cd = _cd;
    cd->setContext(0);
}
