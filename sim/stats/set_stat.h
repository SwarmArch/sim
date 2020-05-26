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

#include <unordered_set>
#include <vector>
#include <time.h>
#include "sim/stats/stats.h"
#include "sim/sim.h"  // for getCurCycle()
#include "sim/str.h"

/* Profiles the number of unique occurrences of an event type at different
 * timescales. Useful to characterize the time-varying behavior of a population
 * (e.g., how many unique lines are accessed at different intervals)
 */
template <typename K>
class SetStat : public AggregateStat {
  private:
    struct Interval {
        std::unordered_set<K> elems;
        RunningStat<size_t> sizeStat;
        uint64_t epoch;
    };

    std::vector<Interval> intervals;
    uint32_t minBits;
    uint32_t bitsPerStep;

  public:
    void init(const char* name, const char* desc, uint32_t _minBits,
              uint32_t maxBits, uint32_t _bitsPerStep = 1) {
        initStat(name, desc);
        minBits = _minBits;
        bitsPerStep = _bitsPerStep;

        assert(bitsPerStep);
        assert(maxBits >= minBits);
        uint32_t steps = (maxBits - minBits + bitsPerStep) / bitsPerStep;
        assert(steps);
        intervals.resize(steps);

        for (uint64_t l = 0; l < intervals.size(); l++) {
            uint64_t bits = minBits + l * bitsPerStep;
            Interval& i = intervals[l];

            i.epoch = 0;
            std::string sname = Str(1ul << bits);
            std::string sdesc = "Set size on " + sname + "-cycle intervals";
            i.sizeStat.init(strdup(sname.c_str()), strdup(sdesc.c_str()));
            this->append(&i.sizeStat);
        }
    }

    void push(K key) {
        update();
        intervals[0].elems.insert(key);
    }

    void update() {
        uint64_t cycle = getCurCycle();
        for (uint64_t l = 0; l < intervals.size(); l++) {
            uint64_t bits = minBits + l * bitsPerStep;
            Interval& i = intervals[l];
            uint64_t epoch = cycle >> bits;
            assert(epoch >= i.epoch);
            if (epoch == i.epoch) break;

            // Close epoch
            i.sizeStat.push(i.elems.size(), 1);
            if (l < intervals.size() - 1) {
                // NOTE(dsm): std::set has more efficient merges... if
                // performance is problem, consider switching
                for (auto& e : i.elems) intervals[l+1].elems.insert(e);
            }
            i.elems.clear();
            i.epoch++;

            // Account for long profiling periods
            if (epoch > i.epoch) {
                i.sizeStat.push(0, epoch-i.epoch);
                i.epoch = epoch;
            }
        }

    }
};
