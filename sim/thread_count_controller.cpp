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

#include "thread_count_controller.h"
#include "sim/sim.h"
#include "sim/driver.h"
#include "sim/core/core.h"
#include "sim/log.h"
#include "sim/event.h"
#include "sim/types.h"
#include "sim/rob.h"
#include "sim/match.h"

#undef DEBUG
#define DEBUG(args...) //info(args)

void ThreadCountController::initStats(AggregateStat* parentStat) {
    AggregateStat* ttStats = new AggregateStat();
    ttStats->init(name(), "Thread count controller stats");
    numAvailableThreads.init("availableThreads",
                             "number of available threads (useful for periodic stats)");
    ttStats->append(&numAvailableThreads);
    threadCount.init("threadCount", "number of available threads");
    ttStats->append(&threadCount);
    assert_msg(cores.size() > 0,
               "Initialize cores of ROB (for thread count controller) "
               "before initializing stats. Call rob->addCore().");
    numAvailableThreads.set(cores[0]->getNumAvailableThreads());
    parentStat->append(ttStats);
}

bool ThreadCountController::handleDequeue(ThreadID tid) {
    bool available = coreMap[GetCoreIdx(tid)]->isAvailableThread(tid);
    if (available)
        return true;
    else {
        DEBUG("[%s] Adding thread %u to waiters", name(), tid);
        numBlockedThreads++;
        blockedThreads.addWaiter(tid);
        return false;
    }
}

void ThreadCountController::addCore(Core* core) {
    coreMap[core->getCid()] = core;
    cores.push_back(core);
}

void ThreadCountController::notifyAll() {
    DEBUG("[%s] notifying all waiters", name());
    blockedThreads.notifyAll();
    numBlockedThreads = 0;
}

void ThreadCountController::notifyOne() {
    // FIXME(mcj) numBlockedThreads completely duplicates the functionality of
    // SpinCV::waiters.size(). However, I'm not proposing that numWaiters should
    // be added to SpinCV; that's rarely a reasonable thing to request of a
    // condition variable. After reviewing the API and callers of this class, I
    // think the cleanest solution would have been to make the capacity of the
    // execQueue changeable. But no time for that.
    if (numBlockedThreads) {
        DEBUG("[%s] notifying one waiter to run a spiller", name());
        blockedThreads.notifyOne();
        numBlockedThreads--;
    }
}

uint32_t ThreadCountController::increaseThreads(uint32_t num) {
    uint32_t count = 0;
    bool success = false;
    threadCount.push(cores[0]->getNumAvailableThreads(),
                     getCurCycle() - lastThreadCountUpdateCycle);
    lastThreadCountUpdateCycle = getCurCycle();
    while (num-- > 0) {
        for (auto c : cores) {
            ThreadID tid;
            std::tie(success, tid) = c->increaseAvailableThreads();
            if (success) {
                count++;
                DEBUG("[%s] Waking up thread %u on core %u", name(), tid, c->getCid());
                numBlockedThreads--;
                // This tid may or may not be blocked.
                // If blocked, unblock it.
                blockedThreads.notify(tid);
            }
        }
    }
    assert(count % cores.size() == 0);
    numAvailableThreads.set(cores[0]->getNumAvailableThreads());
    return (count / cores.size());
}

uint32_t ThreadCountController::decreaseThreads(uint32_t num) {
    uint32_t count = 0;
    bool success = false;
    threadCount.push(cores[0]->getNumAvailableThreads(),
                     getCurCycle() - lastThreadCountUpdateCycle);
    lastThreadCountUpdateCycle = getCurCycle();
    while (num-- > 0) {
        for (auto c : cores) {
            ThreadID tid;
            std::tie(success, tid) = c->decreaseAvailableThreads();
            if (success) {
                count++;
                rob->notifyToBeBlockedThread(tid);
                DEBUG("[%s] Removing thread %u on core %u", name(), tid, c->getCid());
            }
            (void)success; (void)tid;  // Don't do anything with these for now
        }
    }
    assert(count % cores.size() == 0);
    numAvailableThreads.set(cores[0]->getNumAvailableThreads());
    return (count / cores.size());
}

