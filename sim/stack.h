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

#include <cstdlib>
#include <vector>
#include "sim/assert.h"
#include "sim/log.h"
#include "sim/types.h"
#include "sim/alloc/alloc.h"

#define _unused(x) ((void)x)

#undef DEBUG
#if 0
#define DEBUG(args...) infos(args)
#else
#define DEBUG(args...)
#endif

// This class assumes that all Swarm worker threads have equally sized stacks,
// and these stacks form a contiguous range of memory addresses.
class StackSegments {
public:
    StackSegments(uint32_t threads, uint32_t logSegmentSize)
      : threads_(threads), logSize_(logSegmentSize)
    {
        // Allocate stacks from program memory
        void* stacksBase = app_mem_memalign(4096, threads_ * (1ul << logSize_));
        assert(stacksBase);
        base_ = reinterpret_cast<Address>(stacksBase);
        DEBUG("StackSegments: base 0x%lx", base_);
    }

    virtual ~StackSegments() {
        free(reinterpret_cast<void*>(base_));
    }

    Address base() const { return base_; }

    inline uint32_t segment(Address addr) const {
        if (addr < base_ || ((addr - base_) >> logSize_) >= threads_) return -1;
        else return (addr - base_) >> logSize_;
    }
private:
    const uint32_t threads_;
    const uint32_t logSize_;
    Address base_;
};


class StackTracker {
public:
    StackTracker(uint32_t threads, uint32_t logSegmentSize)
      : stack_(threads, logSegmentSize), owners_(threads, INVALID_TID)
    { }

    void setAddressRange(Address base, ThreadID owner) {
        DEBUG("StackTracker: Thread %d stack base 0x%lx", owner, base);
        uint32_t segment = stack_.segment(base);
        assert(segment < owners_.size());
        assert(owners_.at(segment) == INVALID_TID);
        owners_.at(segment) = owner;
    }

    inline ThreadID owner(Address addr) const {
        const uint32_t segment = stack_.segment(addr);
        if (segment < owners_.size()) return owners_[segment];
        else return INVALID_TID;
    }

    void* base() const {
        return reinterpret_cast<void*>(stack_.base());
    }

private:
    StackSegments stack_;
    // Mapping of address segment to owning ThreadID
    std::vector<ThreadID> owners_;
};

