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

#include <algorithm>
#include <list>
#include <stdint.h>
#include "sim/assert.h"
#include "sim/stats/stats.h"
#include "sim/taskobserver.h"
#include "sim/types.h"
#include "sim/loadstatssummary.h"

struct LoadMetricConfig {
    enum class LoadMetric { CYCLES, INSTRS };
    LoadMetric metric;
};

uint64_t getCount(ThreadID tid);

// This is constructued on the same lines as TaskProfiler, but
// to update LoadStatsSummary instead of TaskCycles.
class TaskLoadProfiler : public TaskObserver {
  public:
    TaskLoadProfiler(LoadStatsSummary& parentSummary)
        : parentSummary_(parentSummary),
          start_(INDEFINITE),
          finish_(INDEFINITE),
          runningTid(INVALID_TID) {
        parentSummary_.idleTasks += 1;
    }

    ~TaskLoadProfiler() {}

    void start(ThreadID tid) override {
        start_ = getCount(tid);
        finish_ = INDEFINITE;
        runningTid = tid;
        assert(parentSummary_.idleTasks > 0);
        parentSummary_.idleTasks -= 1;
    }

    void finish() override {
        assert(start_ != INDEFINITE);
        assert(runningTid != INVALID_TID);
        finish_ = getCount(runningTid);
        assert(finish_ >= start_);
        assert(summary_.executed == 0);
        summary_.executed = (finish_ - start_);
        runningTid = INVALID_TID;
    }

    void commit() override {
        assert(runningTid == INVALID_TID);
        assert(finish_ != INDEFINITE);
        summary_.commits += 1;
        // Update parentSummary here, since there can be delay
        // in this profiler getting destructed.
        parentSummary_ += summary_;
        // TODO no further invocations on this class should be made.
        // How to enforce this?
    }

    void abort(const bool requeue) override {
        if (finish_ != INDEFINITE) {
            // The task was aborted during or after completion
            // (TaskObserver::finish() is called when aborted on a core). In that
            // case, we must discount the cycles that were executed between
            // start and finish, and move them to the aborted count.
            summary_.abortexecuted += summary_.executed;
            summary_.executed = 0;
        } else {
            assert(start_ == INDEFINITE);
            // Task has not run
            // If requeue = true, we will increment the count below.
            parentSummary_.idleTasks -= 1;
        }

        assert(summary_.executed == 0);

        // Assume that after abort, the task is immediately re-enqueued
        // or destroyed
        start_ = INDEFINITE;
        finish_ = INDEFINITE;
        summary_.aborts += 1;
        runningTid = INVALID_TID;

        if (requeue) {
            // Add current summary to parent, so we get progress on aborted
            // work.
            parentSummary_ += summary_;
            summary_ = LoadStatsSummary();
            parentSummary_.idleTasks += 1;
        }
    }

  private:
    LoadStatsSummary summary_;
    LoadStatsSummary& parentSummary_;
    uint64_t start_;
    uint64_t finish_;
    ThreadID runningTid;
};
