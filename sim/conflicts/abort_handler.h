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

#include <vector>
#include "sim/assert.h"
#include "sim/bithacks.h"
#include "sim/log.h"
#include "sim/stats/stats.h"
#include "sim/task.h"
#include "sim/timestamp.h"
#include "sim/types.h"

class LLCCD;
class TerminalCD;
class TileCD;
class Staller;

class AbortHandler {
  protected:
    // Stats
    Counter localAborts;
    Counter remoteAborts;
    VectorCounter depthTaskCount;
    VectorCounter depthTaskRunning;
    VectorCounter depthTaskCompleted;
    VectorCounter depthTaskIdle;
    Counter rawAborts, warAborts, wawAborts;
    Counter rdFwdAborts, wrFwdAborts;

    VectorCounter remoteDependenceAborts;
    VectorCounter localDependenceAborts;

    VectorCounter globalMemoryMsg;
    VectorCounter localMemoryMsg;
    // [mcj] The following are assumed to be global, since we have no way to
    // tell otherwise.
    // The undo log may contain multiple instances of the same address, say A.
    // It's likely we issue a single INV for A for the first instance of A,
    // and future writes during the undo log walk to A are likely to hit in
    // the cache. undoLogAccesses captures the total accesses to the undo log.
    // correctiveWrites captures only the first write to A.
    VectorCounter correctiveWrites;
    VectorCounter undoLogAccesses;

    Counter timestampChecks;

    VectorCounter stickyUsefulMemoryMsg;
    VectorCounter stickyNonUsefulMemoryMsg;
    VectorCounter stickyWasteMemoryMsg;
    RunningStat<size_t> stickyTilesQueried;

    Counter totalReads, totalWrites;
    Counter tileReads, tileWrites;
    Counter globalReads, globalWrites;
    Counter stalledReads, stalledWrites;

    const std::string name;
    const uint32_t depthProfile;

    std::vector<Staller*> stallers;

  public:
    AbortHandler(uint32_t numROBs,
                 uint32_t bloomQueryLatency, uint32_t robTiledAvgLatency,
                 uint32_t localRobAccessDelay,
                 const std::vector<Staller*>& stallers);
    ~AbortHandler() = default;
    void initStats(AggregateStat* parentStat);

    /*
    These 3 methods detect and resolve conflicts at increasing scope L1 -> L2 -> L3
    stall is set to true if task must stall
    Core intiates conflict checks at the lowest level with checkL1()
    Conflict checks can be initiated at higher levels: StreamPrefetcher
    initiates checks via checkL2() or checkGlobal()
    */
    uint64_t checkL1(const Task& task, Address addr, uint64_t cycle, bool isLoad,
                     bool& stall);
    uint64_t checkL2(const Task& task, Address lineAddr, uint64_t cycle,
                     bool isLoad, bool& stall);
    uint64_t checkGlobal(const Task& task, Address lineAddr, uint64_t cycle,
                         bool isLoad, bool& stall);

    // FIXME(mcj) once aborts, memory accesses, and invalidations are fully
    // event-driven, the latencies and depths shouldn't be necessary, or will be
    // encapsulated by other means.
    // Presently, this method aborts the given task *now*, causing a cascade of
    // aborts, but only if an AbortEvent can be created and scheduled for
    // later.
    bool abort(const TaskPtr& task, uint32_t tileIdx,
               Task::AbortType type, uint64_t cycle, int callerDepth,
               uint64_t* respCyclePtr);

    void setTileCDArray(const std::vector<TileCD*>& tiles);
    void setTerminalCDArray(const std::vector<TerminalCD*>& tiles);
    void setLLCCD(LLCCD* llcCD);
    const TileCD* getTileCD(uint32_t idx) const { return tileCDs_[idx]; }
    void adjustCanariesOnZoomIn(uint32_t robIdx,
                                const TimeStamp& frameMin,
                                const TimeStamp& frameMax);
    void adjustCanariesOnZoomOut(uint32_t robIdx, uint64_t maxDepth);

  private:

    enum ConflictLevel { TILE, GLOBAL };

    uint64_t handleConflicts(const Task& requester, Address lineAddr, ConflictLevel,
                             uint64_t cycle, bool isLoad, bool& stall);

    // Methods and data that were in BloomAbortHandler

    // [mcj] As a compromise to get the new conflict detection code integrated
    // into master, AbortHandler iterates over each TileCD and queries about
    // which tasks accessed a given address.
    std::vector<TileCD*> tileCDs_;
    // [victory] Adding this here for now just for canary adjustment.
    std::vector<TerminalCD*> terminalCDs_;

    // The AbortHandler queries the llcCD_ for sharers
    LLCCD* llcCD_;

    // Timing information
    const uint32_t bloomQueryLatency_;  // Time to query a transpose-ported SRAM
    const uint32_t robTiledAvgLatency_;
    const uint32_t localRobAccessDelay_;

    // For unselective aborts (modeling prior TLS systems)
    uint64_t abortAllFutureTasks(const Task& firstAbortee, uint64_t cycle);

    uint64_t abortFutureTasks(bool checkReaders, const Task& requester,
                              const Address lineAddr, uint32_t callerDepth,
                              ConflictLevel conflictLevel, uint64_t cycle,
                              bool* stallPtr);

    uint64_t abortIfSuccessor(const TaskPtr& task, const Task& requester,
                              Address instigatingAddress, bool checkReaders,
                              const uint32_t callerDepth,
                              bool* taskAborted, uint64_t cycle, bool* stallPtr);
};
