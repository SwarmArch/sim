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

#include <iostream>
#include <memory>
#include "sim/types.h"

// TODO ssub: Maybe we can move spilling and requeuing into the load stats summary?
//      Complications: Requeuers can also get spilled, which makes it harder to track.
//      For now this will do.
// We maintain one such summary structure for each r, b (r: rob, b: bucket)
// r: rob    --> Corresponds to each rob in the system
// b: bucket --> If we are using any bucket based task mapper, then it corresponds to the specific bucket of the task
//               Else, we just have 1 bucket per ROB
struct LoadStatsSummary {
    // The following stats can be cycles or instructions
    // as initialized in the load metric.
    uint64_t executed;
    uint64_t abortexecuted;

    // These are counts
    uint64_t commits;
    uint64_t aborts;
    uint64_t idleTasks;

    LoadStatsSummary()
        : executed(0),
          abortexecuted(0),
          commits(0),
          aborts(0),
          idleTasks(0) {}

    bool operator==(const LoadStatsSummary& that) const {
        return this->abortexecuted == that.abortexecuted &&
               this->executed == that.executed &&
               this->commits == that.commits &&
               this->aborts == that.aborts;
    }

    // Accumulator function
    LoadStatsSummary& operator+=(const LoadStatsSummary& that) {
        this->executed += that.executed;
        this->abortexecuted += that.abortexecuted;
        this->commits+= that.commits;
        this->aborts+= that.aborts;
        return *this;
    }
};

class TaskLoadProfiler; // Forward declaration
std::unique_ptr<TaskLoadProfiler> createTaskLoadProfiler(LoadStatsSummary*);
