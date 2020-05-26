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

#include "sim/commitqueue.h"
#include "sim/assert.h"
#include "sim/log.h"

#undef DEBUG
#define DEBUG(args...) //infos(args)

CommitQueueAdmissionPolicy stringToCQAdmissionPolicy(std::string str) {
    if (str == "Any") return CommitQueueAdmissionPolicy::ANY;
    if (str == "All") return CommitQueueAdmissionPolicy::ALL;
    if (str == "Half") return CommitQueueAdmissionPolicy::HALF;

    panic("Invalid CommitQueueAdmissionPolicy config %s", str.c_str());
}

CommitQueue::CommitQueue(uint32_t capacity, CommitQueueAdmissionPolicy policy)
: CommitQ_ty(capacity), admission(policy, *this), eviction(*this) { }


TaskPtr CommitQueue::replacementCandidateFor(const Task& incoming) const {
    if (admission.accepts(incoming)) {
        return eviction.candidate(incoming);
    } else {
        return nullptr;
    }
}

bool Admission::accepts(const Task& incoming) const {
    assert(!cq.empty());
    const Task* incumbent;
    switch(policy) {
        case CommitQueueAdmissionPolicy::ANY:
            incumbent = cq.max().get(); break;
        case CommitQueueAdmissionPolicy::ALL:
            incumbent = cq.min().get(); break;
        case CommitQueueAdmissionPolicy::HALF:
            {
            auto median = std::next(cq.cbegin(), cq.size() / 2);
            incumbent = (*median).get(); break;
            }
        default:
            panic("Invalid CommitQueueAdmission policy");
    }
    DEBUG("CQ admission incoming %s incumbent %s",
            incoming.toString().c_str(), incumbent->toString().c_str());
    return incoming.ts < incumbent->ts;
}
