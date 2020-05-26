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

#include "sim/timestamp.h"

// Please see the "Using caches to filter checks" paragraphs in the MICRO'15
// and TopPicks'16 papers for the theory.  Some caches must hold "canary"
// timestamps which must be compared against the accessing task's timestamp on
// all cache hits.  If the task's timestamp is greater, it is allowed to use
// the cached data without further conflict checking, but if the canary is
// greater, then conflict checking must proceed further out in the memory
// heirarchy.

// This file contains logic needed to adjust canary timestamps during zooming,
// i.e., changes to the frame for Fractal timestamps.
// TODO(victory): Move more logic to this file from TerminalCD and tsarray.h?

struct CanaryTS : public TimeStamp {
    // A wrapper around TimeStamp, with no additional state
    CanaryTS(const TimeStamp& other) : TimeStamp(other) {}
    // Users can implicitly convert or copy this into an ordnary TimeStamp,
    // with no risk of slicing, because CanaryTS has no additional state.

    // During zooming, canaries must be adjusted to stay "in frame", i.e.,
    // within the range and resolution representable by fixed-width hardware.

    void adjustOnZoomIn(const TimeStamp& frameMin, const TimeStamp& frameMax) {
        if (isZoomInvariant()) return;

        // Since the GVT is always in-frame, any timestamps before frameMin
        // must have been passed over and are effectively committed.
        // Therefore, clearing the canary to zero should be safe.
        if (operator<(frameMin)) operator=(ZERO_TS);

        // Decreasing the canary risks correctness errors if it incorrectly
        // filters conflict checks.  Should we be okay as long as no task
        // with timestamp beyond frameMax runs and is served by this cache?
        // What if the canary is propagated to another cache?
        //if (operator>(frameMax)) operator=(frameMax);
        // [victory] I think the above is dangerous, let's be more conservative
        // and force subsequent accesses to perform more conflict checks.
        if (operator>(frameMax)) operator=(INFINITY_TS);
    }

    void adjustOnZoomOut(uint64_t maxDepth) {
        if (isZoomInvariant()) return;

        // It can't hurt correctness to increase the canary to deal with
        // the loss of resolution.
        //TODO(victory): Why are we increasing the canary so much? Wouldn't
        // it do to just round it up to the nearest representable timestamp?
        assert(domainDepth() <= maxDepth + 1);
        if (domainDepth() > maxDepth)
            operator=(getTimeStampAtLevel(maxDepth - 1).domainMax());
    }

  private:
    // Certain special timestamp values are invariant under zooming,
    // i.e., they have a fixed representation in hardware
    // that does not need to be adjusted to maintain domain relationships.
    bool isZoomInvariant() const {
        return operator==(ZERO_TS)
            || operator==(IRREVOCABLE_TS)
            || operator==(INFINITY_TS)
            || operator==(NO_TS);
    }
};
