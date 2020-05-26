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

#include "sim/assert.h"
#include "sim/log.h"
#include "sim/stats/table_stat.h"
#include "sim/taskobserver.h"
#include "sim/types.h"

#undef DEBUG
#define DEBUG(args...) //info(args)

/**
 * A TaskToCoreStatsProfiler is registered to observe the events of some Task.
 * When constructed, this class records the stats breakdown of the Task's type
 * (its row in a table stat) at the core at which the task is running. When the
 * task finishes, this profiler records the diff of the stats breakdown of before
 * and after the task executed. When the task surely commits, that diff is
 * merged (vector addition) with the committed table.
 *
 * On the observable Task's abort, this class unregisters itself from the Task.
 * The stats recorded will not be committed, so no action needs to be taken.
 */
class TaskToCoreStatsProfiler : public TaskObserver {
    Task& task;
    const TableStat& input;
    TableStat& committed;
    TableStat& abortedDirect;
    TableStat& abortedIndirect;
    const uint32_t taskRowIdx;
    VectorCounter taskBreakdown;
    bool hasFinished;
  public:
    TaskToCoreStatsProfiler(Task& t, const TableStat& input,
                            TableStat& committed, TableStat& abortedDirect,
                            TableStat& abortedIndirect,
                            uint32_t r)
        : task(t),
          input(input), committed(committed),
          abortedDirect(abortedDirect),
          abortedIndirect(abortedIndirect),
          taskRowIdx(r),
          // Take a snapshot before the task runs
          taskBreakdown(input.row(taskRowIdx)),
          hasFinished(false)
          {}

    void start(ThreadID) override { assert(false); }

    void finish() override {
        hasFinished = true;
        taskBreakdown = input.row(taskRowIdx) - taskBreakdown;
        if (taskBreakdown.sum() == 0ul) {
            // Optimization:
            // No need to update either committed or aborted stats, so this
            // profiler is no longer needed. NOTE: checking that the sum is zero
            // works because VectorCounters are unsigned.
            task.unregisterObserver(this);
        }
    }

    void commit() override { committed.row(taskRowIdx) += taskBreakdown; }

    void abort(bool requeue) override {
        DEBUG("TaskToCoreStatsProfiler: abort");
        auto updateTable = requeue ? &abortedDirect : &abortedIndirect;
        assert(hasFinished);
        updateTable->row(taskRowIdx) += taskBreakdown;
        // The stats recorded will not be committed, so this profiler is no
        // longer needed.
        task.unregisterObserver(this);
    }
};
