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

#include <boost/multi_index/global_fun.hpp>  // for RunQ_ty
#include "sim/collections/ordered_set.h"
#include "sim/sim.h"
#include "sim/stats/stats.h"
#include "sim/task.h"

using ExecQ_ty = fixed_capacity_ordered_set<
        TaskPtr, ordered_non_unique<identity<TaskPtr>,
        TaskTimeLTDeprioritizeIrrevocablesComparator>>;

// The RunQ sorts tasks by their timestamp and deprioritizes producers. It uses
// a global_fun key extractor to allow calls to lower/upper_bound to use keys.
using RunQ_key_ty = std::tuple<const TimeStamp&, uint64_t, uint8_t>;
inline RunQ_key_ty getRunQKey(const TaskPtr& t) {
    // Given an equal choice between running a programmer-defined producer and a
    // requeuer, choose the normal producer
    return std::make_tuple(std::cref(t->lts()), t->softTs,
                           (t->isProducer() << 1) | t->isRequeuer());
}
using RunQ_ty = ordered_pointer_set<TaskPtr, ordered_non_unique<global_fun<const TaskPtr&, RunQ_key_ty, getRunQKey>>, RunQ_key_ty>;

using AbortQ_ty = ordered_pointer_set<TaskPtr, ordered_non_unique<identity<TaskPtr>, TaskTimeLTComparator>>;

// Adaptor class for ROB queues that automatically profiles queue size
// FIXME: This is missing last sample before each dump
template <typename QueueType> struct SelfProfilingQueue : public QueueType {
    template <typename StatType> struct SelfProfilingStat : public StatType {
        SelfProfilingQueue<QueueType>* q;
        SelfProfilingStat(SelfProfilingQueue<QueueType>* q)
            : StatType(), q(q) {}
        void update() override { q->profile(); }
    };

    // Initialized externally (we don't implement initStats here)
    SelfProfilingStat<RunningStat<size_t>> sizeStat;
    SelfProfilingStat<Histogram<size_t>> sizeHist;

  private:
    uint64_t lastProfileCycle = 0;

    inline void profile() {
        uint64_t cycle = getCurCycle();
        if (cycle == lastProfileCycle) return;
        assert(cycle > lastProfileCycle);
        uint64_t weight = cycle - lastProfileCycle;
        sizeStat.push(QueueType::size(), weight);
        sizeHist.insert(QueueType::size(), weight);
        lastProfileCycle = cycle;
    }

  public:
    template <typename... Args>
    SelfProfilingQueue(Args... args)
        : QueueType(args...), sizeStat(this), sizeHist(this) {}

    void insert(const TaskPtr& t) {
        profile();
        QueueType::insert(t);
    }
    void erase(const TaskPtr& t) {
        profile();
        QueueType::erase(t);
    }
};
