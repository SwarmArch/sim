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

#include <cstdint>
#include <memory>

#include "sim/assert.h"
#include "sim/log.h"
#include "sim/rob.h"
#include "sim/task.h"
#include "sim/taskobserver.h"

#undef DEBUG
#define DEBUG(args...) //info(args)

template <bool UnstallOnFinish>
class Unstaller : public TaskObserver {
  protected:
    Task& t;
    const Task& staller;
    const uint32_t initialAborts;
    std::shared_ptr<uint64_t> stallCount;

    virtual void unstall() {
        (*stallCount)--;
        if (!(*stallCount) && staller.abortCount() == initialAborts &&
            !staller.hasPendingAbort()) {
            stallers.notify(staller.runningTid);
        }
        t.unregisterObserver(this);
    }

  public:
    Unstaller(Task& t, const Task& staller,
              const std::shared_ptr<uint64_t>& stallCount)
        : t(t), staller(staller), initialAborts(staller.abortCount()),
          stallCount(stallCount) {}

    void start(ThreadID) override { assert(false); }
    void finish() override { if (UnstallOnFinish) unstall(); }
    void commit() override { unstall(); }
    void abort(bool requeue) override { unstall(); }
};


class IrrevocableUnstaller : public Unstaller<false> {
    void unstall() override {
        // This Unstaller is serving an irrevocable,
        // so it shouldn't have aborted again
        assert(staller.isIrrevocable());
        assert(staller.abortCount() == initialAborts);
        assert(!staller.hasPendingAbort());
        (*stallCount)--;
        if (!(*stallCount)) {
            stallers.notify(staller.runningTid);
        }
        t.unregisterObserver(this);
    }

  public:
    using Unstaller::Unstaller;
    void start(ThreadID) override { assert(false); }
    // Should only observe finished tasks
    void finish() override { assert(false); }
    void commit() override { unstall(); }
    void abort(bool requeue) override { unstall(); }
};
