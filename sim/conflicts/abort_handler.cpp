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

#include "sim/conflicts/abort_handler.h"
#include <algorithm>
#include <bitset>
#include <utility>
#include "pin/pin.H"
#include "sim/conflicts/llc_cd.h"
#include "sim/conflicts/stallers.h"
#include "sim/conflicts/terminal_cd.h"
#include "sim/conflicts/tile_cd.h"
#include "sim/core/core.h"
#include "sim/memory/constants.h"
#include "sim/rob.h"
#include "sim/sim.h"
#include "sim/trace.h"
#include "sim/tsarray.h"
#include "sim/memory/cache.h"

// Transitional interface (REMOVE ASAP), see init.cpp
std::bitset<MAX_CACHE_CHILDREN> HackGetLLCSharers(Address lineAddr, bool checkReaders);

#undef DEBUG
#define DEBUG(args...) //info(args)
#undef TIMING_DEBUG
#define TIMING_DEBUG(args...) //info(args)

void AbortHandler::initStats(AggregateStat* parentStat) {
    AggregateStat* abortHandlerStats = new AggregateStat();
    abortHandlerStats->init(name.c_str(), "Abort handler stats");
    localAborts.init("localAbrt", "aborts to tasks in the same RoB");
    abortHandlerStats->append(&localAborts);
    remoteAborts.init("remoteAbrt", "aborts to tasks in a different RoB");
    abortHandlerStats->append(&remoteAborts);

    localDependenceAborts.init(
        "localDependenceAborts",
        "local tasks aborted because of a r/w dependence", 2);
    abortHandlerStats->append(&localDependenceAborts);
    remoteDependenceAborts.init(
        "remoteDependenceAborts",
        "remote tasks aborted because of a r/w dependence", 2);
    abortHandlerStats->append(&remoteDependenceAborts);

    rawAborts.init("rawAborts", "Aborts due to a RAW (earlier W -> later R) dep");
    warAborts.init("warAborts", "Aborts due to a WAR (earlier R -> later W) dep");
    wawAborts.init("wawAborts", "Aborts due to a WAW (earlier W -> later W) dep");
    abortHandlerStats->append(&rawAborts);
    abortHandlerStats->append(&warAborts);
    abortHandlerStats->append(&wawAborts);

    rdFwdAborts.init("rdFwdAborts", "Aborts that would have been prevented by stalling on read dep instead of forwarding to writer");
    wrFwdAborts.init("wrFwdAborts", "Aborts that would have been prevented by stalling on write dep instead of forwarding to reader/writer");
    abortHandlerStats->append(&rdFwdAborts);
    abortHandlerStats->append(&wrFwdAborts);

    depthTaskCount.init("depthTaskCount", "tasks aborted at various depths",
                        depthProfile);
    abortHandlerStats->append(&depthTaskCount);
    depthTaskRunning.init("depthTaskRunning",
                          "running tasks aborted at various depths",
                          depthProfile);
    abortHandlerStats->append(&depthTaskRunning);
    depthTaskIdle.init("depthTaskIdle", "idle tasks aborted at various depths",
                       depthProfile);
    abortHandlerStats->append(&depthTaskIdle);
    depthTaskCompleted.init("depthTaskCompleted",
                            "completed tasks aborted at various depths",
                            depthProfile);
    abortHandlerStats->append(&depthTaskCompleted);

    globalMemoryMsg.init("globalMemoryMsg",
                         "number of memory messages on the global network", 2);
    abortHandlerStats->append(&globalMemoryMsg);
    localMemoryMsg.init("localMemoryMsg",
                        "number of memory messages to the local tile RoB", 2);
    abortHandlerStats->append(&localMemoryMsg);
    correctiveWrites.init("correctiveWrites",
                          "number of undo-log-walk corrective writes", 2);
    abortHandlerStats->append(&correctiveWrites);
    undoLogAccesses.init("undoLogAccesses",
                          "number of undo-log-walk accesses", 2);
    abortHandlerStats->append(&undoLogAccesses);

    stickyUsefulMemoryMsg.init("stickyUsefulMemoryMsg",
                               "sticky directed message that lead to abort", 2);
    abortHandlerStats->append(&stickyUsefulMemoryMsg);
    stickyNonUsefulMemoryMsg.init("stickyNonUsefulMemoryMsg",
                                  "sticky directed message that didn't lead to "
                                  "abort; but tile shares that address",
                                  2);
    abortHandlerStats->append(&stickyNonUsefulMemoryMsg);
    stickyWasteMemoryMsg.init(
        "stickyWasteMemoryMsg",
        "sticky directed message that was wasted. No abort, no sharer", 2);
    abortHandlerStats->append(&stickyWasteMemoryMsg);
    stickyTilesQueried.init("stickyTilesQueried",
                            "sticky tiles queried on global accesses");
    abortHandlerStats->append(&stickyTilesQueried);

    totalReads.init("totalReads", "total reads conflict checked");
    abortHandlerStats->append(&totalReads);
    totalWrites.init("totalWrites", "total writes conflict checked");
    abortHandlerStats->append(&totalWrites);
    tileReads.init("tileReads", "tile reads conflict checked");
    abortHandlerStats->append(&tileReads);
    tileWrites.init("tileWrites", "tile writes conflict checked");
    abortHandlerStats->append(&tileWrites);
    globalReads.init("globalReads", "global reads conflict checked");
    abortHandlerStats->append(&globalReads);
    globalWrites.init("globalWrites", "global writes conflict checked");
    abortHandlerStats->append(&globalWrites);
    stalledReads.init("stalledReads", "read requests stalled");
    abortHandlerStats->append(&stalledReads);
    stalledWrites.init("stalledWrites", "write requests stalled");
    abortHandlerStats->append(&stalledWrites);

    timestampChecks.init("timestampChecks", "number of timestamp checks");
    abortHandlerStats->append(&timestampChecks);

    parentStat->append(abortHandlerStats);
}

AbortHandler::AbortHandler(uint32_t numROBs,
                           uint32_t bloomQueryLatency,
                           uint32_t robTiledAvgLatency,
                           uint32_t localRobAccessDelay,
                           const std::vector<Staller*>& stallers)
    : name("abort-handler"),
      depthProfile(4),
      stallers(stallers),
      llcCD_(nullptr),
      bloomQueryLatency_(bloomQueryLatency),
      robTiledAvgLatency_(robTiledAvgLatency),
      localRobAccessDelay_(localRobAccessDelay) {}

uint64_t AbortHandler::handleConflicts(const Task& requester, Address lineAddr,
                                       ConflictLevel conflictLevel,
                                       uint64_t cycle, bool isLoad,
                                       bool& stall) {
    assert(!stall);
    bool checkReaders = !isLoad;
    uint64_t respCycle = abortFutureTasks(checkReaders, requester, lineAddr, 0,
                                          conflictLevel, cycle, &stall);
    assert(respCycle > cycle);
    TIMING_DEBUG("AbortHandler: Returning cycle %u", respCycle);

    if (stall) {
        if (isLoad) stalledReads.inc();
        else stalledWrites.inc();
    }

    // NOTE(dsm): Triggered even if stall was set to true
    if (conflictLevel == TILE) {
        tileReads.inc();
    } else {
        assert(conflictLevel == GLOBAL);
        globalReads.inc();
    }
    return respCycle;
}


uint64_t AbortHandler::checkL1(const Task& task, Address addr, uint64_t cycle,
                               bool isLoad, bool& stall) {
    assert(!stall);
    uint32_t coreIdx = GetCoreIdx(task.runningTid);
    Address lineAddr = addr >> ossinfo->lineBits;

    const Cache* l1d = ossinfo->cores[coreIdx]->getDataCache();
    int32_t l1dLineId = l1d->probe(lineAddr);
    MESIState l1dState = (l1dLineId >= 0)? l1d->getState(l1dLineId) : I;
    AccessType type = isLoad? GETS : GETX;
    // FIXME(mcj) why return false if the line ID isn't valid??
    bool shouldSendUp = (l1dLineId >= 0)
            ? ossinfo->cores[coreIdx]->getTerminalCD()->shouldSendUp(
                  type, &task.cts(), l1dLineId, l1dState)
            : false;

    uint64_t respCycle = cycle;

    DEBUG("[%ld|%d] CC %lx %s up=%d | %s", cycle, coreIdx, lineAddr,
          MESIStateName(l1dState), shouldSendUp, task.toString().c_str());

    bool tileCheck = shouldSendUp ||
            (isLoad? !IsValid(l1dState) : !IsExclusive(l1dState));
    if (tileCheck) {
        respCycle = checkL2(task, lineAddr, cycle, isLoad, stall);
        if (stall) return respCycle;
        uint32_t tileIdx = getROBIdx(coreIdx);
        stallers[tileIdx]->recordAccess(task, lineAddr);
    }

    // FIXME(mcj) restore recording the maximum depth of a cascade, but not
    // through deep recursion spanning several methods
    //abortDepth.push(depth);
    //if (depth > 0) conflictAbortDepth.push(depth);

    assert(respCycle >= cycle);
    return respCycle;
}

uint64_t AbortHandler::checkL2(const Task& task, Address lineAddr, uint64_t cycle,
                               bool isLoad, bool& stall) {
    // FIXME(mcj) this shouldn't duplicate TileCD::shouldSendUp. It's
    // already become stale once; duplicate code is bound to wind up stale.
    // Unfortunately TileCD::shouldSendUp increments global stats counters,
    // so it's disingenuous to invoke it here.
    uint32_t coreIdx = GetCoreIdx(task.runningTid);
    uint32_t tileIdx = getROBIdx(coreIdx);
    const Cache* l2 = tileCDs_[tileIdx]->getCache();
    int32_t l2LineId = l2->probe(lineAddr);
    MESIState l2State = l2->getState(l2LineId);
    const TSArray* tsa = ossinfo->tsarray[tileIdx];
    bool globalCheck = isLoad? !IsValid(l2State) : !IsExclusive(l2State);
    globalCheck = globalCheck ||
            (tsa->getTS(l2LineId) >= task.cts() && !tsa->isZeroTS(l2LineId));
    uint64_t respCycle;
    if (globalCheck) {
        respCycle = checkGlobal(task, lineAddr, cycle, isLoad, stall);
    } else {
        respCycle = handleConflicts(task, lineAddr, TILE, cycle, 
                                    isLoad, stall);
    }
    assert(respCycle >= cycle);
    return respCycle;
}

uint64_t AbortHandler::checkGlobal(const Task& task, Address lineAddr, uint64_t cycle,
                                   bool isLoad, bool& stall) {
    uint64_t respCycle = handleConflicts(task, lineAddr, GLOBAL, cycle, isLoad, stall);
    assert(respCycle >= cycle);
    return respCycle;
}

void AbortHandler::setTileCDArray(const std::vector<TileCD*>& tileCDs) {
    assert(tileCDs_.empty());
    tileCDs_ = tileCDs;
    assert(!tileCDs_.empty());
}

void AbortHandler::setTerminalCDArray(const std::vector<TerminalCD*>& terminalCDs) {
    assert(terminalCDs_.empty());
    terminalCDs_ = terminalCDs;
    assert(!terminalCDs_.empty());
}

void AbortHandler::setLLCCD(LLCCD* llcCD) {
    assert(llcCD_ == nullptr);
    llcCD_ = llcCD;
    assert(llcCD_);
}

uint64_t AbortHandler::abortIfSuccessor(
    const TaskPtr& task, const Task& requester, Address instigatingAddr,
    bool checkReaders, const uint32_t callerDepth,
    bool* taskAborted, const uint64_t cycle, bool* stallPtr) {

    uint64_t respCycle = cycle;

    if (requester.noTimestamp && !task->cantSpeculate()) {
        // victory: For now, NOTIMESTAMP tasks should not access tracked
        // memory in a way that could conflict with speculative tasks. In fact,
        // they should not write to tracked memory at all. If they do,
        // we need to think more about what restrictions or guarantees
        // we can give to the programmer.
        warn("Ignoring NOTIMESTAMP task %s conflict with %s on addr Ox%lx.",
             requester.toString().c_str(),
             task->toString().c_str(),
             instigatingAddr);

        // Assume the programmer correctly ensured NOTIMESTAMP tasks only
        // access untracked memory, so any dependence with a speculative task
        // must be false.
        return respCycle;
    }

    bool nonConflict = (task.get() == &requester) ||
                       (requester.isIrrevocable() && task->isIrrevocable());

    if (nonConflict) return respCycle;

    // Check timestamp

    timestampChecks.inc();

    // Ideally an irrevocable task could use its conflict timestamp of 0 to win
    // conflicts against any speculative task that previously accessed its
    // state. Unfortunately, a race condition prevents such simplicity. Because
    // irrevocables do not necessarily have a tiebreaker, they only hold back
    // the GVT when a dependence with a speculative task is discovered. It's
    // possible that a victim speculative task that needs to be aborted is about
    // to become the earliest speculative task, or if it finished execution, it
    // could even be passed over the by the GVT. This is problematic, as the
    // victim's tile may have send an LVT update to the arbiter, and the system
    // GVT will advance.
    // Our solution is to stall the irrevocable until the speculative victim
    // either commits or aborts using prior stall logic. This is a potentially
    // large performance penalty for the irrevocable, but is hopefully rare.

    bool irrevocableMustStall = requester.isIrrevocable() &&
            // The victim has already finished, precedes its LVT, the
            // irrevocable tile LVT cannot be used to hold back the GVT,
            // and the victim presently precedes the irrevocable's logical
            // timestamp
            task->cts() < requester.lts() &&
            task->cts() < task->container()->lvt() &&
            task->cts() < requester.container()->lvt();

    bool needsAbort = !irrevocableMustStall && requester.cts() < task->cts();

    const uint32_t tileIdx = task->container()->getIdx();
    const uint32_t requesterTile = requester.container()->getIdx();
    bool isWriter = tileCDs_[tileIdx]->isWriter(task.get(), instigatingAddr);

    if (needsAbort) {
        if (abort(task, tileIdx, Task::AbortType::HAZARD, cycle, callerDepth,
                  &respCycle)) {
            if (tileIdx == requesterTile) {
                localAborts.inc();
                localDependenceAborts.inc(callerDepth > 0);
            } else {
                remoteAborts.inc();
                remoteDependenceAborts.inc(callerDepth > 0);
            }
            assert(taskAborted);
            *taskAborted = true;
            trace::dataAbort(task.get(), &requester,
                             instigatingAddr, checkReaders, isWriter);
        }
    } else {
        if (irrevocableMustStall) {
            assert(requester.isIrrevocable());
            assert(stallPtr);
            // The irrevocable stalls until the speculative task either commits
            // or aborts.
            registerShouldStall(*task);  // sim
            warn("Stalling irrevocable: %s -> %s gvt %s",
                  requester.toString().c_str(),
                  task->toString().c_str(),
                  task->container()->GVT().toString().c_str());
            *stallPtr = true;
        } else if (stallPtr) {
            // Assign tiebreakers lazily if needed
            task->container()->notifyDependence(task->ptr(), requester.cts());
            assert(task->cts() < requester.cts());

            bool stallCond = stallers[tileIdx]->shouldStall(
                instigatingAddr, checkReaders, *task, isWriter);
            if (stallCond) {
                registerShouldStall(*task);  // sim
                DEBUG("Stalling: %s -> %s", task->toString().c_str(),
                      requester.toString().c_str());
                *stallPtr = true;
            }
        }
    }
    // Timestamp check latency is introduced by the caller through 'cycle'
    assert(respCycle >= cycle);
    return respCycle;
}

uint64_t AbortHandler::abortFutureTasks(
    bool checkReaders,
    const Task& requester,
    const Address lineAddr, const uint32_t callerDepth,
    const ConflictLevel conflictLevel,
    const uint64_t cycle, bool* stallPtr) {
    DEBUG("Bloom Abort Controller: abort accesses of 0x%lx beyond ts %s",
          lineAddr, requester.ts.toString().c_str());

    std::bitset<MAX_CACHE_CHILDREN> sharerTiles = llcCD_->sharers(lineAddr, checkReaders);
    std::bitset<MAX_CACHE_CHILDREN> abortedOrPresentTiles;
    std::bitset<MAX_CACHE_CHILDREN> abortedTiles;

    sharerTiles |= HackGetLLCSharers(lineAddr, checkReaders);

    // Timing model information
    uint64_t respCycle = cycle;
    const uint32_t requesterTile = requester.container()->getIdx();

    // Abort future tasks, but randomize access to the tiles to avoid systemic
    // simulator bias in virtual time assignment
    std::vector<uint32_t> tileIdxes(tileCDs_.size());
    std::iota(tileIdxes.begin(), tileIdxes.end(), 0ul);
    std::random_shuffle(tileIdxes.begin(), tileIdxes.end());
    for (uint32_t tileIdx : tileIdxes) {
        if (conflictLevel == TILE && tileIdx != requesterTile) continue;

        if (sharerTiles[tileIdx]) {
            if (tileIdx != requesterTile) {
                assert(conflictLevel != TILE);
                globalMemoryMsg.inc(callerDepth > 0);
            } else localMemoryMsg.inc(callerDepth > 0);

            // Query the Tile conflict detection for the
            // relevant accessors of the line.
            // checkReaders = false ===> check only writers
            // checkReaders = true ===>  check writers & readers
            Accessors accessors =
                tileCDs_[tileIdx]->queryAllAccessors(lineAddr, checkReaders);

            // This tells us if, before aborting any tasks, this tile had any
            // speculative sharers of the lineAddr, reader or writer.
            abortedOrPresentTiles[tileIdx] =
                tileCDs_[tileIdx]->anyWriters(lineAddr) ||
                (checkReaders && tileCDs_[tileIdx]->anyReaders(lineAddr));

            const uint32_t robAccessDelay = (tileIdx == requesterTile)
                                                ? localRobAccessDelay_
                                                : robTiledAvgLatency_;

            // All accessors at a tile are queried at once with a single
            // bloomQueryLatency_ penalty (after the latency of reaching the
            // remote or local tile). However, checking conflicting timestamp
            // violations for each accessor is serialized using the tsCheck
            // counter. FIXME(mcj) NONE of the rollback/ACK messages suffer
            // network contention. This is not good...
            uint32_t tsCheck = 0;
            const uint64_t conflictDiscoveryCycle = cycle + bloomQueryLatency_
                    + robAccessDelay;

            bool anyTaskAborted = false;
            for (const Task* task : accessors) {
                bool thisTaskAborted = false;
                bool wasTaskWaitingForAbort = task->hasPendingAbort();
                TaskPtr omgInterfaceTask = ((Task*)task)->ptr(); /*FIXME(dsm) interface*/
                uint64_t thisTaskAbortCycle = abortIfSuccessor(
                    omgInterfaceTask, requester, lineAddr, checkReaders,
                    callerDepth, &thisTaskAborted,
                    conflictDiscoveryCycle + tsCheck, stallPtr);
                tsCheck++;

                anyTaskAborted |= thisTaskAborted;

                // The requester will get an ACK back from this task
                // immediately after the conflict is discovered, unless
                // 1) the task had to rollback
                // 2) the offending lineAddr was in the task's write set.
                // In that case, an ACK is only sent back to the requester
                // once the rollback is finished.
                // Note that robAccessDelay represents the ACK latency
                // FIXME(mcj) ignoring network contention of course.
                // FIXME(mcj) the timings are way off if wasTaskWaitingForAbort
                // is true.
                // FIXME(mcj) if the conflict was local, then two
                // localRobAccessDelays are probably overkill.
                const uint64_t taskAckCycle = robAccessDelay + (
                        (thisTaskAborted
                         && tileCDs_[tileIdx]->isWriter(task, lineAddr)) ?
                        thisTaskAbortCycle :
                        (conflictDiscoveryCycle + tsCheck));

                respCycle = std::max(respCycle, taskAckCycle);

                if (thisTaskAborted && !wasTaskWaitingForAbort) {
                    bool isTaskWriter = tileCDs_[tileIdx]
                            ->isWriter(task, lineAddr);
                    if (checkReaders) {
                        // We are a writer
                        if (isTaskWriter) wawAborts.inc();
                        else rawAborts.inc();
                    } else {
                        // We are a reader, so later task must be a writer
                        warAborts.inc();
                    }

                    // Forwarding aborts are those that stalling would have prevented
                    bool prevWrite =
                        tileCDs_[requesterTile]->isWriter(&requester, lineAddr);
                    if (prevWrite) {
                        wrFwdAborts.inc();
                        stallers[requesterTile]->recordForwardingAbort(
                            lineAddr, isTaskWriter, requester, prevWrite);
                    } else {
                        bool prevRead = tileCDs_[requesterTile]->isReader(
                            &requester, lineAddr);
                        // NOTE: It's only preventable if the later task
                        // writes... otherwise we'd be stalling reads on reads
                        if (prevRead && isTaskWriter) {
                            rdFwdAborts.inc();
                            stallers[requesterTile]->recordForwardingAbort(
                                lineAddr, isTaskWriter, requester, prevWrite);
                        }
                    }
                }

                if ((conflictLevel == TILE) && !wasTaskWaitingForAbort &&
                    (task->container() != requester.container()) &&
                    thisTaskAborted) {
                    int32_t lineId = tileCDs_[requesterTile]->cache_->probe(lineAddr);
                    MESIState state = tileCDs_[requesterTile]->cache_->getState(lineId);
                    TimeStamp canary = (lineId == -1)?
                            INFINITY_TS :
                            ossinfo->tsarray[requesterTile]->getTS(lineId);

                    bool isWriter = tileCDs_[tileIdx]->isWriter(task, lineAddr);
                    assert(isWriter
                            || tileCDs_[tileIdx]->isReader(task, lineAddr));

                    panic(
                        "GLOBAL abort on TILE level conflict check for addr "
                        "0x%lx checkReaders %d. "
                        "requester's l2state %s canary %s. "
                        "requester[tile %d]: %s, abortedTask[tile %d]: %s writer %d wfa %d",
                        lineAddr, checkReaders,
                        MESIStateName(state),
                        canary.toString().c_str(),
                        requesterTile, requester.toString().c_str(),
                        tileIdx, task->toString().c_str(), isWriter, wasTaskWaitingForAbort);
                }
            }
            if (anyTaskAborted) abortedTiles.set(tileIdx);
        } else if (ossinfo->usePreciseAddressSets) {
            assert(!tileCDs_[tileIdx]->anyWriters(lineAddr));
            assert(!checkReaders || !tileCDs_[tileIdx]->anyReaders(lineAddr));
        }
    }

    if (conflictLevel == GLOBAL) {
        stickyTilesQueried.push(sharerTiles.count());
        uint32_t idx = callerDepth > 0;
        stickyUsefulMemoryMsg.inc(idx, (sharerTiles & abortedTiles).count());
        stickyNonUsefulMemoryMsg.inc(
            idx, (sharerTiles & abortedOrPresentTiles & ~abortedTiles).count());
        stickyWasteMemoryMsg.inc(
            idx,
            (sharerTiles & ~abortedOrPresentTiles & ~abortedTiles).count());
    }

    // Adding bloomQueryLatency_ everywhere might be overkill
    // Why? If you're querying the same Bloom filter at multiple abort depths
    // then why do you have to pay the access latency twice? It's already
    // there...
    // But then we don't model concurrency properly, so let this pass for now.
    respCycle += bloomQueryLatency_;
    TIMING_DEBUG("\tCaller Depth: %u, abortFutureTasks returning cycle %lu",
                 callerDepth, respCycle);

    // Because of the bloomQueryLatency_ increment, we always increase the cycle
    // FIXME(mcj) that shouldn't be necessary if there are no sticky sharers.
    assert(respCycle > cycle);
    return respCycle;
}

uint64_t AbortHandler::abortAllFutureTasks(const Task& firstAbortee,
                                           const uint64_t cycle) {
    DEBUG("Unselective cascading aborts: abort all tasks beyond ts %s",
          firstAbortee.ts.toString().c_str());

    assert(!firstAbortee.isIrrevocable());

    // Timing model information
    uint64_t respCycle = cycle;
    const uint32_t firstAborteeTile = firstAbortee.container()->getIdx();
    std::vector<uint32_t> robAccessDelays;
    std::vector<uint32_t> robTsChecks;

    std::vector<TaskPtr> abortableTasks;
    for (uint32_t tileIdx = 0; tileIdx < ossinfo->numROBs; tileIdx++) {
        auto& dequeuedTasks = ossinfo->robs[tileIdx]->getCommitQueue();
        std::copy(dequeuedTasks.begin(), dequeuedTasks.end(),
                  std::back_inserter(abortableTasks));

        robAccessDelays.push_back((tileIdx == firstAborteeTile)
                                      ? localRobAccessDelay_
                                      : robTiledAvgLatency_);

        // After the latency of reaching the remote or local tile, task
        // timestamps checks within a tile are serialized using a tsCheck
        // counter.  NOTE: We are being generous by not simulating network
        // contention for any of the rollback/ACK messages.
        robTsChecks.push_back(0);
    }

    // To ensure undo-writes are performed in the correct order, we shall abort
    // tasks in reverse timestamp order.  Note that we are being generous here
    // in not adding any timing cost for getting or keeping abortable (commit
    // queue) tasks in sorted order.
    std::sort(
        abortableTasks.begin(), abortableTasks.end(),
        [](const TaskPtr& a, const TaskPtr& b) { return a->cts() > b->cts(); });

    for (TaskPtr task : abortableTasks) {
        uint32_t tileIdx = task->container()->getIdx();

        bool thisTaskAborted = false;
        uint64_t thisTaskAbortCycle = abortIfSuccessor(
            task, firstAbortee, 0x0, true, 1, &thisTaskAborted,
            cycle + robAccessDelays[tileIdx] + robTsChecks[tileIdx], nullptr);
        robTsChecks[tileIdx]++;

        // The requester will get ACKs back after rollback is
        // finished.  Note that robAccessDelays represents the ACK latency.
        // Again, we are being generous by not simulating network contention.
        // FIXME(mcj) if the conflict was local, then two
        // localRobAccessDelays are probably overkill.
        const uint64_t taskAckCycle =
            robAccessDelays[tileIdx] + thisTaskAbortCycle;
        respCycle = std::max(respCycle, taskAckCycle);
    }

    assert(respCycle >= cycle);
    return respCycle;
}

bool AbortHandler::abort(const TaskPtr& task, uint32_t tileIdx,
                         Task::AbortType type,
                         uint64_t cycle, int callerDepth,
                         uint64_t* respCyclePtr) {
    assert(task);
    assert(!task->isIrrevocable())
    const bool firstAbort = !task->hasPendingAbort();

    if (!task->isAborting(cycle)) {
        Event* abortEvent = schedEvent(cycle, [=](uint64_t cycle) {
            // This task may have been resident in a TSB when the abort
            // cascade got it. But since the cascade, it was received by an
            // ROB. Therefore the ROB id overrides the original.
            // TODO(dsm): Proper PARENT abort forwarding will make this
            // obsolete
            // FIXME(dsm): Just call tileCD's finishAbort...
            uint32_t idx =
                task->hasContainer() ? task->container()->getIdx() : tileIdx;
            tileCDs_[idx]->startAbort(task, type, idx, cycle);
        });

        DEBUG("Scheduled abort event %p on task %s, cycle: %lu",
                     abortEvent, task->toString().c_str(), cycle);
        task->recordStartAbortEvent(abortEvent);
    }
    task->recordFinishAbortCycle(type, cycle);

    // 0. Release our read-set
    if (!task->idle() && task->hasContainer()) {
        uint32_t idx = task->container()->getIdx();
        if (type != Task::AbortType::EXCEPTION) {
            tileCDs_[idx]->clearReadSet(*task);
        }
    }

    if (!firstAbort) return false;  // everything below already done
    if (task->idle()) return true;  // nothing to do

    DEBUG("Abort Handler: abort %s", task->toString().c_str());

    // Timing model information
    struct Delay {
        uint64_t writeSetWalk;
        uint64_t dependencies;

        Delay(uint64_t c) : writeSetWalk(c), dependencies(c) {}
    } abortCycle(cycle);

    // 1. Recursively abort dependencies
    // Hardware -- walk the write log [[ affects timing ]]
    // Software (simulator) -- walk the write set
    // FIXME(dsm): With monolithic aborts, undoLog was being walked twice
    // (first to get the write set, then to perform undo writes). This was OK
    // when the undo log was a vector, but with the new layout, writing an
    // iterator just for this would be complex. So, instead, we now use an
    // ephemeral logEntries vector. This is strange but should go away when
    // aborts are done properly (element by element).
    std::vector<UndoLog::Entry> logEntries;
    std::set<Address> writeSet;
    while (!task->undoLog.empty()) {
        UndoLog::Entry e = task->undoLog.pop();
        logEntries.push_back(e);
        writeSet.insert(e.addr >> ossinfo->lineBits);
    }
    task->undoLog.clear();

    if (ossinfo->selectiveAborts) {
        for (Address lineAddr : writeSet) {
            // In hardware, rollback involves performing writes which can cause
            // invalidations and nested conflict checks.  Here, we simulate
            // just the nested conflict checks to abort dependent tasks.
            // FIXME(mcj) Several inaccuracies:
            // * the timestamped GETX implicit to this call isn't causing network
            //   contention
            // * it doesn't necessarily have to be GLOBAL; perhaps the address is
            //   cached and the canary timestamp is friendly
            bool checkReaders = true;
            uint64_t respCycle = abortFutureTasks(checkReaders,
                    *task /* requester */, lineAddr, callerDepth + 1, GLOBAL,
                    cycle, nullptr);
            abortCycle.dependencies = std::max(abortCycle.dependencies, respCycle);
        }
    } else {
        if (callerDepth == 0 || callerDepth == -1 || type == Task::AbortType::PARENT) {
            // This is the first call to abort within this event.
            // Start an abort cascade.
            uint64_t respCycle = abortAllFutureTasks(*task, cycle);
            abortCycle.dependencies =
                std::max(abortCycle.dependencies, respCycle);
        } else {
            // This is a recursive call of abort() within the atomic abort cascade.
            assert(callerDepth == 1);
            assert(type == Task::AbortType::HAZARD);
            // No need to recurse, the first abortee directly aborts all later tasks.
        }
    }

    // [mcj] I think the idea of these 0 vs other depth counters is that 0 depth
    // stats could/should be handled by the memory hierarchy. Depths 1 and above
    // are inside the cascade, so the memory hierarchy can't help.
    // The corrective writes of an undoLog walk would generate timestamped GETX
    // messages, and bring the line into the local tile. This can't happen in
    // the memory hierarchy now, so give it depth + 1.
    // Only count the corrective write once per task; a task can have several
    // abort cascades in this AbortHandler simulation, but I think these
    // messages would only be sent once.
    correctiveWrites.inc((callerDepth + 1) > 0, writeSet.size());
    undoLogAccesses.inc((callerDepth + 1) > 0, logEntries.size());

    // 3. Restore undo log, back to front (idempotent b/c we clear on first walk)
    // Note that this has to be done now, not when the abort event is dequeued
    // (possibly) in the future...
    for (UndoLog::Entry e : logEntries) {
        uint64_t* p = (uint64_t*)e.addr;
        DEBUG("Abort: restore *(%p)=%ld to %ld", p, *p, e.value);
        size_t copied = PIN_SafeCopy(p, &e.value, sizeof(uint64_t));
        (void)copied;
        assert_msg(copied == sizeof(uint64_t),
                "Undo log walk of %s failed to restore %p --- unmapped "
                "mem? (%ld/%ld bytes restored)",
                task->toString().c_str(), p, copied, sizeof(uint64_t));
    }

    abortCycle.writeSetWalk = logEntries.size();

    // 4. Increment depth count profile
    assert((callerDepth + 1) >= 0);
    uint32_t abortDepthIdx =
        std::min(static_cast<uint32_t>(callerDepth + 1), depthProfile - 1);
    depthTaskCount.inc(abortDepthIdx);
    if (task->idle()) {
        depthTaskIdle.inc(abortDepthIdx);
    } else if (task->running()) {
        depthTaskRunning.inc(abortDepthIdx);
    } else if (task->completed() || task->exceptioned()) {
        // FIXME(dsm): Should aborts from EXCEPTIONED have their own counter?
        depthTaskCompleted.inc(abortDepthIdx);
    } else {
        panic("Invalid state for aborted task");
    }

    // 5. Update the timing model latency
    // FIXME(mcj) problems:
    //  * How many GETX's can be outstanding in parallel?
    //    -> I assume that if we somehow permitted concurrent corrective GETX's,
    //       they would issue at most 1 per cycle. That starting delay isn't
    //       accounted for.
    //  * Why add the writeSetWalk to the dependencies? Is this trying to
    //    account for that 1 GETX per cycle?
    uint64_t respCycle = abortCycle.writeSetWalk + abortCycle.dependencies;
    TIMING_DEBUG(
        "\tDepth: %u, abort returning cycle %lu, dependencies:"
        "%lu",
        callerDepth + 1, latency,
        abortCycle.dependencies + abortCycle.writeSetWalk);

    task->container()->notifyAbort(task);

    // [mcj] Ideally we could identify the exact circumstances when
    //      respCycle == cycle
    // is permitted. However even if a task has an undo log to walk, or children
    // to abort, because of abort scheduling, those aborts may not generate
    // events, and therefore do not always increase the cycle.
    assert(respCycle >= cycle);

    if (respCyclePtr) *respCyclePtr = respCycle;
    return true;
}

void AbortHandler::adjustCanariesOnZoomIn(uint32_t robIdx,
                                          const TimeStamp& frameMin,
                                          const TimeStamp& frameMax) {
    tileCDs_[robIdx]->adjustCanariesOnZoomIn(frameMin, frameMax);
    uint32_t coreIdx = robIdx * (ossinfo->numCores / ossinfo->numROBs);
    uint32_t endIdx = coreIdx + (ossinfo->numCores / ossinfo->numROBs);
    for (; coreIdx < endIdx; coreIdx++)
        terminalCDs_[coreIdx]->adjustCanaryOnZoomIn(frameMin, frameMax);
}

void AbortHandler::adjustCanariesOnZoomOut(uint32_t robIdx, uint64_t maxDepth) {
    tileCDs_[robIdx]->adjustCanariesOnZoomOut(maxDepth);
    uint32_t coreIdx = robIdx * (ossinfo->numCores / ossinfo->numROBs);
    uint32_t endIdx = coreIdx + (ossinfo->numCores / ossinfo->numROBs);
    for (; coreIdx < endIdx; coreIdx++)
        terminalCDs_[coreIdx]->adjustCanaryOnZoomOut(maxDepth);
}
