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
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include "sim/assert.h"
#include "sim/log.h"
#include "sim/robtypes.h"
#include "sim/sim.h"

#undef DEBUG
#define DEBUG(args...) //info(args)

static constexpr int64_t MINTASKSNEEDED = -2;

struct SpillerShortageReturnType {
    uint64_t domainId;
    uint64_t depth;
    bool shortage;
};

class OverflowWatcher {
    struct value_t {
        uint64_t domainId;
        int64_t value;
        uint64_t depth;
    };

    const RunQ_ty& runQ;
    std::function<size_t(void)> taskQueueSize;

    // Number of ROB tasks at which we trigger a spiller task
    const int64_t overflowMarker;
    // Number of removable tasks to "cache" (or protect) in the task queue
    const int64_t removableCacheCapacity;
    std::unordered_map<uint64_t, int64_t> outstanding;

  public:
    OverflowWatcher(const RunQ_ty& rQ, std::function<size_t(void)> tqSize,
                    uint32_t overflowMarker_, uint32_t removableCacheCapacity_)
        : runQ(rQ),
          taskQueueSize(tqSize),
          overflowMarker(static_cast<int64_t>(overflowMarker_)),
          removableCacheCapacity(static_cast<int64_t>(removableCacheCapacity_))
    {
        assert(overflowMarker > 0);
    }

    uint64_t neededSpillers() const {
        const auto neededPerDomain = neededSpillersPerDomain();
        int64_t sum = 0;
        for (auto& it : neededPerDomain)
            sum += it.value;
        assert(sum >= 0);
        return static_cast<uint64_t>(sum);
    }

    SpillerShortageReturnType spillerShortage() const {
        SpillerShortageReturnType ret = {0ul /*domainId*/, 0ul /*domainDepth*/,
                                         false /*hasSpillerShortage*/};
        const auto neededPerDomain = neededSpillersPerDomain();
        for (auto it = neededPerDomain.rbegin(); it != neededPerDomain.rend();
             ++it) {
            const auto element = *it;
            uint64_t key = element.domainId;
            int64_t out = outstanding.count(key) ? outstanding.at(key) : 0;
            DEBUG(
                "spillerShortage, domain: %lu, needed: %ld, outstanding %ld",
                element.domainId, element.value, out);
            if (element.value > out) {
                ret.domainId = key;
                ret.depth = element.depth;
                ret.shortage = true;
                return ret;
            }
        }
        return ret;
    }

    void acquireSpiller(uint64_t domainId) {
        outstanding[domainId]++;
    }

    void releaseSpiller(uint64_t domainId) {
        assert_msg(outstanding[domainId] > 0, "domainId: %lu", domainId);
        outstanding[domainId]--;
        if (outstanding[domainId] == 0) outstanding.erase(domainId);
    }

    bool anySpillersOutstanding() const {
        return std::any_of(outstanding.cbegin(), outstanding.cend(),
                [] (const std::pair<uint64_t,int64_t>& p) {
                    return p.second > 0;
                });
    }

  private:
    std::vector<value_t> neededSpillersPerDomain() const {
        if (runQ.empty()) return {};
        // A task queue *at* the overflow marker has triggered an overflow,
        // hence subtract 1 from the marker. This permits us to leave the
        // overflow marker equal to capacity, easing configurability (not that
        // setting the overflow that high is a good performance idea).
        int64_t excessTasks = static_cast<int64_t>(taskQueueSize())
                              - (overflowMarker - 1);
        if (excessTasks <= 0) return {};

        std::vector<value_t> removablePerDomain;
        // The first untied task is not removable, otherwise we risk
        // replacing an untied task with a requeuer, achieving no space
        // savings, and risking no liveness guarantee. Surprisingly neither
        // can we remove the second untied task for liveness...
        // TODO reason about why. Maybe if the GVT task enqueued a task it
        // would be gobbled by a spiller?
        // FIXME with an unbounded queue, the following O(n) operation is
        // exacerbated. It was a quick hack when written, but isn't a good
        // idea.
        int64_t removableTasks = -removableCacheCapacity;

        TaskPtr minTsTask = runQ.min();
        uint64_t minTaskDomainId = minTsTask->ts.domainId();
        bool minTaskIsSpillable =
            !minTsTask->isTied() && !minTsTask->hasPendingAbort();

        for (auto it = runQ.crbegin(); it != runQ.crend(); ++it) {
            const TaskPtr& t = *it;
            bool canSpill = !t->isTied() && !t->hasPendingAbort();
            removableTasks += canSpill;
            if (canSpill) {
                uint64_t thisTaskDomainId = t->ts.domainId();
                auto it = std::find_if(
                    removablePerDomain.begin(),
                    removablePerDomain.end(), [=](value_t& v) {
                        return v.domainId == thisTaskDomainId;
                    });
                // Each domain needs to have at least two tasks for a
                // spiller to be worthwhile, hence we set INIT to
                // MINTASKSNEEDED + 1 (=-1).  In addition, we protect
                // the very first task (minTs task) in the runQ to
                // prevent livelock. So other than the first task, that
                // domain needs an additional two tasks for spillers
                // to be worthwhile. Hence for the domain the first task
                // belongs to we set INIT to MINTASKSNEEDED (=-2).  If
                // the first task is not spillable, it will not be
                // removed, hence we don't need this fix.
                if (it == removablePerDomain.end()) {
                    bool sameDomainAsMinTask =
                        (thisTaskDomainId == minTaskDomainId) &&
                        minTaskIsSpillable;
                    int64_t INIT =
                        MINTASKSNEEDED + !sameDomainAsMinTask;
                    DEBUG("%s new domain, domainId: %lu",
                          t->toString().c_str(), thisTaskDomainId);
                    removablePerDomain.push_back({thisTaskDomainId,
                                                  INIT + 1,
                                                  t->ts.domainDepth()});
                } else {
                    DEBUG("%s existing domain, domainId: %lu",
                          t->toString().c_str(), thisTaskDomainId);
                    (*it).value++;
                }
            }
            // dsm: Since we take the minimum of removable and
            // excessTasks, cut the search short to improve performance
            // [ssub] I don't think removableTasks accurately captures
            // the actual number of removable tasks anymore. Ideally
            // removableTasks would be \sum removablePerDomain.value.
            if (removableTasks >= excessTasks) break;
        }
        DEBUG("excessTasks %ld removableTasks %ld", excessTasks,
              removableTasks);
        // Counting untied tasks in hardware:
        // 1) have a bit vector indicating whether a task is untied, and
        //    leverage the AVX popcount instruction in a few cycles OR
        // 2) each ROB could have a counter of untied tasks, C_u. When an
        //    untied task commits, decrement C_u. When a parent of some local
        //    tied tasks commit (either remote or local), increment C_u
        //    accordingly.

        // A spiller is needed only if
        // * the queue size has exceeded the marker and
        // * there are at least two untied tasks to remove
        // Note that we round up the number of needed spillers.
        std::vector<value_t> neededPerDomain;
        for (auto const& element : removablePerDomain) {
            if (element.value > 0) {
                neededPerDomain.push_back({
                    element.domainId,
                    (element.value + ossinfo->tasksPerSpiller - 1) /
                        ossinfo->tasksPerSpiller,
                    element.depth});
            }
            DEBUG("neededSpillers (domain %lu): %ld", element.domainId,
                  neededPerDomain.back().value);
        }
        return neededPerDomain;
    }

};

class FrameOverflowWatcher {
    const RunQ_ty& runQ;
    int64_t outstanding;

public:
    FrameOverflowWatcher(const RunQ_ty& rQ) : runQ(rQ) {}

    int64_t neededSpillers(const TimeStamp& frameMax) const {
        int64_t removableTasks = 0;
        int needed = 0;
        for (const TaskPtr& t : runQ) {
            if (t->ts > frameMax)
                removableTasks += !t->isTied() && !t->hasPendingAbort();
        }
        needed = (removableTasks +
                      ossinfo->tasksPerSpiller - 1) /
                     ossinfo->tasksPerSpiller;
        DEBUG("neededFrameSpillers %d", needed);
        return needed;
    }

    bool spillerShortage(const TimeStamp& frameMax) const {
        DEBUG("frameSpillerShortage, outstanding %ld", outstanding);
        return (neededSpillers(frameMax) - outstanding > 0);
    }

    void acquireSpiller() {
        outstanding++;
    }

    void releaseSpiller() {
        assert(outstanding > 0);
        outstanding--;
    }
};
