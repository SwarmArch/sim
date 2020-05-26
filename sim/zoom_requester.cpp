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

#include "sim/zoom_requester.h"

#include "sim/rob.h"
#include "sim/timestamp.h"

#undef DEBUG
#define DEBUG(args...)  //info(args)

bool ZoomRequester::requestZoomInIfNeeded(const ConstTaskPtr& t, ThreadID tid) {
    DEBUG("[%s] Task %s (depth: %lu) wants a subdomain. Current max depth: %u",
          name(), t->toString().c_str(), t->ts.domainDepth(),
          rob.getFrameMaxDepth());

    uint64_t desiredDepth = t->ts.domainDepth() + 1;
    if (desiredDepth <= rob.getFrameMaxDepth()) {
        return false;
    }
    assert(desiredDepth == rob.getFrameMaxDepth() + 1);

    DEBUG(
        "[%s] Will wait to zoom in. Blocking thread %u for now.",
        name(), tid);
    zoomInBlockedThreads.addWaiter(tid);
    if (requestingZoomIn()) {
        assert(zoomInRequesterTs.domainDepth() == t->ts.domainDepth());
        if (zoomInRequesterTs >= t->ts) {
            zoomInRequesterTs = t->ts;
        }
    } else {
        zoomInRequesterTs = t->ts;
    }
    tasksWaitingForZoomIn.insert(std::move(t));
    return true;
}

TimeStamp ZoomRequester::getZoomInRequesterTs() const {
    if (requestingZoomIn()) {
        return zoomInRequesterTs;
    } else
        return INFINITY_TS;
}

void ZoomRequester::notifyMaxDepthIncreased() {
    assert(zoomInBlockedThreads.getNumWaiters() ==
           tasksWaitingForZoomIn.size());
    zoomInRequesterTs = INFINITY_TS;
    wakeupAllPossibleThreads(ZoomCompleted::IN);
}

bool ZoomRequester::requestZoomOutIfNeeded(const ConstTaskPtr& t, ThreadID tid) {
    uint64_t desiredDepth = t->ts.domainDepth() - 1;
    DEBUG(
        "[%s] Task %s wants to enqueue to superdomain (depth %lu). "
        "Current min depth: %u",
        name(), t->toString().c_str(), desiredDepth, rob.getFrameMinDepth());

    if (desiredDepth >= rob.getFrameMinDepth()) return false;

    DEBUG(
        "[%s] Will wait to zoom out. Blocking thread %u for now.",
        name(), tid);
    zoomOutBlockedThreads.addWaiter(tid);
    if (requestingZoomOut()) {
        assert(zoomOutRequesterTs.domainDepth() == t->ts.domainDepth());
        if (zoomOutRequesterTs > t->ts) zoomOutRequesterTs = t->ts;
    } else {
        zoomOutRequesterTs = t->ts;
    }
    tasksWaitingForZoomOut.insert(std::move(t));
    return true;
}

TimeStamp ZoomRequester::getZoomOutRequesterTs() const {
    if (requestingZoomOut()) {
        return zoomOutRequesterTs;
    } else
        return INFINITY_TS;
}

void ZoomRequester::notifyMinDepthDecreased() {
    assert(zoomOutBlockedThreads.getNumWaiters() ==
           tasksWaitingForZoomOut.size());
    zoomOutRequesterTs = INFINITY_TS;
    wakeupAllPossibleThreads(ZoomCompleted::OUT);
}

void ZoomRequester::notifyAbort(const ConstTaskPtr& task, ThreadID tid) {
    DEBUG("[%s] Received abort signal for task %s (tid: %u)", name(),
          task->toString().c_str(), tid);
    /*
    [ssub] It's not sufficient to only remove task from list of waiters.
    We have to wake up all threads to be safe. (optimizations possible TODO)

    Aborted task requested a ZOOM IN, say. We clear it from the set.
    If the new tasksWaitingForZoomIn set is empty, it's safe to move
    to STEADY_STATE. Even if this set was non-empty, the zoomInRequesterTs
    may have to be updated -- and we _must_ update it.

           D        + ------ T0 -- T1 -- T2 --------- T4 ------- + <--- current top frame depth (root domain)
                            /  \        /  \         /  \
                           /    \      /    \       /    \
                          .. .. ..    +- T3 -+     .. .. ..
                          .. .. ..                 .. .. ..
            (1) T0's tree wants to zoom in.
                T1's tree wants to zoom out.
                T3 wants to enqueue to superdomain (i.e. current root domain).
                T4's tree wants to zoom in.
            (2) Suppose T0 sets the zoomInRequesterTs. Now T0 is aborted (along
                with its entire tree). If we retain zoomInRequesterTs, then
                we could incorrectly trigger a ZOOM IN, and then set the
                frame spiller's timestamp in that (now non-existent) tree.
                This can lead to violations of the GVT protocol -- GVT may
                pass to T1, but we set the frame spiller's timestamp based
                on zoomInRequesterTs (and also the tile's LVT). Note that we have
                to set the frame spiller's timestamp in that manner to be
                safe. But more importantly, the corresponding frame
                requeuer's timestamp will definitely be in the (now
                non-existent) tree, even as GVT has surpassed that point.
            (3) We could also incorrectly trigger a ZOOM IN, when T1 should
                get priority for ZOOM OUT.
            (4) T3 may have been blocked waiting for a ZOOM OUT (because we
                realized we are awaiting a ZOOM IN from T1's tree). But it
                may no longer need a ZOOM OUT because the current frame is
                valid. We need to wake up and check for valid frame again.

    It is safe to transition to steady state and wakeup threads, even as we
    may have LVT messages in transit that request a ZOOM.
    */
    if (tasksWaitingForZoomIn.count(task) ||
        tasksWaitingForZoomOut.count(task)) {
        zoomOutBlockedThreads.notify(tid);
        zoomInBlockedThreads.notify(tid);
        tasksWaitingForZoomOut.erase(task);
        tasksWaitingForZoomIn.erase(task);
        assert(zoomInBlockedThreads.getNumWaiters() ==
               tasksWaitingForZoomIn.size());
        assert(zoomOutBlockedThreads.getNumWaiters() ==
               tasksWaitingForZoomOut.size());
        DEBUG("[%s] Moving to steady state and waking up all threads.", name());
        // Hacky way to clear ZoomRequester state and wake everything
        notifyMinDepthDecreased();
        notifyMaxDepthIncreased();
    }
}

void ZoomRequester::wakeupAllPossibleThreads(ZoomCompleted type) {
    // Only wake up threads whose tasks are not pending an abort.
    assert(!requestingZoomIn() || !requestingZoomOut());
    auto* taskSet =
        (type == ZoomCompleted::IN) ? &tasksWaitingForZoomIn : &tasksWaitingForZoomOut;
    SPinConditionVariable* sp =
        (type == ZoomCompleted::IN) ? &zoomInBlockedThreads : &zoomOutBlockedThreads;
    for (auto it = taskSet->begin(); it != taskSet->end();) {
        const auto& task = *it;
        if (!task->hasPendingAbort()) {
            sp->notify(task->runningTid);
            DEBUG("[%s] Wake up tid %u running task %s", name(),
                  task->runningTid, task->toString().c_str());
            it = taskSet->erase(it);
        } else
            ++it;
    }
}
