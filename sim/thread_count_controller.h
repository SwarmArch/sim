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

#include <unordered_map>
#include <vector>
#include "sim/sim.h"
#include "sim/types.h"
#include "sim/spin_cv.h"
#include "sim/stats/stats.h"
#include "sim/str.h"

class ThreadCountController {
  public:
    ThreadCountController(uint32_t _idx)
        : name_("tcc-" + Str(_idx)), lastThreadCountUpdateCycle(0) {}

    bool handleDequeue(ThreadID tid);
    void addCore(Core* core);
    void notifyOne();
    void notifyAll();

    inline uint64_t getNumBlockedThreads() const { return numBlockedThreads; }
    inline void setROB(ROB* r) { rob = r; }

    void initStats(AggregateStat* parentStat);

    uint32_t increaseThreads(uint32_t);
    uint32_t decreaseThreads(uint32_t);

  private:
    const std::string name_;

    ROB* rob;
    std::unordered_map<uint32_t, Core*> coreMap; // cid -> Core* map
    std::vector<Core*> cores;
    SPinConditionVariable blockedThreads;
    uint32_t numBlockedThreads;

    uint64_t lastThreadCountUpdateCycle;

    inline const char* name() const { return name_.c_str(); }

    // Stats
    Counter numAvailableThreads;
    RunningStat<size_t> threadCount;
};
