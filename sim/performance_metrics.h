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

#include <boost/range/combine.hpp>
#include "rob_stats_collector.h"

namespace perfMetric {
struct StallAndAborts {
    uint64_t stall;
    uint64_t abort;

    StallAndAborts operator-(const StallAndAborts& that) const {
        return StallAndAborts{this->stall - that.stall,
                              this->abort - that.abort};
    }

    bool operator>=(const StallAndAborts& that) const {
        return (this->stall >= that.stall) && (this->abort >= that.abort);
    }
};
};

inline std::vector<uint64_t> getCommittedInstructions(RobStatsCollector* c) {
    return c->getCoresEventCount(
        RobStatsCollector::CoreEventType::COMMIT_INSTRS);
}

inline std::vector<uint64_t> getAbortedInstructions(RobStatsCollector* c) {
    return c->getCoresEventCount(
        RobStatsCollector::CoreEventType::ABORT_INSTRS);
}

inline std::vector<perfMetric::StallAndAborts> getStallAndAborts(
    RobStatsCollector* c) {
    auto stalls = c->getCoresEventCount(
        RobStatsCollector::CoreEventType::STALL_SB_CYCLES);
    auto aborts =
        c->getCoresEventCount(RobStatsCollector::CoreEventType::ABORT_INSTRS);
    assert(stalls.size() == aborts.size());
    std::vector<perfMetric::StallAndAborts> values;
    for (const auto& zipped : boost::combine(stalls, aborts))
        values.push_back(
            perfMetric::StallAndAborts{zipped.get<0>(), zipped.get<1>()});
    return values;
}

// Returns v1 - v2
template <typename T>
inline std::vector<T> metricDiff(std::vector<T> v1, std::vector<T> v2) {
    assert(v1.size() == v2.size()); // same size
    assert(std::equal(std::begin(v1), std::end(v1), std::begin(v2),
                      [](T a, T b) -> bool { return a >= b; }));
                        // each elem of v1 >= corresponding elem of v2
    std::vector<T> result(v1.size());
    std::transform(v1.begin(), v1.end(), v2.begin(), result.begin(),
                   std::minus<T>());
    return result;
}
