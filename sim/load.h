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

#include <vector>
#include <numeric>
#include <cmath>
#include <algorithm>
#include "sim/log.h"
#include "sim/sim.h"
#include "sim/stats/stats.h"
#include "sim/core/core.h"
#include "sim/collections/enum.h"
#include "sim/rob.h"
#include "sim/loadstatssummary.h"

using std::vector;

#undef DEBUG
#define DEBUG(args...) //info(args)

class LoadArray {
  public:
    enum LoadMetric { COMMIT_INSTRS, COMMIT_CYCLES, NUM_ELEMENTS };

    LoadArray() {}

    void init(uint32_t r, uint32_t c = 1, LoadMetric m = COMMIT_CYCLES) {
        numRows = r;
        numCols = c;
        metric  = m;
        loadStatsArray.resize(r);
        for (uint32_t i = 0; i < loadStatsArray.size(); ++i)
            loadStatsArray[i].resize(c);
    }

    LoadStatsSummary* getLoadStatsSummary(uint32_t r, uint32_t c) {
        return &loadStatsArray[r][c];
    }

    uint64_t getLoad(uint32_t r, uint32_t c) {
        if (metric != NUM_ELEMENTS) return loadStatsArray[r][c].executed;
        else return loadStatsArray[r][c].idleTasks;
    }

    void spillTask(uint32_t r, uint32_t c) {
        assert(loadStatsArray[r][c].idleTasks > 0);
        loadStatsArray[r][c].idleTasks -= 1;
    }

    void stealTask(uint32_t r, uint32_t c) {
        assert(loadStatsArray[r][c].idleTasks > 0);
        loadStatsArray[r][c].idleTasks -= 1;
    }

  private:
    uint32_t numRows, numCols;
    LoadMetric metric;
    vector<vector<LoadStatsSummary>> loadStatsArray;
};

class UniformityPolicy {
  protected:
    typedef vector<uint64_t> SampleArray;

  public:
    virtual bool isUniform(const SampleArray& sa) = 0;
};

class ChiSquare : public UniformityPolicy {
  private:
    const double threshold[74];

  public:
    // Data sourced from
    // http://wps.aw.com/aw_weiss_introstats_9/177/45509/11650342.cw/index.html
    // threshold[i] gives the chisquared value for degree of freedom = (i+1)
    // such that the probability distance from a chisquare distribution is
    // 0.05. A chisquare value larger than threshold indicates significant
    // deviation from the null hypothesis.
    ChiSquare()
        : threshold{3.841,  5.991,  7.815,  9.488,  11.070, 12.592, 14.067,
                    15.507, 16.919, 18.307, 19.675, 21.026, 22.362, 23.685,
                    24.996, 26.296, 27.587, 28.869, 30.144, 31.410, 32.671,
                    33.924, 35.172, 36.415, 37.652, 38.885, 40.113, 41.337,
                    42.557, 43.773, 44.985, 46.194, 47.400, 48.602, 49.802,
                    50.998, 52.192, 53.384, 54.572, 55.758, 56.942, 58.124,
                    59.304, 60.481, 61.656, 62.830, 64.001, 65.171, 66.339,
                    67.505, 68.669, 69.832, 70.993, 72.153, 73.311, 74.468,
                    75.624, 76.778, 77.931, 79.082, 80.232, 81.381, 82.529,
                    83.675, 84.821, 85.965, 87.108, 88.250, 89.391, 90.531,
                    91.670, 92.808, 93.945, 95.081} {}

    bool isUniform(const SampleArray& sampleCount) override {
        uint64_t range = sampleCount.size();
        assert(range <= 74);  // we only have data for range <= 74 for now

        uint64_t sum = 0;
        for (uint64_t n : sampleCount) sum += n;
        double expected = 1.0 * sum / range;  // Uniform distribution expected

        double chisquared = 0.0;
        for (uint32_t i = 0; i < range; ++i) {
            DEBUG("Element %u, sampleCount: %lu", i, sampleCount[i]);
            chisquared += std::pow(sampleCount[i] - expected, 2) / expected;
        }

        DEBUG("Expected: %.6f, chisquared: %.6f, threshold: %.6f", expected, chisquared, threshold[(range-1)-1]);
        return (chisquared < threshold[(range-1)-1]);
    }
};

class MaximumAbsoluteDeviation : public UniformityPolicy {
  private:
    const double threshold;

  public:
    MaximumAbsoluteDeviation(double t) : threshold(t) {}

    bool isUniform(const SampleArray& sampleCount) override {
        double mean = static_cast<double>(std::accumulate(
                          sampleCount.begin(), sampleCount.end(), 0)) /
                      static_cast<double>(sampleCount.size());

        vector<double> absDeviation;
        for (auto& x : sampleCount)
            absDeviation.push_back(std::abs(x - mean) / mean);

        if (*std::max_element(absDeviation.begin(), absDeviation.end()) >
            threshold)
            return false;
        else
            return true;
    }
};
