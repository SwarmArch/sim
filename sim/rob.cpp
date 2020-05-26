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

#include "sim/rob.h"

#include <cmath>
#include <unordered_map>

#include "sim/assert.h"
#include "sim/domainprofiler.h"
#include "sim/driver.h"
#include "sim/loadstatssummary.h"
#include "sim/taskloadprofiler.h"
#include "sim/task_mapper.h"
#include "sim/trace.h"

#undef DEBUG
#define DEBUG(args...) //info(args)

// Tiebreaker used to start at 0. Now it starts at 1, so ts=0:0 is only used by
// irrevocable tasks.
uint64_t ROB::tiebreakCounter = TimeStamp::MIN_TIEBREAKER;

ROB::ROB(uint32_t commitQueueCapacity,
         CommitQueueAdmissionPolicy admissionPolicy, uint32_t taskQueueCapacity,
         uint32_t taskQueueOverflow, uint32_t removableCacheCapacity,
         uint32_t tiedCapacity, uint32_t underflowThreshold, uint32_t numCores,
         uint32_t numThreadsPerCore, rob::TBPolicy tieBreakPolicy,
         bool clearTieBreakerOnAbort, bool serialSpatialIDs,
         bool bulkSynchronousTasks, bool adaptiveThrottle,
         const std::string& _name, uint32_t _idx, TaskMapper* taskMapper_,
         ThreadCountController* tcc, uint32_t _numCanariesInTsArray)
    : SimObject(_name),
      execQueue(numCores * numThreadsPerCore),
      commitQueue(commitQueueCapacity, admissionPolicy),
      taskQueueCapacity(taskQueueCapacity),
      tiedCapacity(tiedCapacity),
      underflowThreshold(underflowThreshold),
      overflowWatch(runQueue, [this]() { return taskQueueSize(); },
                    taskQueueOverflow, removableCacheCapacity),
      frameOverflowWatch(runQueue),
      tiedTasks(0),
      tiedCQTasks(0),
      gvt(ZERO_TS),
      gvtToSet(INFINITY_TS),
      childIdx(-1U),
      frameState(rob::FrameState::STEADY_STATE),
      zoomInFrameMax(INFINITY_TS),
      zoomRequester(_idx, *this),
      numCanariesInTsArray(_numCanariesInTsArray),
      taskMapper(taskMapper_),
      threadCountController(tcc),
      tieBreakPolicy(tieBreakPolicy),
      clearTieBreakerOnAbort(clearTieBreakerOnAbort),
      serializeSpatiallyEqualTasks(serialSpatialIDs),
      bulkSynchronousTasks(bulkSynchronousTasks),
      adaptiveThrottle(adaptiveThrottle),
      idx(_idx) {
    assert(taskQueueCapacity >= taskQueueOverflow);
    assert(taskQueueCapacity >= underflowThreshold);
    // [mcj] Disable underflow for now as it's only causing headaches
    // (deadlocks) for marginaly performance improvement.
    assert(underflowThreshold == 0);
    assert(taskQueueOverflow > tiedCapacity);
    assert(tiedCapacity > commitQueue.capacity());
    assert(taskQueueOverflow >= commitQueue.capacity());
    assert(taskQueueCapacity >=
           removableCacheCapacity + commitQueue.capacity());
    // With removableCacheCapacity = N, N untied tasks are protected from
    // removal from the task queue. This is intended to keep some balance
    // between tied vs. untied tasks, but with the advent of tiedCapacity, it
    // might be moot.
    // Recall that only when |TQ| exceeds the overflow will a spiller remove
    // untied tasks. However, if the removable capacity plus the overflow exceed
    // |TQ| then no untied tasks will ever be removed.
    assert(taskQueueCapacity >= removableCacheCapacity + taskQueueOverflow);
    // At least one untied task must be protected from removal, or else we risk
    // live-lock, where a spiller removes the one untied task in the queue and
    // replaces it with an untied requeuer.
    // Granted now that we have TSBs (and a reserved TSB slot for a spiller
    // child), maybe the following assertion isn't necessary.
    assert(removableCacheCapacity > 0);
    // If a producer is under the underflowThreshold, it is prioritized to run.
    // However, if that producer's children cause spillers to run and they
    // remove tasks below the underflow, that's wasteful thrashing. Therefore
    // underflow must be set under the removableCacheCapacity.
    assert(underflowThreshold <= removableCacheCapacity);
}

ROB::~ROB() {
    if (taskQueueSize()) {
        info("[%s] Tasks @ termination (%ld rq, %ld cq, %ld exe)",
             name(), runQueue.size(), commitQueue.size(), execQueue.size());
        // dumpQueues();

        // Mark tasks unreachable to avoid assertions...
        for (auto& t : runQueue) t->markUnreachable();
        for (auto& t : commitQueue) t->markUnreachable(true);
    }
}

void ROB::initStats(AggregateStat* parentStat) {
    AggregateStat* robStats = new AggregateStat();
    robStats->init(name(), "ROB stats");
    appDequeues.init("taskdeq", "real task dequeues from ROB"); robStats->append(&appDequeues);
    spillDequeues.init("spilldeq", "spiller,requeuer dequeues from ROB"); robStats->append(&spillDequeues);
    spillers.init("spills", "spillers created by ROB"); robStats->append(&spillers);
    stealSends.init("stealSend", "tasks donated through steals"); robStats->append(&stealSends);
    stealReceives.init("stealRecv", "tasks stolen from others"); robStats->append(&stealReceives);
    appCommits.init("taskcom", "commits for real tasks from ROB"); robStats->append(&appCommits);
    spillCommits.init("spillcom", "commits for spillers, requeuers from ROB"); robStats->append(&spillCommits);
    enqueues.init("enq", "enqueues to ROB"); robStats->append(&enqueues);
    totalAborts.init("totAbrt", "total tasks aborted"); robStats->append(&totalAborts);
    aborts.init("abrt", "aborts from ROB"); robStats->append(&aborts);
    abortsWhileRunning.init("abrtWR", "aborts of a currently running task from ROB"); robStats->append(&abortsWhileRunning);
    abortsForParent.init("abrtFP", "aborts because of a parent abort"); robStats->append(&abortsForParent);
    resourceAborts.init("abrtRsrc", "aborts due to unavailable resource",
                        {"untied", "tied"}, {"running", "completed"});
    robStats->append(&resourceAborts);
    zoomingAborts.init("abrtZoom", "aborts due to Fractal zooming",
                       {"untied", "tied"}, {"running", "completed"});
    robStats->append(&zoomingAborts);
    runQueue.sizeStat.init("runQLength", "run queue length"); robStats->append(&runQueue.sizeStat);
    execQueue.sizeStat.init("execQLength", "exec queue length"); robStats->append(&execQueue.sizeStat);
    commitQueue.sizeStat.init("commitQLength", "commit queue length"); robStats->append(&commitQueue.sizeStat);

    uint32_t binSize = 10;
    auto bins = [binSize](uint32_t elems) {
        return std::min(100u, elems / binSize + !!(elems % binSize) + 1u);
    };
    runQueue.sizeHist.init("runQHist", "run queue length histogram", bins(taskQueueCapacity), binSize, 0/*lower bound*/); robStats->append(&runQueue.sizeHist);
    execQueue.sizeHist.init("execQHist", "exec queue length histogram", execQueue.capacity()+1, 1, 0); robStats->append(&execQueue.sizeHist);
    commitQueue.sizeHist.init("commitQHist", "commit queue length histogram", bins(commitQueue.capacity()), binSize, 0); robStats->append(&commitQueue.sizeHist);
    tasksPerOrdinarySpiller.init("tasksPerOrdinarySpill", "# of tasks removed per ordinary spiller, histogram", ossinfo->tasksPerSpiller);
    robStats->append(&tasksPerOrdinarySpiller);
    tasksPerFrameSpiller.init("tasksPerFrameSpill", "# of tasks removed per frame spiller, histogram", ossinfo->tasksPerSpiller);
    robStats->append(&tasksPerFrameSpiller);
    numTasksSkippedForHintSerializationPerDeq.init("tasksSkippedForHintSerial", "# of tasks in run queue skipped for hint serialization, per dequeue attempt");
    robStats->append(&numTasksSkippedForHintSerializationPerDeq);

    depthCounters.init(robStats);
    parentStat->append(robStats);
}

std::pair<TaskPtr, rob::Stall> ROB::dequeueTask(uint32_t tid) {
    // [ssub] As the threadCountController is only checked in ROB::taskToRun(),
    // the logic below allows "throttled" threads to run spillers
    // if that is the first task they are dequeueing. But that
    // should be a rare occurence.
    TaskPtr task;
    rob::Stall reason;

    bool triggerFrameSpiller =
        (frameState == rob::FrameState::IN) &&
        frameOverflowWatch.spillerShortage(zoomInFrameMax);

    auto spillerShortage = overflowWatch.spillerShortage();
    bool triggerOrdinarySpiller =
        spillerShortage.shortage &&
        (frameState != rob::FrameState::IN);  // Don't mess with zooming in?

    // [victory] Frame spillers are prioritized over ordinary spillers,
    // since if there is a need for both freeing task queue capacity and
    // frame-spilling, running a frame spiller may address both needs,
    // and no frame spiller ever adds to task queue pressure.
    if (triggerFrameSpiller) {
        task = createSpillerTask(rob::SpillerType::FRAME, /*unused*/0, 0);
        assert(task);
    } else if (triggerOrdinarySpiller) {
        task = createSpillerTask(rob::SpillerType::ORDINARY,
                                 spillerShortage.domainId,
                                 spillerShortage.depth);
        assert(task);
    } else {
        std::tie(task, reason) = taskToRun(tid);
    }

    if (!task) {
        if (reason == rob::Stall::EMPTY) {
            rqNotEmpty.addWaiter(tid);
        } else if (reason == rob::Stall::THROTTLE) {
            DEBUG("[%s] Throttling thread %u", name(), tid);
            rqHasRunnable.addWaiter(tid);
        } else /*reason == rob::Stall::THROTTLE_MT*/
            reason = rob::Stall::THROTTLE;
        return std::make_pair(nullptr, reason);
    }

    assert_msg(!task->isIrrevocable(),
               "Idle task should not be irrevocable: %s",
               task->toString().c_str());
    if (canRunIrrevocably(task)) makeIrrevocable(task);

    assert_msg(!(task->isSpiller() || task->isRequeuer()) || task->isIrrevocable(),
               "Spills should happen irrevocably: %s", task->toString().c_str());

    assert(!(task->isIrrevocable() && task->hasPendingAbort()));
    assert(!(task->isIrrevocable() && task->isTied()));
    assert(task->isIrrevocable() || !task->cantSpeculate());

    if (!task->isIrrevocable()) {
        if (commitQueue.full()) attemptCommitQueueEvictionFor(*task);
        if (commitQueue.full()) {
            DEBUG("[%d] %s::dequeue stall %s",
                  tid, name(), task->toString().c_str());
            cqNotFull.addWaiter(tid);
            return std::make_pair(nullptr, rob::Stall::CQ);
        }
    } else {
        // Irrevocable tasks do not need a commit queue entry as they can't
        // mispeculate. Moreover, other than requeuers, they do not need a task
        // queue slot once they start running, as no record is required.
    }
    if (!task->isSpiller()) runQueue.erase(task);

    if (tieBreakPolicy == rob::TBPolicy::DEQUEUE || task->hasPendingAbort()) {
        // See Task::lts() for discussion.
        if (!task->isIrrevocable()) assignTieBreaker(task->ts);
    }

    if (task->noTimestamp) {
        // victory: maybe change this some day, but before allowing speculative
        // NOTIMESTAMP tasks, we need to figure out the semantics for any
        // NOTIMESTAMP tasks touching tracked memory. See reasoning in
        // IsValidAddress().
        assert(task->isIrrevocable());

        // The task must now continue holding the GVT from reaching infinity
        // and triggering termination, but not hold back the GVT from reaching
        // any timestamped tasks.
        assert(task->ts == ZERO_TS ||
               (task->isOrdinarySpiller() && task->ts == NO_TS));
        task->ts = NO_TS;
    }

    // Timestamp changes such as calling assignTieBreaker must be done above,
    // before the task is put into any queue below.

    if (!task->isIrrevocable()) {
        commitQueue.insert(task);
        if (task->isTied()) tiedCQTasks++;
    }

    if (task->hasPendingAbort()) abortQueue.insert(task);

    // All tasks appear in the execQ, even irrevocables, guaranteeing they play
    // a role in LVT computation (i.e. holding it back for correctness).
    execQueue.insert(task);
    task->start(tid);
    assert_msg(task->lts() >= gvt, "%s < %s task %s",
               task->lts().toString().c_str(), gvt.toString().c_str(),
               task->toString().c_str());
    assert(doomedTasks.size() < execQueue.capacity());
    DEBUG("[%d] %s::dequeue task %s", tid, name(), task->toString().c_str());

    if (task->isRequeuer() || task->isSpiller()) spillDequeues.inc();
    else appDequeues.inc();
    return std::make_pair(task, rob::Stall::NONE);
}

void ROB::notifyTaskMapper(const TaskPtr& task, rob::TMEvent t) {
    if (t == rob::TMEvent::ENQUEUE)
        taskMapper->newTask(task.get());
    else if (t == rob::TMEvent::SPILL)
        taskMapper->spillTask(task.get());
    else if (t == rob::TMEvent::STEAL)
        taskMapper->stealTask(task.get());
    else
        panic("Unrecognized TMEvent");
}

void ROB::receive(const CutTieMsg& msg, uint64_t cycle) {
    // Can task be aborted (and removed from TQ/EQ/CQ) before CutTieMsg arrives? No.
    // 1. Task is aborted because of data dependence: It will remain in
    //    in one of TQ,EQ. Hence the CutTieMsg can be processed safely.
    // 2. Task is aborted because of parent->child abort: Our network ensures
    //    point-to-point ordering. Hence a cut-tie message (that can arise from
    //    parent committing, or becoming irrevocable) received after the abort
    //    message => intervening enqueue messages that create the child in this
    //    ROB => child has to be present in one of TQ/EQ/CQ by the time
    //    CutTieMsg arrives.
    const TaskPtr& task = msg.task;

    // But it is possible the child task has already committed before the
    // cut tie message arrives.
    // NOTE: This check is easy in hardware because each task stored the TQ
    // index of their children, and sends it on the cutTie message
    if (isQueued(task)) {
        assert(task->isTied() || task->lts() == gvt);
        if (task->isTied()) cutTie(task);

        if (task->running()) {
            assert(task->hasContainer());
            assert(task->container() == this);
            // Notify TSB of cut ties if required
            // TODO Replace with a tsbPort for cut tie message?
            ossinfo->tsbs[idx]->wakeup(task->runningTid);
            if (!task->isIrrevocable() && canRunIrrevocably(task)) {
                // The task can run now run irrevocably
                makeIrrevocable(task);
            }
        } else if (task->idle()) {
            // Untying this task may have made it spillable.
            // Ensure the tile has a chance to run launch a spiller, unstall
            // any thread waiting on a CQ slot or stalled on a "runnable" task
            cqNotFull.notifyOne();
            rqHasRunnable.notifyOne();
            //FIXME(victory): Do we need to call attemptTaskQueueReservation()
            // here to really ensure ordinary spillers can run?
            ensureFrameSpillingCanHappen();
        }
        DEBUG("[%s] Cut tie for task %s", name(), task->toString().c_str());

        assert(!task->noTimestamp || canRunIrrevocably(task));
    }
}

void ROB::receive(const TaskEnqueueReq& req, uint64_t cycle) {
    const TaskPtr& task = req.task;
    assert(!task->isTied() || task->parent == req.parent);
    assert(!task->ts.hasAssignedTieBreaker());
    assert(task->ts >= gvt);

    auto sendReply = [this, &req, cycle](bool isAck) {
        TaskEnqueueResp resp = {isAck, getIdx(), req.tsbPos};
        tsbPorts[req.tsbIdx]->send(resp, cycle);
    };

    if (task->abortedInTransit) {
        // Just send an ACK and do nothing else, the TSB knows this is an
        // aborted task and will not retry
        sendReply(true);
        return;
    }

    // The task should generally fit within the current frame, with edge cases
    // when a zoom is ongoing.
    uint64_t domainDepth = task->ts.domainDepth();
    if (domainDepth > getFrameMaxDepth()) {
        assert(frameState == rob::FrameState::OUT);
        assert(domainDepth == getFrameMaxDepth() + 1);
        // Too-deep domain will be discarded by the domain creator aborting.
        // See ROB::abortOutOfZoomOutFrameTasks().
        assert(task->isTied() || task->hasPendingAbort());
        DEBUG("[%s] NACKing too-deep tsb %d pos %d", name(),
              req.tsbIdx, req.tsbPos);
        sendReply(false);
        return;
    } else if (domainDepth < getFrameMinDepth()) {
        assert(frameState == rob::FrameState::IN);
        assert(domainDepth == getFrameMinDepth() - 1);
        // the following assertions should be implied by task->ts >= gvt
        assert(task->ts >= zoomInFrameMax.domainMin());
        assert(task->ts > zoomInFrameMax);
        // This task needs to be aborted or spilled to enable the zoom-in.
        // If the task is untied, it must be spilled.
        if (!task->isTied()) ensureFrameSpillingCanHappen();
        // Otherwise, it must be discarded by a parent abort.
        // If the parent abort is likely, we should nack.
        // I assume the parent abort is unlikely and we're better off
        // accepting the task and spilling it when it becomes untied.
        // (See call of ensureFrameSpillingCanHappen() in
        // ROB::receive(CutTieMsg).)
    }

    // NOTE: This check will be invalid with non-atomic GVT updates.
    // But for now it's useful to detect whether we aren't correctly
    // untying GVT children.
    assert_msg(!task->parent
               || task->parent->lts() > gvt
               || task->exceptioned()
               || task->parent->hasPendingAbort(),
               "Received a tied task whose parent is the GVT task: "
               "%s parent %s gvt %s",
               task->toString().c_str(), task->parent->toString().c_str(),
               gvt.toString().c_str());

    if (task->isTied() && taskQueueSize() >= tiedCapacity) {
        assert(!req.isSpillerChild());
        const uint32_t tiedRQTasks = tiedTasks
                                     - tiedCQTasks
                                     - queuelessDoomedTiedTasks();

        // Enable to check that our tied task counts are right
        // (mcj: correct in all tests as of 06/15/2017)
#if 0
        auto tl = [](const TaskPtr& t) { return t->isTied(); };
        uint32_t actualTiedRQTasks =
            std::count_if(runQueue.begin(), runQueue.end(), tl);
        uint32_t actualTiedCQTasks =
            std::count_if(commitQueue.begin(), commitQueue.end(), tl);

        assert_msg(tiedCQTasks == actualTiedCQTasks,
                   "[%s][%ld] Mismatch: "
                   "tiedCQTasks (%u) != actualTiedCQTasks (%u)",
                   name(), cycle, tiedCQTasks, actualTiedCQTasks);
        assert_msg(tiedRQTasks == actualTiedRQTasks,
                   "[%s][%ld] Mismatch: "
                   "tiedRQTasks (%u) != actualTiedRQTasks (%u)",
                   name(), cycle, tiedRQTasks, actualTiedRQTasks);
#endif

        // NOTE : rqCap is variable, as it can grow up to
        // takQueueCapacity with an empry commitQueue.
        uint32_t rqCap = taskQueueCapacity - commitQueue.size();
        uint32_t tiedRQCap = tiedCapacity * rqCap / taskQueueCapacity;
        assert_msg(tiedTasks <= tiedCapacity, "[%s] tiedTasks: %u, tiedCap: %u",
                   name(), tiedTasks, tiedCapacity);
        if (tiedTasks == tiedCapacity || tiedRQTasks >= tiedRQCap) {
            DEBUG("[%s] NACKing tied tsb %d pos %d", name(), req.tsbIdx,
                  req.tsbPos);
            sendReply(false);
            return;
        } else {
            assert(tiedTasks < tiedCapacity);
            assert(tiedRQTasks < tiedRQCap);
        }
    }

    uint32_t threshold = taskQueueCapacity;
    if (!req.isSpillerChild()) threshold -= 1;
    if (taskQueueSize() >= threshold) {
        attemptTaskQueueReservation();
        DEBUG("[%s] NACKing untied tsb %d pos %d nc %ld threshold %d",
              name(), req.tsbIdx, req.tsbPos, overflowWatch.neededSpillers(),
              threshold);
        // [mcj] In all cases the task queue remains full, but we made
        // space if enqueuer is the GVT-holder.
        assert(taskQueueSize() >= threshold);

        // dsm: Corner case: We abort a task that happens to be an
        // ancestor of our current one...
        // mcj: the abortedInTransit flag is scheduled to be brought up,
        // so it should remain false now.
        assert(!task->abortedInTransit);
        sendReply(false);
        return;
    }
    assert(!task->abortedInTransit);
    sendReply(true);

    // FIXME [mcj] try to move DepthCounters to a task observer
    // implementation, rather than this hack
    DepthCounters* dc = req.isSpillerChild() ? nullptr : &depthCounters;
    task->setDepthCounters(dc);
    task->setContainer(this);
    task->registerObserver(createTaskProfiler((void*)task->taskFn));

    if (tieBreakPolicy == rob::TBPolicy::ENQUEUE) {
        // See Task::lts() for discussion.
        assert(!task->isSpiller());
        if (!task->isRequeuer()) assignTieBreaker(task->ts);
    }

    if (req.isSpillerChild()) {
        assert(!task->isTied());
        assert(task->isRequeuer());
        // For generic requeuers, the first arg points to a struct whose first
        // field is the number of children.
        uint64_t nChildren = *reinterpret_cast<uint64_t*>(task->args[0]);
        assert(nChildren >= 1);
        assert(nChildren <= ossinfo->tasksPerSpiller);
        if (task->isOrdinaryRequeuer())
            tasksPerOrdinarySpiller.inc(nChildren-1);
        else if (task->isFrameRequeuer())
            tasksPerFrameSpiller.inc(nChildren-1);
    }

    assert(!task->runOnAbort || task->noTimestamp);

    enqueue(task);
    notifyTaskMapper(task, rob::TMEvent::ENQUEUE);
    task->registerObserver(createTaskLoadProfiler(taskMapper->getLoadStatsSummary(task.get())));
    if (profileDomains()) task->registerObserver(createDomainProfiler(*task));
}


#ifdef ATOMIC_GVT_UPDATES
void ROB::updateGVT(const GVTUpdateMsg& msg, uint64_t cycle) {
#else
void ROB::receive(const GVTUpdateMsg& msg, uint64_t cycle) override {
#endif
    const TimeStamp& newGVT = msg.gvt;
    ZoomType zoom = msg.zoom;
    const TimeStamp& frameBase = msg.frameBase;
    const bool cancelPrevZoom = msg.cancelPrevZoom;
    DEBUG_GVT("[%s] GVT update %s->%s, zoom: %s, frameBase: %s",
              name(),  // cycle,
              gvt.toString().c_str(), newGVT.toString().c_str(),
              ((zoom == ZoomType::NONE)
                   ? "NONE"
                   : (zoom == ZoomType::IN) ? "IN" : "OUT"),
              frameBase.toString().c_str());

    // dsm: GVT arbiter should not send spurious (==) updates, if
    // this assertion fails with equality it's a performance bug.
    //assert(newGVT != gvt);
    // [victory] Yup, we have a performance bug with frame that we should fix.
    //           See gvt_arbiter.cpp.

    // 1. setGvt actions
    if (setGvtInProgress.hasWaiters() && newGVT == gvtToSet) {
        TaskPtr t = lvtTask();
        // In 1-thread systems, not even t->lts.app() == gvt.app() must hold,
        // since tasks are immediately marked irrevocable on dequeue, even
        // before the `GVT protocol' can update the gvt.
        assert((t->lts() == gvt) || ossinfo->oneIrrevocableThread);
        assert(execQueue.find(t) != execQueue.end());
        // Update the VT of the gvt task to the new GVT
        t->ts = newGVT;  // This is safe, task is in execQueue
        // Wake up the blocked thread
        DEBUG_GVT("[%s] Received gvtToSet, now unblocking thread", name());
        // Exactly one thread can have a setGvt in progress (the one
        // running the gvt task).
        setGvtInProgress.notifyOne();
        assert(!setGvtInProgress.hasWaiters());
        gvtToSet = INFINITY_TS;
    }

    // 2. Update gvt
    const bool gvtUpdated = (gvt != newGVT);
    gvt = newGVT;
    commitTasks();

    // 3. Frame actions
    // [victory] We assume here that the GVT update has committed
    // tasks as needed before the zoom can begin, so the only remaining
    // going-out-of-frame tasks are to be aborted and/or spilled.
    assert(gvt >= frameBase);
    rob::FrameState oldFrameState = frameState;
    switch (frameState) {
        case rob::FrameState::STEADY_STATE:
        {
            if (zoom == ZoomType::OUT) {
                DEBUG_GVT(
                    "[%s] Frame::SS -> Frame::OUT, setting "
                    "frameBase: %s",
                    name(), frameBase.toString().c_str());
                assert(frameBase.domainDepth() == frameBaseDepth - 1);
                frameState = rob::FrameState::OUT;
                abortOutOfZoomOutFrameTasks(getFrameMaxDepth());
            } else if (zoom == ZoomType::IN) {
                assert(frameBase.domainDepth() == frameBaseDepth + 1);
                frameState = rob::FrameState::IN;
                zoomInFrameMax = frameBase.domainMax();
                DEBUG_GVT(
                    "[%s] Frame::SS -> Frame::IN, setting "
                    "zoomInFrameMax: %s",
                    name(), zoomInFrameMax.toString().c_str());
                abortOutOfZoomInFrameTasks(zoomInFrameMax);
                ensureFrameSpillingCanHappen();
            }
        }
        break;

        case rob::FrameState::OUT:
        {
            assert(zoom != ZoomType::IN)
            if (zoom == ZoomType::NONE) {
                // If the gvt arbiter de-asserts zoom out, then all ROBs have
                // completed zoom out actions, or the prev zoom has been
                // cancelled. Move back to steady state.
                if (!cancelPrevZoom) {
                    DEBUG_GVT("[%s] Frame::OUT -> Frame::WAIT_SS, cancelPrevZoom: false", name());
                    frameState = rob::FrameState::WAIT_SS;
                    // Adjust (i.e. shift timestamps) in TQ,
                    // canaries in L2 TSarray. There is no need
                    // to clear any of the timestamps.
                    // Upper bound the delay in shifting; primarily for
                    // simulations when using very large TQ/CQ.
                    uint64_t cycle =
                        getCurCycle() +
                        std::min(
                            8192ul,
                            std::max(static_cast<size_t>(numCanariesInTsArray),
                                     taskQueueSize()));
                    Event* canaryEvent =
                        schedEvent(cycle, [this](uint64_t cycle) {
                            DEBUG(
                                "[%s] ZoomOut canary event at %lu "
                                "(Frame::WAIT_SS "
                                "-> Frame::SS)",
                                name(), cycle);
                            ossinfo->abortHandler->adjustCanariesOnZoomOut(
                                idx, getFrameMaxDepth());
                            frameState = rob::FrameState::STEADY_STATE;
                        });
                    DEBUG(
                        "[%s] Scheduled zoom out canary event %p for cycle: "
                        "%lu (%lu)",
                        name(), canaryEvent, cycle, getCurCycle());
                    (void)canaryEvent;
                    DEBUG(
                        "[%s] Zoom out complete.  Waking blocked threads.  "
                        "frameBaseDepth updated to: %u",
                        name(), frameBaseDepth - 1);
                    assert(frameBase.domainDepth() == frameBaseDepth - 1);
                    assert(frameBaseDepth > 0);
                    frameBaseDepth--;
                    zoomRequester.notifyMinDepthDecreased();
                } else {
                    DEBUG_GVT("[%s] Frame::OUT -> Frame::SS, cancelPrevZoom: true", name());
                    assert(frameBase.domainDepth() == frameBaseDepth);
                    frameState = rob::FrameState::STEADY_STATE;
                    zoomRequester.notifyMaxDepthIncreased();
                }
            }
        }
        break;

        case rob::FrameState::IN:
        {
            assert(zoom != ZoomType::OUT)
            if (zoom == ZoomType::NONE) {
                // If the gvt arbiter de-asserts zoom in, then all ROBs have
                // completed zoom in actions, or the prev zoom was cancelled.
                // Move back to steady state.
                if (!cancelPrevZoom) {
                    DEBUG_GVT("[%s] Frame::IN -> Frame::WAIT_SS, cancelPrevZoom: false", name());
                    assert(zoomInFrameMax == frameBase.domainMax());
                    zoomInFrameMax = INFINITY_TS;
                    frameState = rob::FrameState::WAIT_SS;
                    // Adjust (i.e. shift / clear timestamps) in TQ,
                    // canaries in L2 TSarray.
                    // Upper bound the delay in shifting; primarily for
                    // simulations when using very large TQ/CQ.
                    uint64_t cycle =
                        getCurCycle() +
                        std::min(
                            8192ul,
                            std::max(static_cast<size_t>(numCanariesInTsArray),
                                     taskQueueSize()));
                    Event* canaryEvent =
                        schedEvent(cycle, [this, frameBase](uint64_t cycle) {
                            DEBUG(
                                "[%s] ZoomIn canary event at %lu "
                                "(Frame::WAIT_SS "
                                "-> Frame::SS)",
                                name(), cycle);
                            ossinfo->abortHandler->adjustCanariesOnZoomIn(
                                idx, frameBase, frameBase.domainMax());
                            frameState = rob::FrameState::STEADY_STATE;
                        });
                    DEBUG(
                        "[%s] Scheduled zoom in canary event %p for "
                        "cycle: %lu (%lu)",
                        name(), canaryEvent, cycle, getCurCycle());
                    (void)canaryEvent;
                    DEBUG(
                        "[%s] Zoom in complete.  Waking blocked threads.  "
                        "frameBaseDepth updated to: %u",
                        name(), frameBaseDepth + 1);
                    assert(frameBase.domainDepth() == frameBaseDepth + 1);
                    frameBaseDepth++;
                    zoomRequester.notifyMaxDepthIncreased();
                } else {
                    DEBUG_GVT("[%s] Frame::IN -> Frame::SS, cancelPrevZoom: true", name());
                    assert(frameBase.domainDepth() == frameBaseDepth);
                    frameState = rob::FrameState::STEADY_STATE;
                    zoomInFrameMax = INFINITY_TS;
                    zoomRequester.notifyMinDepthDecreased();
                }
            }
        }
        break;

        case rob::FrameState::WAIT_SS: break;

        default: panic("Invalid frame state"); break;
    }
    assert(gvtUpdated || (frameState != oldFrameState) ||
           (frameState == rob::FrameState::WAIT_SS));

    TaskPtr t = lvtTask();
    assert_msg(!t || t->noTimestamp || t->lts() >= gvt,
               "[%s] LVT task < GVT: %s < %s | %s",
               name(), t ? t->lts().toString().c_str() : "(nil)",
               gvt.toString().c_str(), t->toString().c_str());

    if (t && t->lts() == gvt) {
        // Even if it needs to restart due to an abort, its parent must have
        // committed, so we can untie it early.
        if (t->isTied()) cutTie(t);
        // This ROB now has the GVT-holding task, t.
        // If the task has an exception, replay it
        if (t->exceptioned()) exceptionedTaskAbort(t);
        DEBUG("[%s] new GVT-holding task: %s", name(), t->toString().c_str());

        // To guarantee forward progress, ensure GVT task isn't wanting for a
        // core (e.g., if cores are occupied by other speculative tasks that
        // are stalled due to nacks, full TSB, or zoom in).
        if (canRunIrrevocably(t) && runQueue.count(t) && !t->noTimestamp)
            attemptCoreEvictionFor(t);
    }

    for (TaskPtr t : execQueue) {
        if (!t->isIrrevocable() && canRunIrrevocably(t)) {
            makeIrrevocable(t);
        }
    }

    // In case the GVT update caused some ties to be cut, ensure the
    // ROBs that hold the GVT's "former"/untied children are given a
    // chance to launch a spiller. Approaches:
    // 1) iterate over all children of the task, and somehow convince
    //    the ROB container of each to notify cqNotFull waiters (that
    //    requires publicly exposing the cqNotFull variable)
    // 2) always notify one cqNotFull waiter at *every* ROB to check if
    //    a spiller can be launched. (as done below)
    // TODO we could notify all of the waiters, in case there is a lot
    // of spilling to be done... but that would involve more cycles
    // spinning.
    cqNotFull.notifyOne();

    if (gvt.app() == __MAX_TS && gvt.domainDepth()) {
        // GVT corresponded to a frame spiller or frame requeuer somewhere
        assert(gvt == gvt.domainMax());
        // Frame requeuers can be blocked until GVT reaches them.
        rqHasRunnable.notifyAll();
    }

    // For bulk-synchronous swarm, a GVT update can result in several
    // tasks being able to execute, so we notify threads waiting for
    // this condition.
    // TODO [hrlee]: We should probably use a new CV since rqHasRunnable is
    // does not aptly capture this condition.
    rqHasRunnable.notifyAll();
}


// Called periodically and atomically for all tiles (this is a
// distributed snapshot algorithm)
void ROB::sendLVTUpdate(const uint64_t epoch) {
    // A ROB with a pending SetGvt quiesces, making the whole epoch useless
    if (setGvtInProgress.hasWaiters()) return;

    ZoomType zoom;
    AckType ack;
    TimeStamp zoomRequesterTs(INFINITY_TS);
    uint32_t childIdx;
    getZoomState(zoom, zoomRequesterTs, ack, childIdx);

    TimeStamp robLvt = lvt();
    TimeStamp tsbLvt = ossinfo->tsbs[idx]->lvt();
    assignTieBreaker(tsbLvt);

    LVTUpdateMsg msg = {std::min(robLvt, tsbLvt),
                        zoomRequesterTs,
                        epoch,
                        false,
                        childIdx,
                        zoom,
                        ack};
    lvtUpdatePort->send(msg, getCurCycle());
}


void ROB::notifyDependence(const TaskPtr& t, TimeStamp depTs) {
    assert(t->container() == this);
    assert(!t->idle());
    assert(t->cts() <= depTs);  // otherwise, you should be aborting

    if (t->lts() < depTs) return; // nothing to do
    if (t->lts().hasTieBreaker() && t->lts() <= depTs) return;
    if (depTs == IRREVOCABLE_TS) return;

    // With this policy, equality is the only option left
    assert((tieBreakPolicy == rob::TBPolicy::LAZY
             && t->lts() == depTs
             && !depTs.hasTieBreaker())
           || t->isIrrevocable());

    assert_msg(!t->noTimestamp, "NOTIMESTAMP task %s dependence with depTs %s",
               t->toString().c_str(), depTs.toString().c_str());
    assert(t->running() || t->completed());
    // Spillers and requeuers do not need tiebreakers to be set after dequeue,
    // because they do not participate in conflict checking, are irrevocable,
    // and do not deepen.  See Task::lts() for discussion.
    assert(!t->isRequeuer() && !t->isSpiller());
    assert(abortQueue.count(t) == 0);

    // Set our task's tiebreaker, which requires requeuing it
    if (t->running()) execQueue.erase(t);

    if (!t->isIrrevocable()) {
        commitQueue.erase(t);
        assignTieBreaker(t->ts);
        commitQueue.insert(t);
    } else {
        // Pull this irrevocable's VT back only so far that it precedes depTs.
        // (Recall that its conflict VT remains zero)
        if (depTs.hasTieBreaker()) {
            TimeStamp depTsMinus1 = depTs;
            depTsMinus1.clearTieBreaker();
            depTsMinus1.assignTieBreaker(depTs.tiebreaker() - 1);
            // FIXME(mcj) On master branch, we must be careful that we don't pull
            // the irrevocable's VT back to a stale GVT. The GVT may move
            // forward after the next GVT update.
            t->ts = std::max(depTsMinus1, std::min(lvt(), depTs));
        } else {
            t->ts = depTs;
            assignTieBreaker(t->ts);
        }
        // By pulling back this irrevocable's VT, it might now win any conflicts
        // on which it was stalled. FIXME(mcj) stall wakeup should be handled
        // locally at each tile, via network messages. This global variable is
        // awful.
        stallers.notify(t->runningTid);
    }

    if (t->running()) execQueue.insert(t);

    // FIXME(mcj) can we strengthen this assertion to only the first clause?
    assert(t->lts() < depTs || (t->isIrrevocable() && t->lts() <= depTs));
    DEBUG("[%s] notify dependence: %s on %s",
          name(), t->toString().c_str(), depTs.toString().c_str());
}

void ROB::yieldTask(TaskPtr task, uint64_t newTsApp) {
    DEBUG("[%s] %s yielding and requeuing with timestamp %lu",
          name(), task->toString().c_str(), newTsApp);
    assert(task->isIrrevocable());
    assert(task->running());
    assert(task->isFrameRequeuer() || newTsApp >= task->lts().app());
    assert(!task->noTimestamp || task->isOrdinaryRequeuer());

    // Copy the task
    const TimeStamp newTs = task->noTimestamp ? NO_TS
                          : task->isFrameRequeuer() ? task->ts
                          : task->lts().getSameDomain(newTsApp);
    TaskPtr t = std::make_shared<Task>(
        task->taskFn, newTs,
        // Use this ROB idx as the (NOHASH) hint
        idx, true, task->nonSerialHint, task->isProducer(),
        task->isRequeuer(), task->softTs, task->runCond,
        task->runOnAbort, task->args);

    trace::taskYield(task.get(), t.get());
    // Finish this task (which commits it and frees its entry)
    finishTask(task);
    assert(task->unreachable());

    // Enqueue the copy (always succeeds since we have our own space)
    t->setDepthCounters(&depthCounters);
    t->setContainer(this);
    // TODO(mcj) de-duplicate the following. It all appears at the end
    // of ROB::receive() too)
    t->registerObserver(createTaskProfiler((void*)task->taskFn));
    enqueue(t);
    notifyTaskMapper(t, rob::TMEvent::ENQUEUE);
    task->registerObserver(
            createTaskLoadProfiler(taskMapper->getLoadStatsSummary(t.get())));
    if (profileDomains()) task->registerObserver(createDomainProfiler(*task));
}

void ROB::enqueueStolenTask(const TaskPtr& t) {
    assert(!t->isTied());
    assert(!t->noTimestamp || t->ts == ZERO_TS);
    TimeStamp ts = t->noTimestamp ? NO_TS : TimeStamp(t->lts().app());
    // Rehint
    TaskPtr task = std::make_shared<Task>(t->taskFn, ts,
        // Use this ROB idx as the (NOHASH) hint
        idx, true, t->nonSerialHint, t->isProducer(),
        t->isRequeuer(), t->softTs, t->runCond,
        t->runOnAbort, t->args);
    task->setDepthCounters(&depthCounters);
    task->setContainer(this);
    if (t->isIrrevocable()) makeIrrevocable(task);
    notifyTaskMapper(task, rob::TMEvent::ENQUEUE);
    task->registerObserver(createTaskProfiler((void*)task->taskFn));
    task->registerObserver(createTaskLoadProfiler(taskMapper->getLoadStatsSummary(task.get())));
    if (profileDomains()) task->registerObserver(createDomainProfiler(*task));
    trace::taskStolen(t.get(), task.get());
    enqueue(task);
    stealReceives.inc();
    t->markUnreachable();  // so it can be collected
}

TaskPtr ROB::createSpillerTask(rob::SpillerType type, uint64_t domainId,
                               uint64_t domainDepth) {
    // TODO argument of pointer to memory in which to dump
    // the task descriptors, but that is a later optimization
    assert(Task::ORDINARY_SPILLER_PTR);
    assert(Task::FRAME_SPILLER_PTR);

    uint64_t fnPtr;
    TimeStamp cts(INFINITY_TS);
    TimeStamp lvts = lvt();
    if (type == rob::SpillerType::ORDINARY) {
        fnPtr = Task::ORDINARY_SPILLER_PTR;
        cts = lvts.getTimeStampAtLevel(domainDepth);
        DEBUG(
            "[%s] Create ordinary spiller task domainId: %lu, domainDepth: %lu "
            "(gvt: %s, lvt: %s, cts: %s)",
            name(), domainId, domainDepth, gvt.toString().c_str(),
            lvts.toString().c_str(), cts.toString().c_str());
        assert(cts.domainDepth() == domainDepth);
        assert(cts.domainId() == domainId);

        uint64_t bestCustomSpiller = 0;
        uint64_t bestCustomSpillerValue = 0;
        for (const auto s : Task::customSpillers) {
            uint64_t value = std::count_if(
                runQueue.begin(), runQueue.end(), [s](const TaskPtr& t) {
                    return !t->isTied() && !t->hasPendingAbort() &&
                           t->taskFn == s.spillee;
                });
            if (value > bestCustomSpillerValue) {
                bestCustomSpillerValue = value;
                bestCustomSpiller = s.spiller;
            }
        }
        // TODO: make this threshold its own independent configuration knob:
        if (bestCustomSpillerValue > ossinfo->tasksPerSpiller) {
            fnPtr = bestCustomSpiller;
            DEBUG("Creating application-specific spiller task %p", fnPtr);
        }
    } else {
        DEBUG("Creating frame spiller task");
        assert(frameState == rob::FrameState::IN);
        fnPtr = Task::FRAME_SPILLER_PTR;
        // Hold LVT from passing over the to-be-enqueued frame requeuer
        cts = zoomInFrameMax;
    }

    TaskPtr spillerTask = std::make_shared<Task>(
        fnPtr, cts,
        // Use this ROB idx as the (NOHASH & non-serial) hint.  These spillers
        // never serialize, as no tasks conflict with their data.
        idx, true, true, false, false, 0, RunCondition::ANYTIME, false,
        std::vector<uint64_t>({ossinfo->tasksPerSpiller}));

    if (type == rob::SpillerType::ORDINARY)
        overflowWatch.acquireSpiller(domainId);
    else
        frameOverflowWatch.acquireSpiller();

    spillerTask->setDepthCounters(nullptr);
    spillerTask->setContainer(this);
    spillers.inc();
    notifyTaskMapper(spillerTask, rob::TMEvent::ENQUEUE);
    spillerTask->registerObserver(createTaskProfiler((void*)spillerTask->taskFn));
    spillerTask->registerObserver(createTaskLoadProfiler(taskMapper->getLoadStatsSummary(spillerTask.get())));
    if (profileDomains()) spillerTask->registerObserver(createDomainProfiler(*spillerTask));
    trace::taskCreated(spillerTask.get());
    return spillerTask;
}


void ROB::finishDoomedExecution(ThreadID tid) {
    assert(doomedTasks.left.count(tid));
    TaskPtr t = doomedTasks.left.at(tid);
    assert(t);
    DEBUG("[%d] %s::finish doomed execution %s",
          tid, name(), t->toString().c_str());
    doomedTasks.left.erase(tid);
    assert(!doomedTasks.right.count(t));
    if (t->idle()) {
        assert(!commitQueue.count(t));
        assert(!runQueue.count(t));
        // The task reached the finishAbort state before it finished executing
        // its priv region. We must make the task dispatchable now.
        if (t->isTied()) tiedTasks--;
        enqueue(t);
    }
}


TaskPtr ROB::removeUntiedTaskImpl(const uint64_t taskFn,
                                  const TimeStamp& minTS,
                                  const TimeStamp& maxTS,
                                  bool noTimestampOnly) {
    // Iterate in reverse order over the runQueue, up to and excluding
    // the minimum task of the runQueue, searching for an untied task
    // that precedes argument maxTS.
    assert(minTS <= maxTS);
    assert(minTS.domainDepth() == maxTS.domainDepth());
    const uint64_t domainDepth = minTS.domainDepth();

    // Use a O(lg n) search of the queue for the max task preceding
    // maxTS, and the min task succeeding the current spiller's
    // timestamp. This narrows the search for a candidate to remove,
    // reducing the constant on the O(n) iteration. Recall that tasks
    // are being iterated over in *reverse* timestamp order.
    // [mcj] Unfortunately the implementation details of RunQ_ty have
    // leaked out: we indeed use a boost implementation underneath.
    // Likely the ordered pointer set should cast to a
    // std::reverse_iterator
    //
    // Because requeuers are ordered strictly after worker tasks of the
    // same timestamp, I have to be very careful to find the real upper
    // bound of maxTS. Specifically I use lower_bound on a timestamp
    // that dominates maxTS (and all requeuers w maxTS). This is way too
    // complicated :s
    // FIXME(dsm): Use multi-index::range()
    auto start =
             boost::make_reverse_iterator(runQueue.lower_bound(
                  std::make_tuple(std::cref(maxTS), false)));
    auto end =
             boost::make_reverse_iterator(runQueue.lower_bound(
                         std::make_tuple(std::cref(minTS), false)));

    // For liveness: we don't want to spill the minimum task.
    // Spilling that task would certainly induce a swarm::requeuer(...) of
    // the spilled tasks, defeating the utility of spilling them,
    // and potentially entering an infinite loop of
    // spilling/requeuing.
    if (end == runQueue.rend() && start != runQueue.rend()) {
        end = std::prev(runQueue.rend());
    }
    TaskPtr lvtt = lvtTask();
    auto isRemovableTask = [domainDepth, noTimestampOnly, lvtt, taskFn]
            (const TaskPtr& t) {
                return !t->isTied() && !t->hasPendingAbort()
                        && (taskFn == t->taskFn || !taskFn)
                        // FIXME(mcj) HACK due to priority inversion of
                        // nontimestamped tasks and the GVT/LVT.
                        && (t != lvtt)
                        && (!noTimestampOnly || t->noTimestamp)
                        && (t->lts().domainDepth() == domainDepth);
            };
    auto candidate = std::find_if(start, end, isRemovableTask);

    if (candidate != end) {
        TaskPtr t = *candidate;
        assert(t != runQueue.min());
        assert(t->idle());
        assert(!t->isSpiller());
        assert(t->lts().app() >= minTS.app());
        assert(t->lts().app() <= maxTS.app());
        runQueue.erase(t);
        DEBUG("[%s] remove %s by %s", name(),
              t->toString().c_str(),
              GetCurTask()->toString().c_str());
        assert(!taskQueueIsFull());
        notifyTaskMapper(t, rob::TMEvent::SPILL);
        return t;
    } else {
        return nullptr;
    }
}


TaskPtr ROB::removeOutOfFrameTasks() {
    if (frameState != rob::FrameState::IN) return nullptr;

    auto candidate = std::find_if(
        runQueue.begin(), runQueue.end(), [&](const TaskPtr& t) {
            return ((t->ts > zoomInFrameMax) &&
                    (!t->isTied()) && !t->hasPendingAbort());
        });

    if (candidate != runQueue.end()) {
        TaskPtr t = *candidate;
        //assert(t != runQueue.min()); // Is this true?
        assert(!t->isSpiller());
        assert(t->idle());
        runQueue.erase(t);
        DEBUG("[%s]: remove %s", name(), t->toString().c_str());
        assert(!taskQueueIsFull());
        // FIXME  Check all TaskMapper calls.
        notifyTaskMapper(t, rob::TMEvent::SPILL);
        return t;
    } else {
        return nullptr;
    }
}


TaskPtr ROB::stealTask(bool lifo) {
    auto tl = [](const TaskPtr& t) {
        return !t->isTied() && !t->hasPendingAbort();
    };
    // We can't steal the LVT task
    // TODO(mcj) handle the case that the LVT is in the execQ
    assert(runQueue.size() > 1);

    TaskPtr t;
    if (lifo) {
        // Don't steal the LVT task
        auto end = std::prev(runQueue.rend());
        auto it = std::find_if(runQueue.rbegin(), end, tl);
        assert(it != end);
        t = *it;
    } else {
        // Don't steal the LVT task
        auto begin = std::next(runQueue.begin());
        auto it = std::find_if(begin, runQueue.end(), tl);
        assert(it != runQueue.end());
        t = *it;
    }
    assert(t && t->idle());
    runQueue.erase(t);
    notifyTaskMapper(t, rob::TMEvent::STEAL);
    stealSends.inc();
    DEBUG("[%s] Stolen: %s", name(), t->toString().c_str());
    return t;
}


void ROB::attemptCoreEvictionFor(const TaskPtr& task) {
    // If this task beats every currently-running task, nuke one of them.
    // TODO(victory): Consider less-aggressive alternatives: set a timer, then
    // nuke one, or fire a periodic event that aborts stalled running tasks.
    // TODO(victory): Consider more-aggressive alternatives: can we prevent any
    // core/thread from being hogged by a single misspeculating task for an
    // arbitrarily long time after less-speculative tasks are enqueued?
    // TODO(victory): Maybe we should take into account hint serialization:
    // if the current task is less speculative than a same-hint running task,
    // perhaps we should immediately abort the speculative running task to run
    // the less speculative one.

    // FIXME(mcj) There's no way the interaction between
    // cantspec/mustspec/irrevocables and the threadCountController is stable
    // TODO(mcj) threadCountController should be embedded in the execQueue; they
    // seem highly related
    uint64_t nrunning = (execQueue.size() +
          threadCountController->getNumBlockedThreads());

    if (nrunning == execQueue.capacity()
            && std::all_of(execQueue.begin(), execQueue.end(),
                [task] (const TaskPtr& t) { return t->cts() > task->cts(); })) {
        // All cores are executing speculative tasks, so to
        // guarantee forward progress, abort one of them. We don't
        // know whether they would stall, or have read bad memory
        // and are in an infinite loop.
        TaskPtr candidate = execQueue.max();
        assert(!candidate->isIrrevocable());
        DEBUG("[%s] Abort %s for resource: Core (lower timestamp task enqueued)",
              name(), candidate->toString().c_str());
        resourceConstrainedAbort(candidate);
    }
}


void ROB::enqueue(const TaskPtr& task) {
    DEBUG("%s::enqueue %s", name(), task->toString().c_str());

    assert(!taskQueueIsFull());
    assert(task->noTimestamp || task->lts() >= gvt);
    assert(task->noTimestamp || !task->isTied() ||
           (task->parent->cts() < task->lts()));
    // A requeuer task's hint is equal to the ROB::idx to which it's
    // enqueued
    assert(!task->isRequeuer() || task->hint == idx);
    TimeStamp oldLvt = lvt();

    if (task->noTimestamp) {
        // The task must have been holding the GVT from reaching infinity, but
        // not from passing over any timestamped tasks.
        assert(task->ts == NO_TS);
        // Priotize the task for dequeue and deprioritize the task for spills.
        task->ts = ZERO_TS;
    }

    enqueues.inc();
    runQueue.insert(task);
    if (task->isTied()) tiedTasks++;

    // dsm: If this task is not runnable, it's pointless to nuke running tasks;
    // if the task is noTimestamp, aborting a running task leads to bad
    // priority inversion (often nukes the parent, since our "timestamp" is 0!)
    bool peaceful = (task->cantSpeculate() && !canRunIrrevocably(task)) ||
                    task->noTimestamp;
    if (!peaceful) attemptCoreEvictionFor(task);

    // [mcj] Wake a waiting thread on every enqueue, regardless of space
    // in the commit queue. Dequeue is currently responsible to ensure
    // there is space for mission-critical tasks.
    //
    // Wake a thread even if there isn't enough steady work to warrant
    // it (i.e. that same thread might immediately sleep).
    // 1) If we only wake threads based on when the runQueue was
    //    previously empty, with the multi-versioned PQ, I haven't
    //    reasoned that this strategy won't leave threads sleeping too
    //    long. From experiments, I think many threads would never wake
    //    up due to the value of curCycle hiding the size of the
    //    runQueue
    // 2) A potentially safer optimization to avoid overzealous wakeup
    //    follows:
    //    - let Q be the number of elements in the runQueue and T be the
    //      number of active (non-waiting) threads
    //    - on enqueue, wake up (Q-T) threads
    rqNotEmpty.notifyOne();
    // Notify waiters on a commit queue slot, that a task was enqueued,
    // since the new task might induce a commit queue eviction,
    // permitting them to proceed.
    cqNotFull.notifyOne();

    // The new task might be useful to any threads who were throttled
    rqHasRunnable.notifyOne();

    // If the LVT dropped, a stalled irrevocable might now be able to
    // win against a speculative task that completed
    if (lvt() < oldLvt) wakeStalledIrrevocables();
}

// TODO this method is really like a join on the runQueue and
// execQueue, and is the reason for the ridiculous iterator interface on
// the erasable prio queue.
// mcj isn't thrilled that this method isn't abstracted away, but
// it depends on state of the runQ and execQ, and could even risk
// deadlocking the system, so it stays fairly global for now...
// dsm: This is an implementation detail.
std::pair<TaskPtr, rob::Stall> ROB::taskToRun(ThreadID tid) {
    if (!threadCountController->handleDequeue(tid))
        return std::make_pair(nullptr, rob::Stall::THROTTLE_MT);

    if (runQueue.empty()) return std::make_pair(nullptr, rob::Stall::EMPTY);

    // [hrlee] Bulk-synchronous swarm does not dequeue tasks with TS greater
    // than the GVT
    if (bulkSynchronousTasks && runQueue.min()->lts().app() > gvt.app()) {
        DEBUG("[%s] Not dequeueing task with ts %s due to gvt at %s.",
              name(), runQueue.min()->lts().toString().c_str(),
              gvt.toString().c_str());
        // TODO [hrlee]: rqHasRunnable CV does not exactly capture this
        // condition. Come up with a new/better CV.
        return std::make_pair(nullptr, rob::Stall::THROTTLE);
    }

    if (shouldThrottle()) return std::make_pair(nullptr, rob::Stall::THROTTLE);

    // With some Swarm extensions, there may be idle tasks in the task queue
    // that are not runnable.  We ignore/skip these ineligible runQueue entries.
    // TODO(victory): Repeatedly skipping over non-runnable tasks is
    // inefficient.  We should remove them from the runQueue, (re)inserting
    // them into the runQueue only when they actually become runnable.
    auto isRunnable = [this](const TaskPtr& t) {
        if (t->cantSpeculate() && !canRunIrrevocably(t)) return false;
        // Frame requeuers are always CANTSPEC with a special timestamp.
        assert(!t->isFrameRequeuer() || t->ts == gvt)

        // Don't run tasks that are going out of frame.
        assert(!t->isSpiller());
        if (frameState == rob::FrameState::IN
            && t->ts >= zoomInFrameMax) {
            // Will be spilled or aborted in order for the zoom-in to complete.
            return false;
        } else if (frameState == rob::FrameState::OUT
                   && t->ts.domainDepth() > getFrameMaxDepth()) {
            // Too-deep domain will be discarded by the domain creator aborting.
            // See ROB::abortOutOfZoomOutFrameTasks().
            assert(t->isTied() || t->hasPendingAbort());
            return false;
        }

        return true;
    };

    // If there is only one remaining commit queue slot, then do not enable
    // underflow, which could prioritize non-lvt tasks that fill up resources
    // while the lvt/gvt task waits in the runQueue, leading to deadlock.
    if (underflowThreshold &&
        (commitQueue.capacity() - commitQueue.size() > 1) &&
        (runQueue.min()->lts() <
         (execQueue.empty() ? INFINITY_TS : execQueue.min()->lts()))) {
        // dsm: Hint serialization is incompatible with underflow for now.
        // It could be supported but it'd be a pain to implement.
        assert(!serializeSpatiallyEqualTasks);

        // Find all tasks with the same timestamp, including all producers
        TimeStamp ubTs = runQueue.min()->lts();
        ubTs.clearTieBreaker();
        auto ub = runQueue.upper_bound(std::make_tuple(std::cref(ubTs), true));
        ub = std::prev(ub);

        // Manually check if std::distance(rq.begin(), ub) < underflow.
        // std::distance is a linear operation, so below we bound the number
        // of checks to the underflowThreshold.
        uint32_t sameTsTasks = 0ul;
        for (auto it = runQueue.cbegin();
             sameTsTasks < underflowThreshold && it != ub;
             sameTsTasks++, it++) {}
        // If there are fewer same-timestamp tasks than the underflow
        // threshold, prioritize a producer
        if (sameTsTasks < underflowThreshold && ub != runQueue.end()
            && (*ub)->isProducer() && isRunnable(*ub)) {
            assert(runQueue.min()->lts().app() == (*ub)->lts().app());
            return std::make_pair(*ub, rob::Stall::NONE);
        }
    }

    TaskPtr selection = nullptr;
    if (!serializeSpatiallyEqualTasks) {
        // Avoid dequeueing not-runnable cantSpec tasks
        for (const TaskPtr& candidate : runQueue) {
            if (isRunnable(candidate)) {
                selection = candidate;
                break;
            }
        }
    } else {
        // Find the set of tasks that are executing at this
        // tile whose hints may cause a conflict.
        // don't record the timestamps for nonserializing hint tasks, these tasks
        // should not influence serialization
        struct TSRange { TimeStamp min; TimeStamp max; };
        std::unordered_map<uint64_t, TSRange> hintMap;
        for (const TaskPtr& t : execQueue) {
            if (!t->nonSerialHint) {
                auto it = hintMap.find(t->hint);
                if (it == hintMap.end()) {
                    hintMap.insert(std::make_pair(
                        t->hint, TSRange{t->cts(), t->cts()}));
                } else {
                    it->second.min = std::min(t->cts(), it->second.min);
                    it->second.max = std::max(t->cts(), it->second.max);
                }
            }
        }

        // Find the first idle candidate that doesn't conflict with a
        // hint in the conflictingHints set.
        uint32_t numSkipped = 0;
        for (const TaskPtr& candidate : runQueue) {
            assert(!candidate->isSpiller());
            // Skip non-runnable tasks early (no need to even count them)
            if (!isRunnable(candidate)) {
                continue;
            }
            // non-serializing hint tasks can run in parallel
            // with tasks of the same hint
            if (candidate->nonSerialHint) {
                selection = candidate;
                break;
            }

            bool earlierMatch = false;
            bool laterMatch = false;
            auto it = hintMap.find(candidate->hint);
            if (it != hintMap.end()) {
                earlierMatch = it->second.min <= candidate->cts();
                laterMatch = it->second.max > candidate->cts();
                assert_msg(earlierMatch || laterMatch,
                        "candidate %s earlier %s later %s",
                        candidate->toString().c_str(),
                        it->second.min.toString().c_str(),
                        it->second.max.toString().c_str());
            }
            // dsm: Only skip this candidate (serializing by spatial hint)
            // if there is an earlier-task match. Don't consider later matches.
            // If a higher-timestamp task is already running, we might as well
            // abort it early.
            if (!earlierMatch) {
                selection = candidate;
                break;
            }

            numSkipped += 1;
            DEBUG("[%s] Skipping candidate ts %s hint %ld [%d/%d]",
                  name(), candidate->cts().toString().c_str(),
                  candidate->hint, earlierMatch, laterMatch);
        }
        DEBUG("[%s] Skipped %u tasks due to hint serialization.", name(),
              numSkipped);
        numTasksSkippedForHintSerializationPerDeq.push(numSkipped);
    }
    if (selection) {
        DEBUG("[%s] Selected highest-priority runnable task: %s", name(),
              selection->toString().c_str());
        return std::make_pair(selection, rob::Stall::NONE);
    } else {
        DEBUG("[%s] Give up, tasks in runQueue are not runnable.", name());
        return std::make_pair(nullptr, rob::Stall::THROTTLE);
    }
}

// Adaptive dequeue-time throttling. Seeks to avoid dequeuing any task too far
// in advance to avoid over-speculation. It is not selective and does not seek
// to skip tasks or run-them out of timestamp order.
//
// Implementations of different throttling policies may use different aspects
// of the LVT task (e.g., aborts) or global state (e.g., commit queue
// occupancy) to decide whether to throttle.
//
// This method is called on every taskToRun(), so implementations must ensure
// they don't end up always throttling, which can cause deadlock. For example,
// one approach would be to throttle based on commit queue utilization,
// ensuring that throttling doesn't happen if the commit queue is nearly empty.
bool ROB::shouldThrottle() {
    if (!adaptiveThrottle) return false;
    // TODO: Other policies, task/pc-specific throttling, etc.
    // Restrict throttling based on tentative commit queue depth of rq.min task
    auto numAborts = runQueue.min()->abortCount();
    if (!numAborts) return false;
    auto threshold = (16 - std::min(14u, numAborts)) * commitQueue.capacity() / 16;
    if (commitQueue.size() > threshold) {
        auto it = commitQueue.cbegin();
        std::advance(it, threshold);
        if ((*it)->lts() < runQueue.min()->lts()) return true;
    }
    return false;
}

void ROB::commitTasks() {
    // "GVT can thus be viewed as a moving commitment horizon: any event
    // with virtual time less than GVT cannot be rolled back, and may be
    // irrevocably committed with safety."
    // David R. Jefferson. 1985. Virtual time. ACM Trans. Program. Lang.
    // Syst. 7, 3. (Section 4.3)
    while (!commitQueue.empty()
            && commitQueue.min()->completed()
            && commitQueue.min()->lts() < gvt) {
        commitTask(commitQueue.min());
    }

    if (!commitQueue.empty()) {
        TaskPtr t = commitQueue.min();
        // The GVT task can be committed iff
        // * it's exception-free and
        // * an abort isn't impending "from the future" (notifyAbort is
        //   called deep within an AbortHandler cascade, i.e. at a cycle
        //   very from curCycle).
        if (t->isTied() && t->lts() == gvt) cutTie(t);
        if (t->completed() && canRunIrrevocably(t)) commitTask(t);
    }

    // Due to adaptive throttling, task commits may make rq tasks runnable
    if (adaptiveThrottle) rqHasRunnable.notifyAll();

    // Wake up all threads if the program is terminating.
    if (terminate()) {
        assert(runQueue.empty()
                && execQueue.empty()
                && commitQueue.empty()
                && abortQueue.empty());
        DEBUG("ROB done, waking up all waiters");
        rqNotEmpty.notifyAll();
        threadCountController->notifyAll();
    }
}


void ROB::makeIrrevocable(const TaskPtr& t) {
    DEBUG("[%s] make irrevocable %s", name(), t->toString().c_str());
    assert(t->idle() || t->running());
    if (t->idle()) {
        t->makeIrrevocable();
    } else if (t->running() && t->lts() == gvt) {
        // May be waiting on a full TSB; can now enqueue untied
        ossinfo->tsbs[idx]->wakeup(t->runningTid);
        // Free all parent/children ties, since t will never abort
        sendCutTieMessages(t);
        tileCD->clearReadSet(*t);
        tileCD->clearWriteSet(*t);
        commitQueue.erase(t);
        cqNotFull.notifyOne();
        t->makeIrrevocable();

        // Due to adaptive throttling, task commits may make rq tasks runnable
        if (adaptiveThrottle) rqHasRunnable.notifyAll();
    } else {
        // TODO(mcj) read-set validation
    }
}


void ROB::sendCutTieMessages(const TaskPtr& task) {
    for (const TaskPtr& child : task->children) {
        if (child->runOnAbort) {
            // RunOnAbort tasks are held in the TSB; when the parent
            // commits, we must discard them.
            assert(child->idle() && !child->hasContainer());
            ossinfo->tsbs[idx]->discard(child);
            trace::taskDiscarded(child.get());
            child->markUnreachable();
        } else if (child->cantSpeculate()) {
            // Tied CANTSPEC tasks are held in the TSB; when the parent
            // commits, they can be released to the ROBs, so they never need
            // a separate cut-tie message.
            DEBUG("[%s] Cutting tie to child %s immediately",
                  name(), child->toString().c_str());
            assert(child->idle() && !child->hasContainer());
            child->cutTie();
            ossinfo->tsbs[idx]->release(child, getCurCycle());
        } else if (child->hasContainer()) { // child has been enqueued to its ROB
            DEBUG("[%s] Sending cut tie message to child %s",
                  name(), child->toString().c_str());
            cutTiePorts[child->container()->getIdx()]->send(
                CutTieMsg{child}, getCurCycle());
        } else { // child is still in local TSB; cut tie immediately
            DEBUG("[%s] Cutting tie to child %s immediately",
                  name(), child->toString().c_str());
            assert(child->idle());
            child->cutTie();
        }
    }
    task->children.clear();
}


void ROB::exceptionedTaskAbort(TaskPtr t) {
    // [dsm]: We do really want a hazard abort fired on this thing to
    // requeue it.
    filtered_info("[%s-%ld] Replaying task 0x%lx ts %lu with exception: %s",
         name(), getCurCycle(), t->taskFn,
         t->lts().app(),
         t->exceptionReason.c_str());
    assert(!t->isIrrevocable());
    assert(t->container()->getIdx() == getIdx());
    bool success =
        ossinfo->abortHandler->abort(t, getIdx(), Task::AbortType::HAZARD,
                                     getCurThreadLocalCycle() + 1, -1, nullptr);
    // TODO(mcj) upgrade the following to never be idle?
    assert(!success || !t->idle());
}


void ROB::attemptTaskQueueReservation() {
    if (overflowWatch.neededSpillers()) {
        // Tasks at this ROB can be spilled to make space.
        // Ensure some thread is running or can run a spiller,
        // then stall the enqueuer.
        // N.B. This strategy won't work on a 1-thread-per-ROB system,
        // as the enqueuer could be using the sole thread. Preventing
        // any spiller from running
        DEBUG("[%s] tasks at ROB can be spilled to make space", name());
        if (overflowWatch.anySpillersOutstanding()) {
            DEBUG("[%s] one of the cores is running a spiller,"
                  " let it clear TQ space", name());
            // N.B. if the spiller isn't able to reserve the GVT holder a
            // slot, the former will wake up the latter task so that we can try
            // to enqueue again, and re-evaluate the current state of the queue.
        } else if (!execQueue.full()) {
            // Not all cores have running tasks, so a spiller can run on one
            // of them. Wake a thread blocked on any of the following three
            // condition variables, to run a spiller.
            DEBUG("[%s] execQueue not full", name());
            threadCountController->notifyOne();
            if (commitQueue.full()) cqNotFull.notifyOne();
            else rqHasRunnable.notifyOne();
        } else {
            DEBUG("[%s] all cores running tasks,"
                  " none of which is a spiller", name());
            // All cores are running tasks, none of which is a
            // spiller. Abort one of them to guarantee fwd progress.
            // FIXME(dsm): Abort highest-ts/tied?
            auto it = std::find_if(
                execQueue.begin(), execQueue.end(),
                [](const TaskPtr& t) { return !t->isIrrevocable(); });
            if (it != execQueue.end()) {
                TaskPtr t = *it;
                DEBUG("[%s] Abort %s for resource: Core (evicting for spiller)",
                      name(), t->toString().c_str());
                resourceConstrainedAbort(t);
            }  // else we can't abort a thing
        }
    }
}


void ROB::notifyToBeBlockedThread(ThreadID tid) {
    // If tid is not in any of these, nothing will happen.
    // If tid is present, it will get woken up and get blocked by
    // the thread count controller.
    rqNotEmpty.notify(tid);
    rqHasRunnable.notify(tid);
    cqNotFull.notify(tid);
}

const TimeStamp& ROB::commitQueueMedian() const {
    auto median = std::next(commitQueue.cbegin(), commitQueue.size() / 2);
    return (*median)->lts();
}


uint32_t ROB::cqQuartile(const TimeStamp& ts) const {
    return 0;
    // Disable temporarily
#if 0
    if (!commitQueue.empty()) {
        // Constant- or log-n-time checks at the boundaries
        if (ts < commitQueue.min()->lts()) return 0;
        if (ts > commitQueue.max()->lts()) return 3;

        // On a bidirectional iterator (i.e. a sorted set) std::next has
        // linear complexity, so call sparingly, and memoize prior work
        uint32_t quartile = 0;
        auto it = commitQueue.cbegin();
        while (quartile < 3) {
            it = std::next(it, commitQueue.size() / 4);
            if (ts < (*it)->lts()) return quartile;
            quartile++;
        }
        return quartile;
    } else return 0;
#endif
}


uint64_t ROB::access(const AbortReq& req, AbortAck& ack, uint64_t cycle) {
    assert(req.tileIdx == idx);
    DEBUG("[%s][%ld]: parent->child abort for %s",
          name(), cycle, req.task->toString().c_str());
    // Consequences of cutting ties here:
    // 1) The ROB's tiedTasks count is reduced, allowing a new tied task to
    // sneak into its slot. This is likely desirable, as this task certainly
    // will be evicted soon. Unfortunately it may trigger a spiller that finds
    // it can't remove this untied-but-aborting task.
    // 2) Does this affect profiling stats related to the parent pointer?
    // 3) Does this break parent smart pointers? Probably not, now that the
    // child won't be requeued, they have no need to communicate anymore with
    // the parent (other than the ACK sent at the end of this method).
    cutTie(req.task);
    ossinfo->abortHandler->abort(req.task, idx, Task::AbortType::PARENT, cycle,
                                 req.depth, nullptr);

    // Once this child has affected (held back) its tile's LVT, send an
    // immediate ACK upon abort message. This tile can start the rollback
    // procedure independently. The parent doesn't need to wait for the child to
    // abort, as the child will now hold back its LVT if needed.
    return cycle + 1;
}


uint64_t ROB::abortChildTasks(TaskPtr p, uint32_t depth, const uint64_t cycle) {
    uint64_t respCycle = cycle;
    // Add a 1-cycle latency per child abort message (otherwise, with local
    // aborts we can have a chain of parent->child aborts that unrealistically
    // all trigger at the same cycle).
    uint64_t count = 1;
    for (TaskPtr child : p->children) {
        if (child->runOnAbort) {
            // RunOnAbort tasks are held in the TSB, and an abort makes them
            // untied and releases them to the ROBs.
            assert(child->idle() && !child->hasContainer());
            child->cutTie();
            ossinfo->tsbs[idx]->release(child, cycle);
        } else if (child->cantSpeculate()) {
            // Tied CANTSPEC tasks are held in the TSB, so when the parent
            // aborts, they can simply be discarded without network messages.
            assert(child->idle() && !child->hasContainer());
            ossinfo->tsbs[idx]->discard(child);
            trace::taskDiscarded(child.get());
            child->markUnreachable();
        } else {
            trace::childAbort(child.get());
            uint32_t tileId =
                child->hasContainer() ? child->container()->getIdx() : idx;
            AbortReq req = {child, tileId, depth + 1};
            AbortAck ack;
            // TODO(mcj) what if the child is local? Do we want a latency like
            // the old localRobAccessDelay_ which factored in the # of cores
            // per tile?  What if it's in the local TSB?
            uint64_t childRespCycle =
                childAbortPorts[tileId]->access(req, ack, cycle + count);
            respCycle = std::max(respCycle, childRespCycle);
            count++;
        }
    }
    // Because we atomically send/receive abort messages, we can clear the
    // children set now. When the messages are non-atomic, we'll have to remove
    // each child from the set individually, and only permit the parent's CQ
    // slot to be released once all children are removed from the set.
    p->children.clear();

    assert(respCycle > cycle || p->children.empty());
    return respCycle;
}


TaskPtr ROB::lvtTask() const {
    TaskPtr t = TaskPtr(nullptr);
    for (const TaskPtr& task : runQueue) {
        if (!task->noTimestamp) {
          t = task;
          break;
        }
    }
    if (!execQueue.empty() && (!t || execQueue.min()->lts() < t->lts())) {
        t = execQueue.min();
    }
    if (!abortQueue.empty() && (
            !t ||
            abortQueue.min()->lts() < t->lts() ||
            // Irrevocable tasks can set their VT to match the local
            // LVT, so it's possible that several tasks match the GVT,
            // and the speculative task-of-interest is in the abortQ
            (abortQueue.min()->lts() == t->lts() && t->isIrrevocable())
            )) {
        t = abortQueue.min();
    }
    // Let any doomed priv-mode task hold back the LVT
    for (const auto& pair : doomedTasks) {
        TaskPtr doomed = pair.right;
        if (!t || (doomed->lts() < t->lts()) ||
                  (doomed->lts() == t->lts() && t->isIrrevocable())) {
            t = doomed;
        }
    }
    if (!t && !runQueue.empty()) {
        t = runQueue.min();
        assert(t->noTimestamp);
    }
    return t;
}


void ROB::getZoomState(ZoomType& zoom, TimeStamp& zoomRequesterTs,
                  AckType& ack, uint32_t& chIdx) const {
    TimeStamp inRequesterTs(INFINITY_TS);
    TimeStamp outRequesterTs(INFINITY_TS);
    inRequesterTs = zoomRequester.getZoomInRequesterTs();
    outRequesterTs = zoomRequester.getZoomOutRequesterTs();
    chIdx = childIdx;

    bool isSteadyState = (frameState == rob::FrameState::STEADY_STATE);
    bool ackZoomIn = false;
    bool ackZoomOut = false;

    // 1. Check if there are any tasks outside of the contracted frame.
    // Set ackZoomIn if there are no such tasks.
    if (frameState == rob::FrameState::IN) {
        auto isOutOfBounds = [this](const TaskPtr& t) {
            return ((t->ts > zoomInFrameMax) || t->isFrameSpiller());
        };
        bool rqPresent =
            any_of(runQueue.begin(), runQueue.end(), isOutOfBounds);
        bool eqPresent =
            any_of(execQueue.begin(), execQueue.end(), isOutOfBounds);
        bool cqPresent =
            any_of(commitQueue.begin(), commitQueue.end(), isOutOfBounds);

        DEBUG_GVT("[%s] IN rqPresent: %s, eqPresent: %s, cqPresent: %s",
              name(), rqPresent ? "yes" : "no",
              eqPresent ? "yes" : "no", cqPresent ? "yes" : "no");
        ackZoomIn = !(rqPresent || eqPresent || cqPresent);
    }

    // 2. Check if there are any tasks too deep for the expanded frame.
    // Set ackZoomOut if there are no such tasks.
    if (frameState == rob::FrameState::OUT) {
        uint32_t depth = getFrameMaxDepth();
        bool rqPresent = any_of(runQueue.begin(), runQueue.end(),
                                [&depth](const TaskPtr& t) {
                                    return ((t->ts.domainDepth() > depth));
                                });
        bool eqPresent = any_of(execQueue.begin(), execQueue.end(),
                                [&depth](const TaskPtr& t) {
                                    return ((t->ts.domainDepth() > depth));
                                });
        bool cqPresent = any_of(commitQueue.begin(), commitQueue.end(),
                                [&depth](const TaskPtr& t) {
                                    return ((t->ts.domainDepth() > depth));
                                });

        DEBUG_GVT("[%s] OUT rqPresent: %s, eqPresent: %s, cqPresent: %s",
              name(), rqPresent ? "yes" : "no",
              eqPresent ? "yes" : "no", cqPresent ? "yes" : "no");
        ackZoomOut = !(rqPresent || eqPresent || cqPresent);
    }

    // 3. Set the frame management params for the LVT msg after
    // a couple of sanity checks
    assert(!(ackZoomIn && ackZoomOut));
    assert(
        !(isSteadyState && (ackZoomOut || ackZoomIn)));

    if (isSteadyState) ack = AckType::SS;
    else if (ackZoomIn || ackZoomOut) ack = AckType::ZOOM;
    else ack = AckType::NONE;

    if ((inRequesterTs != INFINITY_TS) || (outRequesterTs != INFINITY_TS)) {
        zoom = inRequesterTs < outRequesterTs ? ZoomType::IN : ZoomType::OUT;
        zoomRequesterTs = inRequesterTs < outRequesterTs ? inRequesterTs : outRequesterTs;
    } else {
        zoom = ZoomType::NONE;
        zoomRequesterTs = INFINITY_TS;
    }

    // 4. Helpful debug message
    DEBUG_GVT(
        "[%s] Zoom state query result: %s, "
        " zoomRequesterTs: %s, ack: %s",
        name(), ((zoom == ZoomType::NONE)
                     ? "NONE"
                     : (zoom == ZoomType::IN) ? "IN" : "OUT"),
        zoomRequesterTs.toString().c_str(),
        ((ack == AckType::SS)
             ? "SS"
             : (ack == AckType::ZOOM && ackZoomIn)
                   ? "IN"
                   : (ack == AckType::ZOOM && ackZoomOut) ? "OUT"
                                                        : "NONE"));
}


void ROB::ensureFrameSpillingCanHappen() {
    if (frameState != rob::FrameState::IN) return;
    // This follows a similar pattern to attemptTaskQueueReservation()
    if (frameOverflowWatch.spillerShortage(zoomInFrameMax)) {
        DEBUG("[%s] tasks at ROB need to be frame spilled", name());
        if (!execQueue.full()) {
            DEBUG("[%s] execQueue not full", name());
            threadCountController->notifyOne();
            if (commitQueue.full()) cqNotFull.notifyOne();
        } else {
            DEBUG(
                "[%s] All cores occupied, but need to run a frame "
                "spiller. Aborting one of the running tasks.",
                name());
            // TODO(victory): Find and abort highest-timestamp task?
            auto it = std::find_if(
                execQueue.begin(), execQueue.end(),
                [](const TaskPtr& t) { return !t->isIrrevocable(); });
            if (it != execQueue.end()) {
                TaskPtr t = *it;
                DEBUG("[%s] Abort %s for resource: "
                      "Core (evicting for frame spiller)",
                      name(), t->toString().c_str());
                resourceConstrainedAbort(t);
            }  // else we can't abort a thing
        }
    }
}


void ROB::abortOutOfZoomOutFrameTasks(uint32_t maxDepth) {
    DEBUG("[%s] Going to abort too-deep tasks due to zoom out, maxDepth: %u",
          name(), maxDepth);
    std::vector<TaskPtr> tasksToAbort;

    // In order to eliminate all tasks in too-deep domains,
    // we will abort domain creators.
    // If a frame expansion has been triggered, then the
    // instigating task is the gvt task, and is irrevocable.
    // All remaining too-deep domain creators that we want to abort
    // will not be irrevocable.  This is safe in part because
    // irrevocable spillers and requeuers do not deepen.
    // Note that this may not work if Espresso's irrevocable
    // tasks deepen.
    // [victory] This also assumes maxFrameDepth >= 3. (If maxFrameDepth == 2,
    // the GVT task that is requesting the zoom-out might be the creator of a
    // too-deep domain.)
    auto abortIfNeeded = [this, maxDepth, &tasksToAbort](TaskPtr t) {
        if (t->ts.domainDepth() == maxDepth && t->hasSubdomain()) {
            DEBUG("[%s] Abort domain creator %s", name(),
                  t->toString().c_str());
            assert(!t->isIrrevocable());
            assert(t->container()->getIdx() == getIdx());
            tasksToAbort.push_back(t);
        } else if (t->ts.domainDepth() > maxDepth && !t->isIrrevocable()) {
            DEBUG("[%s] Abort too-deep task %s", name(),
                  t->toString().c_str());
            // Too-deep domain will be discarded by the domain creator aborting.
            assert(t->isTied() || t->hasPendingAbort());
            assert(t->container()->getIdx() == getIdx());
            tasksToAbort.push_back(t);
        }
    };
    std::for_each(execQueue.begin(), execQueue.end(), abortIfNeeded);
    std::for_each(commitQueue.begin(), commitQueue.end(), abortIfNeeded);

    for (auto& t : tasksToAbort) zoomingAbort(t);
}


void ROB::abortOutOfZoomInFrameTasks(TimeStamp zoomInFrameMax) {
    DEBUG("[%s] Going to abort tasks with timestamp > bound: %s", name(),
          zoomInFrameMax.toString().c_str());
    std::vector<TaskPtr> tasksToAbort;

    // Abort task if its ts > zoomInFrameMax, or if
    // it has enqueued a child to parent domain and its
    // depth is just one below the current root domain (i.e.
    // same depth as zoomInFrameMax).
    auto needsAbortBeforeZoomIn =
        [&zoomInFrameMax](const TaskPtr& t) {
            return ((t->ts > zoomInFrameMax) ||
                    (t->hasEnqueuedChildToSuperdomain() &&
                     t->ts.domainDepth() ==
                         zoomInFrameMax.domainDepth())) &&
                   (!t->isIrrevocable());
        };

    // Walk commit queue and exec queue, and abort relevant tasks
    std::for_each(commitQueue.begin(), commitQueue.end(),
                  [this, &needsAbortBeforeZoomIn, &tasksToAbort](TaskPtr t) {
                      if (needsAbortBeforeZoomIn(t)) {
                          assert(t->container()->getIdx() == getIdx());
                          DEBUG("[%s] [cq] Going to abort %s",
                                name(), t->toString().c_str());
                          tasksToAbort.push_back(t);
                      }
                  });
    std::for_each(execQueue.begin(), execQueue.end(),
                  [this, &needsAbortBeforeZoomIn, &tasksToAbort](TaskPtr t) {
                      if (needsAbortBeforeZoomIn(t)) {
                          assert(t->container()->getIdx() == getIdx());
                          DEBUG("[%s] [eq] Going to abort %s",
                                name(), t->toString().c_str());
                          tasksToAbort.push_back(t);
                      }
                  });

    for (auto& t : tasksToAbort) zoomingAbort(t);
}


bool ROB::deepen(TaskPtr t, ThreadID tid, uint64_t maxTS) {
    // With lazy tiebreaking or irrevocable tasks, must set tiebreak now.
    // This notification has to happen before querying zoomRequester.
    notifyDependence(t->ptr(), t->lts());

    if (zoomRequester.requestZoomInIfNeeded(t, tid)) {
        // TODO(victory): Instead of having many tasks blocked on cores all
        // requesting zoom-in, we should probably just abort some tasks that
        // need a zoom-in, if they are speculative and we have other tasks we
        // can run in the mean time.  Maybe we should set a timer?
        return false;
    } else {
        t->deepen(maxTS);
        return true;
    }
}


void ROB::dumpQueues() const {
    info("[%s] gvt %s", name(), gvt.toString().c_str());
    for (auto& t : runQueue)
        info("[%s]  [rq]  %s", name(), t->toString().c_str());
    for (auto& t : commitQueue)
        info("[%s]  [cq]  %s", name(), t->toString().c_str());
    for (auto& t : execQueue) {
        ThreadID tid = t->runningTid;
        info("[%s]  [exe] %s", name(),
             (tid == INVALID_TID) ? t->toString().c_str()
                                  : GetThreadState(tid)->toString().c_str());
    }
}
