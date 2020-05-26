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

#include "sim/alloc/alloc.h"
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "sim/assert.h"
#include "sim/log.h"
#include "sim/pad.h"
#include "sim/sim.h"

#define DLMALLOC_BASEADDR (ALLOC_UNTRACKED_BASEADDR)
#include "sim/alloc/dlmalloc.h.c"

#undef DEBUG
#define DEBUG(args...) //info(args)

static std::vector<mspace> allocPools;

void InitUntrackedAllocPools(uint32_t numCores) {
    allocPools.resize(numCores);
    DEBUG("[alloc] allocPools.data()=%p", allocPools.data());
    for (uint32_t i = 0; i < numCores; i++) {
        allocPools[i] = create_mspace(0, 0);
        DEBUG("[alloc] allocPools[%d]=%p", i, allocPools[i]);
    }
}

// Memory always cleared & cache-aligned, so we can route malloc, calloc, and memalign variants thru
void* untracked_alloc(size_t minSz) {
    // Select pool based on current core, or 0 if invalid
    uint32_t tid = GetCurTid();
    uint32_t pool = (tid == INVALID_TID)? 0 : GetCoreIdx(tid);
    assert(pool < allocPools.size());

    // [mcj] if minSz is a multiple of CACHE_LINE_BYTES, then doesn't this
    // allocate one cache line too many?
    size_t sz = minSz + CACHE_LINE_BYTES - ((minSz) % CACHE_LINE_BYTES);
    void* ptr = mspace_memalign(allocPools[pool], CACHE_LINE_BYTES, sz);
    assert(ptr);
    memset(ptr, 0, sz);
    assert(isUntrackedAddress(ptr));
    //info("[alloc] pool %4d size %8ld - %p", pool, sz, ptr);
    return ptr;
}

void untracked_dealloc(void* ptr) {
    assert(isUntrackedAddress(ptr));
    mspace_free(allocPools[0], ptr);
}
