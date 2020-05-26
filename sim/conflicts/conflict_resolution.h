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

#include <cstdint>

#include "sim/atomic_port.h"
#include "sim/taskptr.h"

// Start with child aborts. They are always in the network.
// The corrective writes of the undo log are difficult for two reasons:
// 1) The invalidations for a write that happens from the core
//    are already in the network via the memory hierarchy.
// 2) The corrective writer may already have the line cached, which would
//    greatly reduce the latency and bandwidth requirement. Do I probe the
//    nearby L2?

struct AbortReq {
    TaskPtr task;
    uint32_t tileIdx;
    uint32_t depth; // Depth in the abort cascade

    // * Child's Task Queue index: 8 to 10 bits,
    //   plus some way to uniquely identify the child (in case it committed)
    // * Parent's Task Queue index: 8 to 10 bits,
    //   since the receiving tile must send an AbortAck to the right tile, and
    //   the child doesn't store its parent's TQ index, only an isTied bit.
    uint32_t bytes() const {
        return 2  /* child TQ */
             + 2; /* parent TQ */
    }
};

struct AbortAck {
    // Parent's Task Queue index: 8 to 10 bits.
    uint32_t bytes() const { return 2; }
    uint32_t bytes(const AbortReq&) const { return bytes(); }
};

using ChildAbortPort = AtomicPort<const AbortReq, AbortAck>;
