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

#include <stdint.h>
#include <queue>
#include "sim/assert.h"
#include "sim/log.h"
#include "sim/rob.h"

#define DEBUG_STEAL(args...) //info(args)

class StealingTaskBalancer {
  private:
    const std::vector<ROB*> robs;
    const uint32_t victimThreshold;
    const uint32_t stealThreshold;
    const bool lifo;

  public:
    StealingTaskBalancer(const std::vector<ROB*>& _robs,
                         uint32_t _victimThreshold, uint32_t _stealThreshold,
                         bool _lifo)
        : robs(_robs),
          victimThreshold(_victimThreshold),
          stealThreshold(_stealThreshold),
          lifo(_lifo)
    {
        assert(victimThreshold >= stealThreshold);
    }

    // Call this periodically
    void balance(uint64_t cycle) {
        std::priority_queue<std::tuple<uint32_t, ROB*>> victims;
        std::priority_queue<std::tuple<uint32_t, ROB*>> stealers;
        for (ROB* rob : robs) {
            uint32_t tasks = rob->untiedIdleTasks();
            if (tasks < stealThreshold) {
                stealers.push(std::make_tuple(stealThreshold - tasks, rob));
            } else if (tasks > victimThreshold) {
                victims.push(std::make_tuple(tasks - victimThreshold, rob));
            }
        }

        while (!victims.empty() && !stealers.empty()) {
            ROB* victim;
            ROB* stealer;
            uint32_t victimSurplus, stealerDeficit;
            std::tie(victimSurplus, victim) = victims.top();
            std::tie(stealerDeficit, stealer) = stealers.top();
            assert(victimSurplus && stealerDeficit);
            victims.pop();
            stealers.pop();

            DEBUG_STEAL("[%ld] Stealing %d -> %d", cycle, victim->getIdx(),
                        stealer->getIdx());
            stealer->enqueueStolenTask(victim->stealTask(lifo));

            if (victimSurplus > 1)
                victims.push(std::make_tuple(victimSurplus - 1, victim));
            if (stealerDeficit > 1)
                stealers.push(std::make_tuple(stealerDeficit - 1, stealer));
        }
    }
};
