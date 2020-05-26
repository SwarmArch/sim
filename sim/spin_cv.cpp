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

#include "sim/spin_cv.h"
#include "sim/assert.h"
#include "sim/log.h"
#include "sim/sim.h"

#include <algorithm>
#include <random>
#include <vector>

#undef DEBUG
#define DEBUG(args...) //info(args)

static std::mt19937 PRNG(0);

void SPinConditionVariable::addWaiter(ThreadID tid) {
    DEBUG("spin_cv[%p] add tid %d", this, tid);
    auto result = waiters.insert(tid);
    assert(result.second);
}

// If tid is not in the waiters set, exit gracefully
void SPinConditionVariable::notify(ThreadID tid) {
    if (waiters.erase(tid)) {
        DEBUG("spin_cv[%p] wake tid %d", this, tid);
        UnblockThread(tid, getCurCycle());
    }
}

void SPinConditionVariable::notifyOne() {
    if (!waiters.empty()) {
        // Random selection (unfortunately O(n) in the number of watiers).
        std::uniform_int_distribution<uint32_t> randomIdx(0, waiters.size()-1);
        auto it = std::next(waiters.cbegin(), randomIdx(PRNG));
        const ThreadID tid = *it;
        waiters.erase(it);
        DEBUG("spin_cv[%p] wake tid %d", this, tid);
        // FIXME(dsm): What cycle?
        UnblockThread(tid, getCurCycle());
    }
}

void SPinConditionVariable::notifyAll() {
    // Randomize the wakeup order to avoid systemic bias in access to resources
    std::vector<ThreadID> randomized(waiters.begin(), waiters.end());
    std::shuffle(randomized.begin(), randomized.end(), PRNG);
    for (const ThreadID tid : randomized) {
        DEBUG("spin_cv[%p] Wake waiter %d", this, tid);
        UnblockThread(tid, getCurCycle());
    }
    waiters.clear();
}
