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

#include <time.h>
#include "sim/stats/stats.h"

// Helper function
inline uint64_t getNs() {
    struct timespec ts;
    // Guaranteed synchronized across processors, ~20ns/call on Ubuntu 12.04.
    // Linux hrtimers have gotten really good! In comparison, rdtsc is 9ns.
    clock_gettime(CLOCK_REALTIME, &ts);
    return 1000000000L * ts.tv_sec + ts.tv_nsec;
}

/* Implements a single stopwatch-style cumulative clock. Useful to profile
 * isolated events. get() accounts for current interval if clock is running.
 */
class ClockStat : public ScalarStat {
  private:
    uint64_t startNs;
    uint64_t totalNs;

  public:
    ClockStat() : ScalarStat(), startNs(0), totalNs(0) {}

    void start() {
        assert(!startNs);
        startNs = getNs();
    }

    void end() {
        assert(startNs);
        uint64_t endNs = getNs();
        assert(endNs >= startNs) totalNs += (endNs - startNs);
        startNs = 0;
    }

    uint64_t get() const {
        return totalNs + (startNs ? (getNs() - startNs) : 0);
    }
};
