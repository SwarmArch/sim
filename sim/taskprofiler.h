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

struct TaskCyclesSummary {
    struct TaskCycles {
        uint64_t idle;  // from creation->start
        uint64_t exec;  // from start->finish
        uint64_t wait;  // from finish until commit or abort
        uint64_t tasks;
        uint64_t maxExec;

        TaskCycles() : idle(0), exec(0), wait(0), tasks(0), maxExec(0) {}

        void add(uint64_t i, uint64_t e, uint64_t w) {
            idle += i;
            exec += e;
            wait += w;
            tasks++;
            if (maxExec < e) maxExec = e;
        }
    };

    TaskCycles commit;
    TaskCycles abort;

    TaskCyclesSummary* parentSummary;

    TaskCyclesSummary(TaskCyclesSummary* parent = nullptr)
        : parentSummary(parent) {}

    void addCommit(uint64_t idle, uint64_t exec, uint64_t wait) {
        commit.add(idle, exec, wait);
        if (parentSummary) parentSummary->addCommit(idle, exec, wait);
    }

    void addAbort(uint64_t idle, uint64_t exec, uint64_t wait) {
        abort.add(idle, exec, wait);
        if (parentSummary) parentSummary->addAbort(idle, exec, wait);
    }
};

class TaskProfiler : public TaskObserver {
  private:
    TaskCyclesSummary* parentSummary;
    uint64_t enqueueCycle;
    uint64_t startCycle;
    uint64_t finishCycle;
    const uint64_t& curCycle;

    static const uint64_t INVALID_CYCLE = ~((uint64_t)0UL);

  public:
    TaskProfiler(TaskCyclesSummary* parentSummary, const uint64_t& curCycle)
        : parentSummary(parentSummary),
          startCycle(INVALID_CYCLE),
          finishCycle(INVALID_CYCLE),
          curCycle(curCycle) {
        // N.B. This assumes that a task is enqueued immediately upon
        // construction. Alternatively we could create an enqueue() signal
        enqueueCycle = curCycle;
    }

    void start(ThreadID) override {
        startCycle = curCycle;
        assert(startCycle >= enqueueCycle);
    }

    void finish() override {
        finishCycle = curCycle;
        assert(finishCycle >= startCycle);
    }

    void commit() override {
        assert(startCycle != INVALID_CYCLE);
        assert(finishCycle != INVALID_CYCLE);
        uint64_t commitCycle = curCycle;
        assert(commitCycle >= finishCycle);
        uint64_t idle = startCycle - enqueueCycle;
        uint64_t exec = finishCycle - startCycle;
        uint64_t wait = commitCycle - finishCycle;

        assert(parentSummary);
        parentSummary->addCommit(idle, exec, wait);
        parentSummary = nullptr; // ensure we don't invoke it further
    }

    void abort(const bool requeue) override {
        if (startCycle == INVALID_CYCLE) startCycle = curCycle;
        if (finishCycle == INVALID_CYCLE) finishCycle = curCycle;
        uint64_t abortCycle = curCycle;
        assert(abortCycle >= finishCycle && finishCycle >= startCycle);
        uint64_t idle = startCycle - enqueueCycle;
        uint64_t exec = finishCycle - startCycle;
        uint64_t wait = abortCycle - finishCycle;

        assert(parentSummary);
        parentSummary->addAbort(idle, exec, wait);

        if (requeue) {
            enqueueCycle = curCycle;
            startCycle = INVALID_CYCLE;
            finishCycle = INVALID_CYCLE;
        } else {
            parentSummary = nullptr; // ensure we don't invoke it further
        }
    }
};
