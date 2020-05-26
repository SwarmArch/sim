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

#include <string>
#include "sim/collections/ordered_set.h"
#include "sim/task.h"

using CommitQ_ty = fixed_capacity_ordered_set<TaskPtr, ordered_non_unique<identity<TaskPtr>, TaskTimeLTComparator>>;

// Admit an incoming task to a full CQ if the task precedes ALL/HALF/ANY tasks
enum class CommitQueueAdmissionPolicy { ALL, HALF, ANY };
CommitQueueAdmissionPolicy stringToCQAdmissionPolicy(std::string str);

class Admission {
    const CommitQueueAdmissionPolicy policy;
    const CommitQ_ty& cq;
public:
    Admission(CommitQueueAdmissionPolicy p, const CommitQ_ty& _cq)
        : policy(p), cq(_cq) { }

    bool accepts(const Task& incoming) const;
};

/**
 * Select a candidate task for eviction/abort.
 * For now just selects the "most speculative" task in the CQ. But later we
 * could support other policies:
 *   - most speculative
 *   - random
 *   - least number of cycles completed
 *   - largest memory footprint
 *
 *  FIXME(dsm): Suffers from classitis. Refactor.
 */
class Eviction {
    const CommitQ_ty& cq;
public:
    Eviction(const CommitQ_ty& _cq) : cq(_cq) { }
    TaskPtr candidate(const Task& incoming) const {
        for (auto it = cq.crbegin(); it != cq.crend(); it++) {
            if (!(*it)->isIrrevocable() && (*it != cq.min())) {
                if ((*it)->ts <= incoming.ts) break;
                return *it;
            }
        }
        return nullptr;
    }
};


/**
 * The Commit Queue is an ordered set with configurable replacement policy.
 */
class CommitQueue : public CommitQ_ty {
public:
    CommitQueue(uint32_t capacity, CommitQueueAdmissionPolicy policy);
    TaskPtr replacementCandidateFor(const Task& incoming) const;
private:
    Admission admission;
    Eviction eviction;
};
