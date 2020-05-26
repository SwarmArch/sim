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

#pragma once

#include <algorithm>
#include <boost/bimap.hpp>
#include <utility>
#include "sim/assert.h"
#include "sim/commitqueue.h"
#include "sim/conflicts/abort_handler.h"
#include "sim/conflicts/conflict_resolution.h"
#include "sim/conflicts/tile_cd.h"
#include "sim/gvt_arbiter.h"
#include "sim/overflow_watcher.h"
#include "sim/robtypes.h"
#include "sim/sim.h"
#include "sim/spin_cv.h"
#include "sim/stats/stats.h"
#include "sim/stats/table_stat.h"
#include "sim/task.h"
#include "sim/taskprofilingsummary.h"
#include "sim/thread_count_controller.h"
#include "sim/timestamp.h"
#include "sim/trace.h"
#include "sim/tsb.h"
#include "sim/types.h"
#include "sim/zoom_requester.h"

#undef DEBUG
#define DEBUG(args...) //info(args)

#undef DEBUG_GVT
#define DEBUG_GVT(args...) //info(args)

namespace rob {
enum class Stall { NONE, EMPTY, THROTTLE, CQ, THROTTLE_MT /*better name?*/ };
enum class TBPolicy { ENQUEUE, DEQUEUE, LAZY };
enum class TMEvent { ENQUEUE, SPILL, STEAL };
enum class FrameState { STEADY_STATE, IN, OUT, WAIT_SS };
enum class SpillerType { ORDINARY, FRAME };
};

class TaskMapper;
typedef Port<CutTieMsg> CutTiePort;

class ROB : public SimObject,
            public Port<GVTUpdateMsg>::Receiver,
            public ROBTaskPort::Receiver,
            public CutTiePort::Receiver,
            public ChildAbortPort {
    private:
        /* FOR YE HARDWARE FOLK: The runQueue is like the instruction queue;
         * runQueue + commitQueue is the ROB.
         */

        // Has tasks that are running (RUNNING state)
        // N.B. these tasks are duplicated from those in the commitQueue:
        // * they must appear in the CQ to reserve slots in the fixed-capacity
        //   commit queue
        // * this separate queue exists for ease of access to RUNNING tasks
        //   (e.g. see lvt() for use)
        // This could be a vector or array, but we use a queue
        // 1) for fast access to the minimum timestamp, and
        // 2) for fast erase
        SelfProfilingQueue<ExecQ_ty> execQueue;

        // Has tasks that have not yet run (IDLE state).
        // * Tasks enter here when they are created and when they abort.
        SelfProfilingQueue<RunQ_ty> runQueue;
        SPinConditionVariable rqNotEmpty;
        // Whereas threads wait on the previous CV when the rq is empty, threads
        // wait on the following CV when the rq's tasks aren't runnable:
        //  1) tasks are being serialized on hints?
        //  2) tasks are being throttled for other reasons?
        // The following condition might change when
        //  1) a task finishes
        //  2) a new task arrives at the RQ
        // so threads are woken and will check (but the condition may not have
        // changed)
        SPinConditionVariable rqHasRunnable;

        // TODO the various queues need a rewrite. Consider a table that stores
        // all uncommitted tasks. Queries that we want:
        // * get min ts task that is idle
        // * get min ts task that is executing
        // * get min ts N tasks that are idle
        // * erase task by pointer
        // * insert task
        // * update task state from idle, to exec, to finished, to committed, to
        //   aborted
        // * get max task that is idle, that has no parent
        // * get min ts task that is idle, that has empty join on hint with
        //   executing tasks
        //
        // Solution? A la databases, we'd have a giant vector of unordered
        // tasks, and created indices for each application that I might want.
        // The boost multi_index container might do the trick.
        // http://www.boost.org/doc/libs/1_57_0/libs/multi_index/doc/tutorial/basics.html
        // Benefit: the collection would more accurately represent the task
        // queue as we described it in ISCA
        //
        // TODO(dsm): Though the full thing may be too heavyweight, abortQueue
        // and execQueue are strict subsets of commitQueue, and would be easily
        // encapsulated inside it.

        // Has tasks that are executing or have finished, but can't yet be
        // committed (RUNNING or COMPLETED state).
        // * Commit condition is checked on every task finish operation,
        //   and all tasks that can be committed are committed in bulk.
        SelfProfilingQueue<CommitQueue> commitQueue;
        SPinConditionVariable cqNotFull;

        // Holds tasks that have a pending abort or an exception and are in the
        // commit queue. These must hold the LVT. abortQueue is a performance
        // optimization: it avoids walking the commit queue to find the LVT.
        AbortQ_ty abortQueue;

        // Holds locally running tasks that entered priv mode but are doomed and
        // running their capsule to completion. Tasks in this queue must
        // contribute to the LVT. Because the task is doomed, it no longer
        // appears in the execQueue, so this queue is necessary to hold back the
        // LVT.
        boost::bimap<ThreadID, TaskPtr> doomedTasks;

        // taskQueueSize() is not to exceed the capacity
        // TODO wrap all of these fixed-capacity task queue concepts into their
        // own class, rather than have concerns scattered around the ROB.
        const uint32_t taskQueueCapacity;
        const uint32_t tiedCapacity;
        const uint32_t underflowThreshold;
        OverflowWatcher overflowWatch;
        FrameOverflowWatcher frameOverflowWatch;
        std::vector<TSBPort*> tsbPorts;
        std::vector<CutTiePort*> cutTiePorts;
        LVTUpdatePort* lvtUpdatePort;

        // Tied task counts --- kept for performance
        uint32_t tiedTasks;
        uint32_t tiedCQTasks;

        // lvt() is the minimum of timestamps in the runQueue and execQueue
        // GVT is the minimum of timestamps across all lvt's
        // This local copy must be updated via updateGVT()
        TimeStamp gvt;

        // SET_GVT state
        TimeStamp gvtToSet;
        SPinConditionVariable setGvtInProgress;
        // Frame ACKs require knowledge of (which)-th child port this is in the
        // GVT arbiter tree.
        //FIXME(victory): Remove this.  See comments on childIdx in
        //gvt_arbiter.h.  Note that using this brought significant mess in
        //commit 174c86ebe431416f2f9bb1371974249515be08fe which I want to undo.
        uint32_t childIdx;

        // The current state of the Fractal frame
        rob::FrameState frameState;
        uint32_t frameBaseDepth;
        TimeStamp zoomInFrameMax;

        // Tracking of tasks/threads requesting zooms to change the frame
        ZoomRequester zoomRequester;

        // Other info we need to manage the Fractal frame
        const uint32_t numCanariesInTsArray;

        // The Tile conflict detection unit holds read/write sets in a strategic
        // table layout for finished but uncommitted tasks. The ROB determines
        // when tasks commit, so the ROB notifies the TileCD when it can clear
        // out speculative task state. Soon it will be the conduit through which
        // the ROB triggers local aborts due to resource exhaustion, instead of
        // the AbortHandler.
        // TODO(mcj) better interface? The ROB shouldn't have access to the
        // entire TileCD API.
        TileCD* tileCD;
        std::vector<ChildAbortPort*> childAbortPorts;

        // Some task mappers keep internal mappping of tasks to buckets for
        // load balancing purposes. But the task mapper is invoked before the
        // task is actually created (enqueued). The ROB simply notifies the
        // task mapper once the task is created.
        TaskMapper* taskMapper;

        // Thread throttler: Throttles threads in multi-threaded configurations
        // suitably based on policy. Each ROB has its own thread throttler.
        ThreadCountController* threadCountController;

        // Application-level timestamps may form a partial order, but to avoid
        // livelock or deadlock in conflict resolution, we must assign a
        // tie-breaker to tasks with equal app-level ts.
        // At present, the tie-breaker can be set either at
        // * enqueue time OR
        // * dequeue time (roughly similar to LogTM)
        const rob::TBPolicy tieBreakPolicy;
        const bool clearTieBreakerOnAbort;

        // If two tasks have the same spatial ID, they should not be running on
        // concurrent threads as they will likely conflict.
        const bool serializeSpatiallyEqualTasks;

        // If true, tasks are only enqueued when their application timestamp
        // matches the GVT
        const bool bulkSynchronousTasks;

        // If true, performs adaptive dequeue-time throttling
        // TODO: Generalize to other adaptive throttling policies
        const bool adaptiveThrottle;

        // The following counter ensures strict increasing values for the
        // tiebreaker.
        // TODO [mcj] LogTM uses distributed, loosely-synchronized counters for
        // conflict resolution. Time-permitting we should do that.
        // dsm: We just need to use curCycle for tiebreaks... this counter
        // should not exist, but it's functionally equivalent to curCyle.
        static uint64_t tiebreakCounter;

        // Stats
        Counter enqueues;
        Counter appDequeues;
        Counter spillDequeues;  // Includes spillers and requeuers
        Counter spillers;  // Note: spillDequeues - spillers = requeuer dequeues
        VectorCounter tasksPerOrdinarySpiller;  // histogram of # tasks rm'd
        VectorCounter tasksPerFrameSpiller;  // histogram of # tasks rm'd
        RunningStat<size_t> numTasksSkippedForHintSerializationPerDeq; // # tasks skipped
        Counter stealSends;
        Counter stealReceives;
        Counter appCommits;
        Counter spillCommits;
        Counter totalAborts;
        Counter aborts;
        Counter abortsWhileRunning;
        Counter abortsForParent;
        TableStat resourceAborts;
        TableStat zoomingAborts;
        // [mcj] would rather see these counters live somewhere more relevant. I
        // would prefer that *most* of rob.h is ROB algorithms for
        // functionality; profiling shouldn't be intertwined with functional
        // logic.
        DepthCounters depthCounters;

        const uint32_t idx;

    public:
        ROB(uint32_t commitQueueCapacity, CommitQueueAdmissionPolicy,
            uint32_t taskQueueCapacity, uint32_t taskQueueOverflow,
            uint32_t removableCacheCapacity, uint32_t tiedCapacity,
            uint32_t underflowThreshold, uint32_t numCores,
            uint32_t numThreadsPerCore, rob::TBPolicy,
            bool clearTieBreakerOnAbort, bool serialSpatialIDs,
            bool bulkSynchronousTasks, bool adaptiveThrottle,
            const std::string& _name, uint32_t idx, TaskMapper*,
            ThreadCountController*, uint32_t numCanariesInTsArray);

        ~ROB() override;

        void setPorts(const std::vector<TSBPort*> ports) { tsbPorts = ports; }
        void setPorts(const std::vector<CutTiePort*> ports) { cutTiePorts = ports; }
        void setPort(LVTUpdatePort* port) { lvtUpdatePort = port; }
        void setPorts(const std::vector<ChildAbortPort*>& cap) {
            assert(childAbortPorts.empty());
            childAbortPorts = cap;
            assert(!childAbortPorts.empty());
        }

        void setTileCD(TileCD* cd) {
            assert(cd);
            assert(!tileCD);
            tileCD = cd;
        }

        uint32_t getIdx() const { return idx; }

        //FIXME(victory): Remove this interface.  See comments on childIdx in
        //gvt_arbiter.h.  Note that using this brought significant mess in
        //commit 174c86ebe431416f2f9bb1371974249515be08fe which I want to undo.
        void setChildIdx(uint32_t idx) {
            assert(childIdx == -1U);
            childIdx = idx;
            assert(childIdx != -1U);
        }

        void dumpQueues() const;

        inline size_t taskQueueSize() const {
            // RQ holds idle tasks, EQ executing tasks, and CQ completed tasks.
            // All tasks in EQ have a slot in the CQ, so are excluded from
            // computation.
            //
            // Irrevocable tasks, other than requeuers, do not need a task queue
            // slot once they start running, as no record is required. By
            // sitting in the EQ while running, irrevocable tasks still
            // participate in, the GVT protocol until they finish. But by
            // avoiding CQ entries, they do not contribute to taskQueueSize.
            //
            // However, requeuers yield if the TQ is full, so they need to hold
            // onto a TQ slot for re-enqueue. Since they don't hold a CQ entry,
            // this is implemented by adding running requeuers to the sum here.
            uint64_t runningRequeuers = std::count_if(
                    execQueue.begin(), execQueue.end(),
                    [] (const TaskPtr& t) { return t->isRequeuer(); });
            return runQueue.size()
                    + commitQueue.size()
                    + runningRequeuers
                    + queuelessDoomedTasks();
        }

        inline uint32_t untiedIdleTasks() const {
            return std::count_if(
                runQueue.begin(), runQueue.end(), [](const TaskPtr& t) {
                    return !t->isTied() && !t->hasPendingAbort();
                });
        }

        inline std::pair<uint64_t, uint64_t> getCommits() const {
            return std::make_pair(appCommits.get(), spillCommits.get());
        }

        inline std::pair<uint64_t, uint64_t> getDequeues() const {
            return std::make_pair(appDequeues.get(), spillDequeues.get());
        }

        const TimeStamp& commitQueueMedian() const;
        uint32_t cqQuartile(const TimeStamp& ts) const;

        /* Task mapper interface */
        // The task mapper needs to be notified when a new task is created
        // or when a task is spilled so it can take suitable action.
        void notifyTaskMapper(const TaskPtr& task, rob::TMEvent t);

        /* Thread throttler interface */
        void notifyToBeBlockedThread(ThreadID);

        void receive(const TaskEnqueueReq& req, uint64_t cycle) override;
        void receive(const CutTieMsg& msg, uint64_t cycle) override;

        uint64_t access(const AbortReq& req, AbortAck& ack, uint64_t) override;

#ifdef ATOMIC_GVT_UPDATES
        void receive(const GVTUpdateMsg& msg, uint64_t cycle) override {}
        void updateGVT(const GVTUpdateMsg& msg, uint64_t cycle);
#else
        void receive(const GVTUpdateMsg& msg, uint64_t cycle) override;
#endif

        /* Task queue and dequeue interface */

        /**
         * @returns a task ptr if not empty and the cq is not full
         * or nullptr and the reason for the stall
         */
        std::pair<TaskPtr, rob::Stall> dequeueTask(uint32_t tid);

        void finishTask(const TaskPtr& task) {
            DEBUG("[%d] %s finish task %s",
                  GetCurTid(), name(), task->toString().c_str());

            task->finish();
            execQueue.erase(task);

            if (task->isOrdinarySpiller() || task->isCustomSpiller()) {
                overflowWatch.releaseSpiller(task->lts().domainId());
            }
            if (task->isFrameSpiller()) {
                frameOverflowWatch.releaseSpiller();
            }

            if (task->isIrrevocable()) {
                commitTask(task);
            } else {
                // FIXME(dsm): This is overkill, but simplifies the commit
                // point, which doesn't need to look for tasks without a
                // tiebreaker (the right way to do this is to watch out for
                // cutTies)
                notifyDependence(task, task->cts());
            }

            // Since a worker task finished, some throttled thread might now be
            // eligible to execute.
            rqHasRunnable.notifyAll();
        }

        void yieldTask(TaskPtr task, uint64_t newTsApp);

        const TimeStamp& GVT() const { return gvt; }
        bool terminate() const { return gvt == INFINITY_TS; }

        void sendLVTUpdate(uint64_t epoch);

        TimeStamp lvt() const {
            TaskPtr task = lvtTask();
            TimeStamp ts = !task ? INFINITY_TS :
                           task->noTimestamp ? NO_TS :
                           task->lts();

            // The running (irrevocable) GVT-holding speculative task must have
            // a tiebreaker, as it may need to abort speculative successor
            // tasks. If the GVT-holding speculative task was passed over by the
            // GVT, then speculative succcessors could (incorrectly) commmit.
            assert_msg(tieBreakPolicy == rob::TBPolicy::LAZY
                   || ossinfo->oneIrrevocableThread
                   || task == nullptr
                   || !task->isIrrevocable()
                   || !task->mustSpeculate()
                   || task->idle()
                   || ts.hasTieBreaker(), "%s", task->toString().c_str());

            // The GVT should always correspond to a task real timestamp in the
            // system for ease of fixed-sized commitQ logic. If LVT has no
            // tiebreaker (TB) assigned, its task has yet to be dequeued,
            //
            // but we know its TB will be at least [curCycle, idx] when it is
            // dequeued, so assign a TB.
            // Cases to consider:
            // 1) lvt has a TB; no problem assignTieBreaker is idempotent
            // 2) lvt has no TB, but its TB would be at least [curCycle, idx]
            //    when dequeued.
            assignTieBreaker(ts);
            DEBUG("[%s] lvt task: %s",
                  name(), task ? task->toString().c_str() : "null");
            assert(ts >= gvt);
            return ts;
        }

        void notifyDependence(const TaskPtr&, TimeStamp depTs);

        void notifyZoomRequesterOfAbort(const ConstTaskPtr& task, ThreadID tid) {
            zoomRequester.notifyAbort(task, task->runningTid);
        }

        // Called when this task gets hit by an atomic abort cascade
        void notifyAbort(const TaskPtr& task) {
            assert(task && task->hasPendingAbort());

            DEBUG("[%s]: notify abort task %s",
                  name(), task->toString().c_str());

            assert_msg(!task->isIrrevocable(),
                       "Do not abort an irrevocable task %s gvt %s",
                       task->toString().c_str(), gvt.toString().c_str());

            if (task->running()) {
                // We block the thread running this task to avoid complexities
                // in handling the undo log if we allow this thread to proceed
                // while an abort is scheduled for the future.
                ThreadID tid = task->runningTid;
                if (!IsPriv(tid)) {
                    BlockAbortingTaskOnThread(tid);
                } else {
                    // Ensure the task participates in the virtual time protocol
                    // even after finishAbort by holding it in the doomedTasks
                    // queue.
                    // TODO(mcj) ideally this could happen in startAbort so that
                    // the task transitions from execQ to doomedTasks, but
                    // that doesn't mix well with sim.cpp calling
                    // ROB::finishDoomedExecution.
                    // TODO(mcj) Even better just ensure the task is left in the
                    // execQ, but that would require changing the queue
                    // structure of the execQ.
                    assert(IsPrivDoomed(tid));
                    assert(!doomedTasks.left.count(tid));
                    assert(!doomedTasks.right.count(task));
                    doomedTasks.insert({tid, task});
                }
            }

            aborts.inc();
            if (!task->idle()) {
                // Force a tiebreaker
                notifyDependence(task, task->cts());
                abortQueue.insert(task);
            }
        }

        // Called at the cycle at which this task would actually start aborting
        uint64_t startAbort(TaskPtr task, uint64_t cycle) {
            assert(task && task->hasPendingAbort());
            assert(!task->isIrrevocable());
            assert(isQueued(task));
            DEBUG("[%s][%ld]: startAbort task %s ", name(),cycle,
                  task->toString().c_str());

            // TODO(mcj) what is the penalty for aborting a task on a core?
            uint64_t coreReleaseCycle = cycle;
            if (task->running()) {
                abortsWhileRunning.inc();
                ThreadID tid = task->runningTid;
                if (IsPriv(tid)) {
                    // A running capsule could be stalled on a full TSB as it
                    // tries to enqueue its abort handler as tied. To guarantee
                    // forward progress, we must ensure it retries the enqueue
                    // when the surrounding task aborts, as it will subsequently
                    // enqueue untied.
                    ossinfo->tsbs[idx]->wakeup(tid);
                }

                // Once this task starts aborting, the core is available for
                // another task to run. However, this assumes concurrency in the
                // tile's conflict resolution: undo log rollback and child abort
                // messages can be concurrent with a new task starting
                // execution.
                AbortTaskOnThread(tid);
                notifyZoomRequesterOfAbort(task, tid);
                coreReleaseCycle++;
                finishTask(task);
            }

            // FIXME(mcj) fix depth counter
            uint64_t childACKsCycle = abortChildTasks(task, 1, cycle);
            return std::max(coreReleaseCycle, childACKsCycle);
        }

        // Called at the cycle at which this task is done aborting (e.g. all
        // children aborts TODO(mcj) and memory requests have ACK'd).
        void finishAbort(TaskPtr task, Task::AbortType type) {
            assert(task);
            DEBUG("[%s][%ld]: finishAbort task %s type = %d",
                  name(), getCurCycle(),
                  task->toString().c_str(), (uint32_t)type);

            assert(task->hasPendingAbort());
            assert(isQueued(task));
            // If the task was running before the abort, it should have been
            // removed from the core by ROB::startAbort().
            assert(execQueue.count(task) == 0ul && !task->running());
            // If this is a parent abort, the child should have already cut the
            // tie in ConflictResolution. That isn't required in the protocol,
            // but the expected behavior for now.
            assert(type != Task::AbortType::PARENT || !task->isTied());

            if (type == Task::AbortType::EXCEPTION) {
                // Retain commit queue slot
            } else if (task->completed() || task->exceptioned()) {
                commitQueue.erase(task);
                abortQueue.erase(task);
                if (task->isTied()) tiedCQTasks--;
                // Notify one commit-queue waiter, since any task that could
                // have evicted its way into the CQ would have already done so,
                // so there must be no predecessors of task that are waiting to
                // get into the queue. If 'task' was previously blocking others
                // from entering the queue, then it will be dequeued first,
                // preventing them from entering the queue again.
                // TODO assert that notion?
                assert(!commitQueue.full());
                cqNotFull.notifyOne();

                // Due to adaptive throttling, task aborts may clear space in
                // the cq and make rq tasks runnable
                if (adaptiveThrottle) rqHasRunnable.notifyAll();
            } else {
                assert(task->idle());
                // HAZARD aborts cannot happen to idle tasks
                assert(type == Task::AbortType::PARENT);
                // If the task is doomed, it wasn't yet restored to the
                // runQueue, so there is nothing to erase.
                if (!doomedTasks.right.count(task)) runQueue.erase(task);
            }
            totalAborts.inc();
            task->abort(type);

            if (type == Task::AbortType::EXCEPTION) {
                assert(task->lts().hasTieBreaker());
                if (task->lts() == gvt) exceptionedTaskAbort(task);
            } else if (type == Task::AbortType::HAZARD) {
                // The GVT holder should have had its tie cut at the GVT update
                assert(task->lts() != gvt || !task->isTied());
                if (clearTieBreakerOnAbort) {
                    // Keep the tiebreaker only if this is the LVT-holding task.
                    // This ensures there is a subset of aborted tasks that keep
                    // their tiebreakers, so we always make forward progress.
                    if (lvt() < task->lts()) task->ts.clearTieBreaker();
                }
                if (!doomedTasks.right.count(task)) {
                    if (task->isTied()) tiedTasks--;
                    enqueue(task);
                    if (!task->isTied()) {
                        // Needed frame spillers may have gone from 0 to 1 on
                        // enqueue.  Maybe the task is just now spillable.
                        ensureFrameSpillingCanHappen();
                        // TODO(victory): Should we also call
                        // attemptTaskQueueReservation() here or at least wake
                        // a thread blocked on rqHasRunnable or cqNotFull to
                        // also give ordinary spillers a chance to run?
                    }
                }
            } else {
                assert(type == Task::AbortType::PARENT);
                abortsForParent.inc();
            }
        }

        void finishDoomedExecution(ThreadID);

        // For unselective aborts, a way to search for any tasks that have
        // started (and possibly finished) execution
        SelfProfilingQueue<CommitQueue>& getCommitQueue() {
            return commitQueue;
        }

        TaskPtr removeUntiedTaskInRange(const TimeStamp& minTS,
                                        const TimeStamp& maxTS,
                                        bool noTimestampOnly) {
            return removeUntiedTaskImpl(0, minTS, maxTS, noTimestampOnly);
        }

        TaskPtr removeUntiedTaskFnInRange(uint64_t taskFn,
                                          const TimeStamp& minTS,
                                          const TimeStamp& maxTS) {
            return removeUntiedTaskImpl(taskFn, minTS, maxTS, false);
        }

        TaskPtr removeUntiedTaskImpl(uint64_t taskFn,
                                     const TimeStamp& minTS,
                                     const TimeStamp& maxTS,
                                     bool noTimestampOnly);

        TaskPtr removeOutOfFrameTasks();

        TaskPtr stealTask(bool lifo) ;

        void enqueueStolenTask(const TaskPtr& t);

        void initStats(AggregateStat* parentStat);

    public:
        // Should this be private? Needed by TSB
        static void assignTieBreaker(TimeStamp& ts) {
            uint64_t tiebreak = ROB::tiebreakCounter++;
            // Recall the following is idempotent
            ts.assignTieBreaker(tiebreak);
        }

        bool deepen(TaskPtr t, ThreadID tid, uint64_t maxTS);

        bool requestZoomOutIfNeeded(const ConstTaskPtr& t, ThreadID tid) {
            return zoomRequester.requestZoomOutIfNeeded(t, tid);
        }

        uint32_t getFrameMinDepth() const {
            return frameBaseDepth + (frameState == rob::FrameState::IN);
        }
        uint32_t getFrameMaxDepth() const {
            return frameBaseDepth + ossinfo->maxFrameDepth - 1
                   - (frameState == rob::FrameState::OUT);
        }

        void setGvt(TimeStamp newGvt, ThreadID tid, uint64_t cycle) {
            DEBUG_GVT("[%s] Received set gvt %s from tid %u", name(),
                      newGvt.toString().c_str(), tid);
            assert_msg(!setGvtInProgress.hasWaiters(),
                       "Set gvt already in progress with new vt: %s",
                       gvtToSet.toString().c_str());
            assert(gvtToSet == INFINITY_TS);

            setGvtInProgress.addWaiter(tid);
            gvtToSet = newGvt;
            assignTieBreaker(gvtToSet);
            assert(gvtToSet < gvt);  // For now we only allow setGvt to lower the gvt
            LVTUpdateMsg msg = {gvtToSet,
                                INFINITY_TS,
                                0,
                                true,
                                0 /*Don't care for setGvt*/,
                                ZoomType::NONE,
                                AckType::NONE};
            lvtUpdatePort->send(msg, cycle);
        }

        // FIXME(mcj) why here?? Also this risks letting a *finished* task clear
        // its read set. Is that the goal? I doubt it. I guess ideally the Core
        // could call this, and its TerminalCD would be tracking read/write sets
        // for its one task. But all tasks are lumped into the tile's accessors
        // table, so here were are.
        void clearReadSet(const Task& task) { tileCD->clearReadSet(task); }

    private:
        void getZoomState(ZoomType& zoom, TimeStamp& zoomRequesterTs,
                          AckType& ack, uint32_t& chIdx) const;

        inline bool isQueued(const TaskPtr& task) const {
            return runQueue.count(task)
                    || execQueue.count(task)
                    || commitQueue.count(task)
                    || doomedTasks.right.count(task);
        }

        inline uint64_t queuelessDoomedTasks() const {
            // When a task is aborted within priv mode it is doomed but runs to
            // the completion of its priv block
            // If ROB::finishAbort is called before the task exits priv mode,
            // the task will be removed from the CQ, and not contribute to
            // commitQueue.size() like an ordinary running speculative task.
            // FIXME(mcj) The existence of this method is further proof that
            // this doomedTasks handling of doomed tasks is horrendous.
            uint64_t sum = 0ul;
            for (const auto& pair : doomedTasks) {
                TaskPtr doomed = pair.right;
                sum += !commitQueue.count(doomed);
            }
            return sum;
        }

        inline uint64_t queuelessDoomedTiedTasks() const {
            uint64_t sum = 0ul;
            for (const auto& pair : doomedTasks) {
                TaskPtr doomed = pair.right;
                sum += (doomed->isTied() && !commitQueue.count(doomed));
            }
            return sum;
        }

        void sendCutTieMessages(const TaskPtr& task);

        // dsm: This method is a performance optimization---avoids traversing
        // rq and cq on every task receive().
        void cutTie(const TaskPtr& task) {
            task->cutTie();
            if (task->hasContainer()) {
                // [mcj] We could also test isQueued(task), but that's expensive
                tiedTasks--;
                if (!task->idle()) tiedCQTasks--;
            } else {
                // The task is in the local TSB; no need to adjust tied count
            }
        }

        TaskPtr createSpillerTask(rob::SpillerType type, uint64_t domainId,
                                  uint64_t domainDepth);

        void abortOutOfZoomOutFrameTasks(uint32_t maxDepth);

        void abortOutOfZoomInFrameTasks(TimeStamp zoomInFrameMax);

        void ensureFrameSpillingCanHappen();

        void zoomingAbort(const TaskPtr& t) {
            assert(!t->isIrrevocable());
            assert(!t->idle());
            // Assume t is resident at this ROB
            assert(t->container()->getIdx() == getIdx());
            trace::zoomingAbort(t.get());

            if (ossinfo->abortHandler->abort(
                    t, getIdx(), Task::AbortType::HAZARD,
                    getCurCycle() + 1, -1, nullptr)) {
                zoomingAborts.inc(t->isTied(), t->completed());
            }
        }

        // NOTE: Takes in a TaskPtr, not a const TaskPtr&, so that it's safe to
        // call with a direct ref to one of the queues (if we didn't copy the ref,
        // it'd go out of scope when we abort it in the middle of the method)
        void resourceConstrainedAbort(TaskPtr t) {
            assert(!t->isIrrevocable());
            // Assume t is resident at this ROB
            assert(t->container()->getIdx() == getIdx());
            trace::resourceAbort(t.get());

            // FIXME the following will issue two unnecessary wakeups:
            // 1) because of the re-enqueue, any thread waiting for an
            //    element in the runqueue will be woken
            // 2) any thread that was waiting for a commitQ slot will be
            //    awoken
            // Those are both wasteful because this method was called to free
            // resources for the current thread, but it is about to use up all
            // the resources again.
            // This is mostly a simulation performance problem, but it could
            // transfer some cycles to executing that should have just remained
            // idle cycles.
            // FIXME(victory): The use of getCurThreadLocalCycle() doesn't make
            // sense here, as this is frequently called by non-thread events.
            if (ossinfo->abortHandler->abort(
                    t, getIdx(), Task::AbortType::HAZARD,
                    getCurThreadLocalCycle() + 1, -1, nullptr)) {
                // [mcj] Given the context of all current callers, a
                // resource-constrained-aborted task should never be idle.
                assert(!t->idle());
                resourceAborts.inc(t->isTied(), t->completed());
            }
        }

        void exceptionedTaskAbort(TaskPtr t);

        bool taskQueueIsFull() const {
            assert(taskQueueSize() <= taskQueueCapacity);
            return taskQueueSize() == taskQueueCapacity;
        }

        void attemptTaskQueueReservation();

        void attemptCommitQueueEvictionFor(const Task& incoming) {
            assert(commitQueue.full());

            DEBUG("%s commit queue full", name());
            TaskPtr t = commitQueue.replacementCandidateFor(incoming);
            if (t) {
                assert(t.get() != &incoming);
                DEBUG("[%s] Abort %s for resource: Commit queue", name(),
                      t->toString().c_str());
                resourceConstrainedAbort(t);
                // TODO(mcj) with same-hint serialization or other sophisticated
                // dequeue selection, we may have freed up a commit queue slot
                // for an undeserving task...
            }
        }

        void attemptCoreEvictionFor(const TaskPtr&);

        void enqueue(const TaskPtr& task);

        std::pair<TaskPtr, rob::Stall> taskToRun(ThreadID tid);

        bool shouldThrottle();

        void commitTasks();

        void commitTask(const TaskPtr& task) {
            DEBUG("%s::commit task %s gvt %s", name(),
                  task->toString().c_str(), gvt.toString().c_str());
            assert(abortQueue.count(task) == 0);
            assert_msg(!doomedTasks.right.count(task),
                       "Committing task remains in doomed set: %s",
                       task->toString().c_str());

            if (task->isIrrevocable()) {
                // Irrevocables should never take a CQ slot
                assert(commitQueue.count(task) == 0);
            } else {
                commitQueue.erase(task);
                assert(!commitQueue.full());
                // Wake up all threads waiting for a commit queue slot, since we
                // don't know how many idle tasks might be able to evict their
                // way to a slot in the CQ now.
                cqNotFull.notifyAll();
            }
            tileCD->clearReadSet(*task);
            tileCD->clearWriteSet(*task);
            if (task->isTied()) cutTie(task);
            sendCutTieMessages(task);

            task->commit();

            if (task->isRequeuer() || task->isSpiller()) spillCommits.inc();
            else appCommits.inc();
        }

        void makeIrrevocable(const TaskPtr& t);

        bool canRunIrrevocably(TaskPtr t) const {
            if (t->exceptioned()
                    || t->hasPendingAbort()
                    || t->madeSpeculativeSyscall()
                    || t->isTied()
                ) return false;
            // Optionally run single-threaded systems without (much) speculation
            if (ossinfo->oneIrrevocableThread
                    && t->lts() <= lvt() && t->lts() <= ossinfo->tsbs[0]->lvt()
                ) return true;
            return
                   (t->lts() == gvt) ||
                   (t->noTimestamp) ||
                   (t->isRequeuer() && !t->isFrameRequeuer()) || t->isSpiller() ||
                   ((t->maySpeculate() || t->cantSpeculate())
                    && t->lts().sameDomain(gvt)
                    && t->lts().app() == gvt.app()
                   );
        }

        void wakeStalledIrrevocables() {
            for (TaskPtr t : execQueue) {
                // A harmless action if t is not stalled
                if (t->isIrrevocable()) stallers.notify(t->runningTid);
            }
        }

        uint64_t abortChildTasks(TaskPtr p, uint32_t depth, uint64_t cycle);

        TaskPtr lvtTask() const;
};
