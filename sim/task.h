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

/* Task-based speculation */
#pragma once

#include <array>
#include <memory>
#include <numeric>
#include <set>
#include <sstream>
#include <stdint.h>
#include <utility>
#include <vector>

#include "sim/alloc/alloc.h"
#include "sim/event.h"
#include "sim/stats/stats.h"
#include "sim/taskptr.h"
#include "sim/timestamp.h"
#include "sim/types.h"
#include "sim/run_condition.h"
#include "sim/stats/breakdown_stat.h"
#include "sim/undo_log.h"

// [mcj] Ideally, the concepts of task depth profiling vs. task execution
// correctness should be teased apart, but I'd rather have depth profiling at
// least consolidated in the Task domain, rather than spanning Task/ROB.
struct DepthCounters {
    VectorCounter created;
    VectorCounter start;
    VectorCounter finish;
    VectorCounter successfulCreated;
    VectorCounter abortedCreated;
    VectorCounter abortedStart;
    VectorCounter successfulStart;

    VectorCounter startQuartile;
    VectorCounter abortedStartQuartile;
    VectorCounter successfulStartQuartile;

    static const uint32_t MAX_DEPTH = 4;

    void init(AggregateStat* parentStat);
};

class ROB;
class Event;
class TaskObserver;

struct CutTieMsg {
    TaskPtr task;
    // Child's TQ index (8--10 bits)
    uint32_t bytes() const { return 2; }
};

// [dsm] Tasks should be manipulated by the ROB exclusively; they are structs,
// it makes no sense to keep anything private To avoid restructuring all the
// code at once, sim.cpp still imports task.h. With time, task should turn into
// an opaque pointer (or make everything in it private and make the ROB a
// friend)
class Task : public std::enable_shared_from_this<Task> {
public:
    TaskPtr ptr() { return shared_from_this(); }

    struct SpillerKind { uint64_t spiller; uint64_t spillee; };

    // Address of user-space tasks that will spill untied tasks into memory and
    // reenqueue them to task queues. Set at run time
    static uint64_t ORDINARY_SPILLER_PTR;
    static uint64_t ORDINARY_REQUEUER_PTR;
    static uint64_t FRAME_SPILLER_PTR;
    static uint64_t FRAME_REQUEUER_PTR;
    static std::vector<SpillerKind> customSpillers;

    ThreadID runningTid;

    // [mcj] HACK This circular dependency is unfortunate and only necessary so
    // that the AbortHandler knows from which ROB this task should be
    // aborted. Alternatively the ROBArray could have a method abort, and the
    // ROBArray tracks task->ROB* mappings internally. I just want to get
    // something working, and the best strategy will be more clear when we
    // tinker more with distributing ROBs, task stealing, etc.
    ROB* container_;

    // Family relationships
    TaskPtr parent;
    std::vector<TaskPtr> children;

    // Input data
    const uint64_t taskFn;
    // FIXME [mcj] the app-level TS is constant, but the tie-breaker can be set
    // later in program execution. Clearly something about this design is off if
    // I can't state that the TimeStamp is constant.
    // [dsm] Lazy tiebreaking has turned this into a very brittle design; ts
    // should be private and accessible through the ROB only (with a public
    // ts() method that returns a const ref). The reason is that only the ROB
    // can change ts, because it uses ts to index the queues!!
    TimeStamp ts;

    // Returns the *conflict* timestamp, which should be used on all timestamp
    // comparisons made as part of conflict detection (this is the upper bound
    // ts with timetraveler tiebreaking)
    const TimeStamp& cts() const {
        // Essentially 0 for irrevocable tasks, so they will win all conflicts.
        return irrevocable? IRREVOCABLE_TS : ts;
    }

    // Returns the *lower bound* timestamp, which should be used to prioritize
    // tasks and to run the virtual time algorithm. With timetraveler, this can
    // be <= cts, and cts can be lowered to lts. With any other tiebreaking
    // scheme, lts === cts.
    const TimeStamp& lts() const {
        // Ideally, irrevocable tasks never get a tiebreaker, and only hold the
        // GVT with their app-level timestamp, to allow independent speculative
        // tasks with the same timestamp to commit as fast as possible.
        // However, if an irrevocable task has same-timestamp dependent
        // speculative tasks, it must hold the GVT with a tiebreaker to prevent
        // dependant tasks from committing.  (See ROB::notifyDependence().)
        // Since spillers/requeuers are irrevocable and do not participate in
        // conflicts (or deepen), as long as they have an app-level timestamp
        // to hold the GVT, they really do not need tiebreakers.
        assert(!(isOrdinarySpiller() || isOrdinaryRequeuer()) ||
               !ts.hasTieBreaker() || noTimestamp);
        // Frame spillers & requeuers use end-of-domain timestamps not used
        // by any other tasks and, like ordinary spillers & requeuers, never
        // need their tiebreaker dynamically assigned/updated.
        assert((isFrameRequeuer() || isFrameSpiller())
               == (ts == ts.domainMax()));
        return ts;
    }

    const std::vector<uint64_t> args;

    const uint64_t hint;
    const bool noHashHint;
    const bool nonSerialHint;
    const bool producer;
    const bool requeuer;
    const bool runOnAbort;
    const bool noTimestamp;

    // Unordered tasks indicated as using soft priority will take on the programmer-
    // specified timestamp as their soft timestamp, meaning that the tasks will be
    // dequeued in the soft timestamp order.
    const uint64_t softTs;

    // Must this task be executed speculatively? Can it be run speculatively,
    // but also non-speculatively when its parent commits and assuming perfect
    // hints? Or can this task never be run speculatively?
    const RunCondition runCond;

    bool isDeepened() const { return deepenState == DeepenState::DEEPENED; }
    bool hasSubdomain() const { return deepenState != DeepenState::NO_SUBDOMAIN; }

    UndoLog undoLog;

    // Tasks can have three types of aborts, in order of increasing priority:
    // 1. EXCEPTION aborts undo all of the task's effects, but leave it
    //    listening for dependence violations, in the EXCEPTIONED state.
    // 2. HAZARD aborts are caused by either a data or a structural hazard
    //    (i.e., a data dependence violation or a resource-constrained abort).
    //    They undo all task's effects and release its commit queue entry,
    //    leaving the task in the IDLE state.
    // 3. PARENT aborts are caused by the task's parent aborting. They
    //    completely kill the task, leaving it in the UNREACHABLE state.
    //
    // All abort types:
    // - Clear the task's write set
    // - Undo the task's immediate writes
    // - Launch PARENT aborts on all their children
    //
    // Aborts events are delayed into two parts:
    // 1. Start Abort
    //    - Relinquish TSB slot if not yet sent, OR
    //    - Launch PARENT aborts
    //    - Relinquish the core from a running task
    //    - Keep the task's commit queue slot and write set intact
    // 2. Finish Abort once all children have ACKed their abort
    //    - Clear the task's write set
    //    - Relinquish the commit queue slot
    // Because of delayed events, a task may have multiple pending Finish Aborts
    // (e.g., an EXCEPTION abort at cycle 10, a HAZARD abort at cycle 15, and a
    // PARENT
    // abort at cycle 20). Pending aborts follow two invariants:
    // - A task may have at most one pending abort per type. Earlier aborts of
    //   a given type replace the latter abort event.
    // - A task may not have a lower-priority abort at a cycle EQUAL OR HIGHER
    //   than a higher-priority abort (e.g., a HAZARD abort at cycle 15 prevents
    //   EXCEPTION aborts at cycle 15 or higher).
    enum class AbortType { EXCEPTION, HAZARD, PARENT, NUM_TYPES };
    Event* pendingStartAbort;
    Event* pendingFinishAbort;
    std::array<uint64_t, (size_t)AbortType::NUM_TYPES> minimumFinishAbortCycles;

    bool hasPendingAbort() const {
        return pendingStartAbort || pendingFinishAbort;
    }

    // As of the given cycle, is the task currently aborting?
    // (i.e. between start and finish abort events?)
    bool isAborting(uint64_t cycle) const;
    void recordStartAbortEvent(Event* ev);
    void recordFinishAbortEvent(Event* ev);
    void recordFinishAbortCycle(AbortType type, uint64_t cycle);

    AbortType abortTypeBefore(uint64_t cycle);
    uint64_t nextAbortCycle() const;

    // dsm: exceptionReason is set right before launching an exception abort,
    // and reset automatically when either the exception abort is superseded,
    // or when the task leaves the EXCEPTIONED state.
    std::string exceptionReason;

    Task(uint64_t _taskFn, TimeStamp _ts, uint64_t _hint, bool _noHash,
         bool _nonSerialHint, bool _producer, bool _requeuer,
         uint64_t _softTs, RunCondition _runCond, bool _runOnAbort,
         const std::vector<uint64_t>& _args);

    // Cannot be defined inline because unique_ptr destructor requires complete
    // type, and we're forward-declaring TaskObserver.
    // https://stackoverflow.com/q/13414652/12178985
    ~Task();

    void setDepthCounters(DepthCounters* dc);

    void tie(TaskPtr _parent);

    // dsm: Transitional methods for the ROB/TSB decoupling. Remove when
    // container goes away. Also, why is this named container and not rob?
    void setContainer(ROB* rob) { assert(!container_); container_ = rob; }
    ROB* container() const { assert(container_); return container_; }
    bool hasContainer() const { return container_; }

    // FIXME(dsm): There's no abstraction here. Just use state()
    inline bool idle() const { return state == IDLE; }
    inline bool running() const { return state == RUNNING; }
    inline bool completed() const { return state == COMPLETED; }
    inline bool exceptioned() const { return state == EXCEPTIONED; }
    inline bool unreachable() const { return state == UNREACHABLE; }

    /* Some of these functions are vestigial and only change state.
     * ROB manipulates most members directly */
    void start(ThreadID _runningTid);
    void finish();
    void commit();
    void makeIrrevocable();
    void abort(AbortType type);
    void cutTie();
    void cutTies();

    void markUnreachable(bool terminating = false) {
        if (!terminating) assert(state == IDLE);
        state = UNREACHABLE;
    }

    void markEnqueuedChildToSuperdomain() { enqueuedChildToSuperdomain = true; }

    /* Fast-forward task execution FSM */
    void ffStart(ThreadID _runningTid);
    void ffFinish();

    /**
     * Is this task's success still tied to its parent's? A task whose parent
     * has committed is untied. [We can work on the term later if necessary]
     */
    bool isTied() const { return parent != nullptr; }

    bool isOrdinarySpiller() const { return taskFn == Task::ORDINARY_SPILLER_PTR; }
    bool isOrdinaryRequeuer() const { return taskFn == Task::ORDINARY_REQUEUER_PTR; }
    bool isFrameSpiller() const { return taskFn == Task::FRAME_SPILLER_PTR; }
    bool isFrameRequeuer() const { return taskFn == Task::FRAME_REQUEUER_PTR; }
    bool isCustomSpiller() const {
        return std::any_of(
            customSpillers.begin(), customSpillers.end(),
            [this](SpillerKind sp) { return taskFn == sp.spiller; });
    }
    bool isCustomRequeuer() const { return isRequeuer() && !(isOrdinaryRequeuer() || isFrameRequeuer()); }
    bool isSpiller() const { return isOrdinarySpiller() || isFrameSpiller() || isCustomSpiller(); }
    bool isRequeuer() const { return requeuer; }
    bool isIrrevocable() const { return irrevocable; }
    bool hasEnqueuedChildToSuperdomain() const { return enqueuedChildToSuperdomain; }
    bool isProducer() const { return producer; }
    bool mustSpeculate() const { return runCond == RunCondition::MUSTSPEC; }
    bool maySpeculate() const { return runCond == RunCondition::MAYSPEC; }
    bool cantSpeculate() const { return runCond == RunCondition::CANTSPEC; }

    bool madeSpeculativeSyscall() const {
        assert(!specSyscall || running());
        assert(!specSyscall || !irrevocable);
        return specSyscall;
    }
    void setSpeculativeSyscall() {
        assert(!irrevocable);
        specSyscall = true;
    }

    /* Fractal time: domain management */
    void deepen(uint64_t maxTs);
    void undeepen();

    std::ostream& operator<<(std::ostream& os) const;
    std::string toString() const;

    void registerObserver(std::unique_ptr<TaskObserver>);
    void unregisterObserver(TaskObserver* to);

    /* Used by alloc intercept functions */
    TaskAllocCtxt allocCtxt;

    // HACK(dsm): Temporary, see AbortHandler, please kill this ASAP...
    bool abortedInTransit = false;

    inline uint32_t abortCount() const { return numAborts; }

    // Record cycles as aborted even if it was committed. Used in labyrinth
    // where self abort is implemented as committing a read-only task.
    bool recordAsAborted = false;

    // Needed for requeuers, which enqueue untied now
    uint32_t numEnqueues = 0;

  private:
    enum TaskState {
        IDLE, // from enqueue/abort till dequeue
        RUNNING, // has been dequeued, is currently being executed
        COMPLETED, // has been executed, but may still be aborted
        EXCEPTIONED,  // suffered an exception-triggered abort, may be aborted again (conserves cq entry and read set)
        UNREACHABLE,  // has been committed or aborted without requeue
    };

    // The state is only externally read, so reduce the verbosity and use the
    // boolean accessor functions below
    TaskState state;

    // No matter its speculation mode, any tasks can enter irrevocable mode
    // if, e.g., it becomes the GVT task.
    bool irrevocable;
    bool enqueuedChildToSuperdomain;

    // Because HandleSyscall does not atomically abort a task, we must prevent a
    // task that speculatively tried to run a system call from becoming
    // irrevocable. An ideal solution would instead appear in sim.cpp.
    bool specSyscall;

    // Catch if the task tries deepen or undeepen more than once
    enum class DeepenState {
        NO_SUBDOMAIN,
        DEEPENED,
        UNDEEPENED,
    } deepenState;

    typedef std::unique_ptr<TaskObserver> TaskObserverPtr;
    // dsm: Note that there are typically few observers, so map should be more
    // efficient than unordered_map
    std::map<TaskObserver*, TaskObserverPtr> observers;

    template <typename F> void visitObservers(F visitLambda) {
        // Copy elements first, since visitor may change observers set
        std::vector<TaskObserver*> oVec;
        oVec.reserve(observers.size());
        for (auto& o : observers) oVec.push_back(o.first);
        for (TaskObserver* o : oVec) visitLambda(o);
    }

    // Depth-related
    uint32_t createdDepth;
    uint32_t startDepth;
    uint32_t startQuartile;
    uint32_t numAborts;
    DepthCounters* depths;

    uint32_t currentDepth() const;
};

struct TaskTimeLTDeprioritizeIrrevocablesComparator {
    inline bool operator()(const TaskPtr& t1, const TaskPtr& t2) const {
        return t1->lts() < t2->lts() || (t1->lts() == t2->lts()
                 && !t1->isIrrevocable() && t2->isIrrevocable());
    }
};

struct TaskTimeLTComparator {
    inline bool operator()(const TaskPtr& t1, const TaskPtr& t2) const {
        return t1->lts() < t2->lts();
    }
};

struct TaskTimeGTComparator {
    inline bool operator()(const TaskPtr& t1, const TaskPtr& t2) const {
        return t1->lts() > t2->lts();
    }
};

std::ostream& operator<<(std::ostream& os, Task* t);
