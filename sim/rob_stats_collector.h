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

#include "sim/rob.h"
#include "sim/core/core.h"
#include "sim/sim.h"

class RobStatsCollector {
  public:
    enum class RobEventType { CYCLE, COMMIT, DEQUEUE };
    enum class CoreEventType { COMMIT_INSTRS, ABORT_INSTRS, STALL_SB_CYCLES };

    RobStatsCollector(ROB* _rob) : rob(_rob) {}
    void addCore(Core* c) { cores.push_back(c); }

    uint64_t getRobEventCount(RobEventType type) {
        switch (type) {
            case RobEventType::CYCLE:
                return getCurCycle();
                break;
            case RobEventType::COMMIT:
                return rob->getCommits().first;
                break;
            case RobEventType::DEQUEUE:
                return rob->getDequeues().first;
                break;
            default:
                panic("Invalid ROB event type");
                break;
        };
    }

    std::vector<uint64_t> getCoresEventCount(CoreEventType type) {
        std::vector<uint64_t> coresEventCount;
        for (auto core : cores) {
            uint64_t count = 0;
            switch(type) {
                case CoreEventType::COMMIT_INSTRS:
                    count = core->getCommittedInstrs();
                    break;
                case CoreEventType::ABORT_INSTRS:
                    count = core->getAbortedInstrs();
                    break;
                case CoreEventType::STALL_SB_CYCLES:
                    count = core->getStallSbCycles();
                    break;
                default:
                    panic("Invalid core event type");
                    break;
            };
            coresEventCount.push_back(count);
        }
        return coresEventCount;
    }

  private:
    ROB* rob;
    std::vector<Core*> cores;
};
