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

#include "sim/ff_queue.h"

#include "sim/driver.h"
#include "sim/sim.h"
#include "sim/task_mapper.h"
#include "sim/trace.h"
#include "sim/tsb.h"

#include <stack>

#undef DEBUG_FF
#define DEBUG_FF(args...)  //info(args)
#undef DEBUG_DRAIN
#define DEBUG_DRAIN(args...)  //info(args)

typedef std::priority_queue<TaskPtr, std::vector<TaskPtr>, TaskTimeGTComparator>
    FfPrioQueue;

// We maintain a LIFO stack of FfPrioQueues, one FfPrioQueue per domain,
// so comparisons performed inside a binary heap are within the same domain,
// which makes the comparison operation constant time.  We could instead put
// all FfTasks into a single FfPrioQueue, but that might be more expensive
// because it would rely on comparisons of full FVTs which may require
// a lot of domain pointer indirection.
// Initially, there is a single FfPrioQueue for the root domain.
static std::vector<FfPrioQueue> ffTaskQueue = {FfPrioQueue()};

static uint32_t CurDomainDepth() {
    // Root domain is depth 0.
    assert(ffTaskQueue.size() >= 1U);
    return ffTaskQueue.size() - 1;
}

// Operations for steady-state fast-forwarding

bool IsFfQueueEmpty() {
    assert(!ffTaskQueue.back().empty() || CurDomainDepth() == 0);
    return CurDomainDepth() == 0 && ffTaskQueue.back().empty();
}

void EnqueueFfTask(TaskPtr&& t) {
    DEBUG_FF("[ffQueue] queuing task: %s", t->toString().c_str());
    while (CurDomainDepth() < t->ts.domainDepth())
        ffTaskQueue.emplace_back();

    auto targetPrioQueue = ffTaskQueue.begin() + t->ts.domainDepth();
    assert(targetPrioQueue->empty() ||
           (targetPrioQueue->top()->ts.sameDomain(t->ts)));
    targetPrioQueue->emplace(std::move(t));
}

TaskPtr DequeueFfTask() {
    // The lowest-timestamp task will be the one in the deepest domain
    assert(!ffTaskQueue.back().empty());

    // Ugh, the lack of a version of pop() that returns a value makes it
    // annoying to move values out of priority queues while avoiding copies.
    // See: https://stackoverflow.com/a/20149745/12178985
    TaskPtr t = std::move(const_cast<TaskPtr&>(ffTaskQueue.back().top()));
    ffTaskQueue.back().pop();

    assert(t->ts.domainDepth() == CurDomainDepth());

    while (ffTaskQueue.back().empty() && CurDomainDepth())
        ffTaskQueue.pop_back();

    DEBUG_FF("[ffQueue] dequeuing task: %s", t->toString().c_str());
    return t;
}

// Operations for transitioning out of fast-forwarding into full simulation.

static uint64_t drainStartCycle = 0;
static uint64_t drainTasks = 0;
static uint64_t drainedTasks = 0;

static uint32_t domainDepth = 0;
static uint32_t frameBaseDepth = 0;
static TimeStamp zoomInRequesterTs = INFINITY_TS;

static bool DrainFfTasks(TSB* tsb) {
    // Enqueue tasks from shallow domains first, allowing them to be
    // frame-spilled if we need to zoom in for deep tasks in ffTaskQueue.
    while (domainDepth < ffTaskQueue.size()) {
        if (ffTaskQueue[domainDepth].empty()) {
            domainDepth++;
            continue;
        }

        TaskPtr task = ffTaskQueue[domainDepth].top();
        assert(task->ts.domainDepth() == domainDepth);

        // If needed, request a zoom-in (which will happen on the next GVT
        // update), and tell TSB we're done fone for now.
        if (domainDepth >= frameBaseDepth + ossinfo->maxFrameDepth) {
            DEBUG_DRAIN("[ffQueue] Requesting zoom to enqueue: %s",
                        task->toString().c_str());
            zoomInRequesterTs = task->ts.getTimeStampAtLevel(
                frameBaseDepth + ossinfo->maxFrameDepth - 1);
            DEBUG_DRAIN("[ffQueue] Set zoom requester ts to: %s",
                        zoomInRequesterTs.toString().c_str());
            return true;
        }

        bool reserved = tsb->reserve(0, 0, false, false);
        if (!reserved) {
            DEBUG_DRAIN("[ffQueue] Waiting for more TSB space.");
            zoomInRequesterTs = INFINITY_TS;
            return false;  // tell TSB we're not done
        }

        DEBUG_DRAIN("[ffQueue] draining task: %s", task->toString().c_str());
        trace::taskCreated(task.get(), nullptr);
        uint32_t robIdx = ossinfo->taskMapper->getDstROBIdx(
            0, nullptr, task->hint, task->noHashHint);
        tsb->enqueue(task, robIdx, 0, nullptr, false,
                     getCurCycle());
        ffTaskQueue[domainDepth].pop();
        drainedTasks++;
    }

    DEBUG_DRAIN("[ffQueue] Done draining tasks.");
    uint64_t drainCycles = getCurCycle() - drainStartCycle;
    if (drainTasks) {
        info("When exiting FF, drained %ld initial tasks to TSB in %ld cycles",
             drainTasks, drainCycles);
    }
    assert(drainTasks == drainedTasks);

    assert(std::all_of(ffTaskQueue.begin(), ffTaskQueue.end(),
                       [](const FfPrioQueue& q) { return q.empty(); }));
    ffTaskQueue.clear();
    ffTaskQueue.emplace_back();
    assert(IsFfQueueEmpty());
    zoomInRequesterTs = INFINITY_TS;
    return true;  // done
}

void DrainFfTaskQueue() {
    assert(!drainStartCycle);
    drainStartCycle = getCurCycle();
    assert(!drainTasks);
    drainTasks = 0;
    for (const FfPrioQueue& q : ffTaskQueue) drainTasks += q.size();

    // When we exit fast-forward, these tasks are drained to thread 0's TSB.
    auto curThread = GetCurThread();
    assert(curThread);
    assert(curThread->tid == 0);
    TSB* tsb = ossinfo->tsbs[0];
    if (!DrainFfTasks(tsb)) {
        // Called any time tsb has a free slot, until the callback returns true
        tsb->registerFillCallback(DrainFfTasks);
    }
}

void FfTaskQueueNotifyZoomIn(uint32_t newFrameBaseDepth) {
    if (IsFfQueueEmpty()) return;

    frameBaseDepth++;
    assert(frameBaseDepth == newFrameBaseDepth);

    TSB* tsb = ossinfo->tsbs[0];
    if (!DrainFfTasks(tsb)) {
        // Called any time tsb has a free slot, until the callback returns true
        tsb->registerFillCallback(DrainFfTasks);
    }
}

static LVTUpdatePort* lvtUpdatePort = nullptr;
void FfTaskQueueSetPort(LVTUpdatePort* port) { lvtUpdatePort = port; }

void FfTaskQueueSendLvt(const uint64_t epoch) {  // analogous to ROB::sendLvt
    // We must hold the GVT from passing over any FfTasks.
    TimeStamp ffLvt =
        IsFfQueueEmpty() ? INFINITY_TS : ffTaskQueue.back().top()->ts;
    ROB::assignTieBreaker(ffLvt);
    LVTUpdateMsg msg = {ffLvt, zoomInRequesterTs, epoch, false, -1U,
                        zoomInRequesterTs == INFINITY_TS ? ZoomType::NONE
                                                         : ZoomType::IN,
                        AckType::NONE};
    lvtUpdatePort->send(msg, getCurCycle());
}
