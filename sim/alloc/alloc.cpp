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
#include "sim/alloc/untracked.h"
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "sim/assert.h"
#include "sim/log.h"
#include "sim/pad.h"
#include "sim/sim.h"
#include "sim/task.h"

// NOTE: Code below handles cases where a single task both allocs and frees the
// same pointer; alloc - free - alloc and other multi-op sequences on the same
// pointer are not possible b/c frees are delayed.

//#define USE_DLMALLOC 1
#ifdef USE_DLMALLOC

#define DLMALLOC_BASEADDR (ALLOC_TRACKED_BASEADDR)
#include "sim/alloc/dlmalloc.h.c"

// DEBUG has special meaning in dlmalloc
#undef DEBUG
#define DEBUG(args...) //info(args)

static std::vector<mspace> allocPools;

void InitAllocPools(uint32_t numCores) {
    allocPools.resize(numCores);
    DEBUG("[alloc] allocPools.data()=%p", allocPools.data());
    for (uint32_t i = 0; i < numCores; i++) {
        allocPools[i] = create_mspace(0, 0);
        DEBUG("[alloc] allocPools[%d]=%p", i, allocPools[i]);
    }
    InitUntrackedAllocPools(numCores);
}

/* Raw alloc/dealloc methods */

// Memory always cleared & cache-aligned, so we can route malloc, calloc, and memalign variants thru
static void* alloc(size_t minSz) {
    // Select pool based on current core, or 0 if invalid
    // IMPORTANT: All program allocs must come through here, even when
    // fast-forwarding, so they can be freed inside simulation
    uint32_t tid = GetCurTid();
    uint32_t pool = (tid == INVALID_TID)? 0 : GetCoreIdx(tid);
    assert(pool < allocPools.size());

    // [mcj] if minSz is a multiple of CACHE_LINE_BYTES, then doesn't this
    // allocate one cache line too many?
    size_t sz = minSz + CACHE_LINE_BYTES - ((minSz) % CACHE_LINE_BYTES);
    void* ptr = mspace_memalign(allocPools[pool], CACHE_LINE_BYTES, sz);
    assert(ptr);
    memset(ptr, 0, sz);
    assert(isTrackedAddress(ptr));
    //info("[alloc] pool %4d size %8ld - %p", pool, sz, ptr);
    return ptr;
}

static void dealloc(void* ptr) {
    if (isTrackedAddress(ptr)) {
        mspace_free(allocPools[0] /*irrelevant but needed by interface*/, ptr);
    } else {
        assert(isUntrackedAddress(ptr));
        untracked_dealloc(ptr);
    }
}

static size_t usable_size(void* ptr) { return mspace_usable_size(ptr); }

#else  // !USE_DLMALLOC

#define PLSALLOC_INCLUDED_FROM_SIM (1)
#define PLSALLOC_TRACKED_BASEADDR (ALLOC_TRACKED_BASEADDR)
// FIXME: In this plsalloc instance, "untracked" really isn't untracked but the
// pool of memory used for internal allocations. Should change plsalloc
// nomenclature to refer to internal arena, and perhaps to stop requiring this
// fixed location.
#define PLSALLOC_UNTRACKED_BASEADDR (ALLOC_UNTRACKED_BASEADDR + (1ul << ALLOC_UNTRACKED_BITS))

static uint32_t __allocPool = -1;
static inline uint32_t getAllocPool() { return __allocPool; }
static inline void setAllocPool(uint32_t p) { __allocPool = p; }

// HACK: For now we need to define these as plsalloc's own internal allocator
// functions. We should generalize plsalloc though. KEEP ALL THESE STATIC.
static void* sim_zero_cycle_untracked_malloc(size_t sz) { return malloc(sz); }
static void sim_zero_cycle_free(void* ptr) { free(ptr); }
static uint32_t sim_get_tid() { return getAllocPool(); }
static void sim_rdrand(uint64_t* rd) { *rd = 0; } // FIXME: Used by BankedCentralFreelist

#include "plsalloc/alloc.h"

#undef DEBUG
#define DEBUG(args...) //info(args)

// NOTE: Code below handles cases where a single task both allocs and frees the
// same pointer; alloc - free - alloc and other multi-op sequences on the same
// pointer are not possible b/c frees are delayed.

void InitAllocPools(uint32_t numCores) {
    DEBUG("[alloc] Initializing pools...");
    InitUntrackedAllocPools(numCores);
}

/* Raw alloc/dealloc methods */

// Memory always cleared & cache-aligned, so we can route malloc, calloc, and memalign variants thru
static void* alloc(size_t minSz) {
    // Select pool based on current core, or 0 if invalid
    // IMPORTANT: All program allocs must come through here, even when
    // fast-forwarding, so they can be freed inside simulation
    uint32_t tid = GetCurTid();
    uint32_t pool = (tid == INVALID_TID)? 0 : GetCoreIdx(tid);
    setAllocPool(pool);
    void* ptr = plsalloc::do_alloc(minSz);
    setAllocPool(-1);
    assert(ptr);
    memset(ptr, 0, plsalloc::chunk_size(ptr));
    assert(isTrackedAddress(ptr));
    DEBUG("[alloc] requested size %8ld, got %8ld-byte chunk from pool %4d - %p",
          minSz, plsalloc::chunk_size(ptr), pool, ptr);
    return ptr;
}

static void dealloc(void* ptr) {
    if (isTrackedAddress(ptr)) {
        uint32_t tid = GetCurTid();
        uint32_t pool = (tid == INVALID_TID)? 0 : GetCoreIdx(tid);
        DEBUG("[alloc] free pool %4d - %p", pool, ptr);
        setAllocPool(pool);
        plsalloc::do_dealloc(ptr);
        setAllocPool(-1);
    } else {
        assert(isUntrackedAddress(ptr));
        DEBUG("[alloc] free untracked - %p", ptr);
        untracked_dealloc(ptr);
    }
}

static size_t usable_size(void* ptr) { return plsalloc::chunk_size(ptr); }

#endif


/* Task alloc context implementation */

void TaskAllocCtxt::handleCommit() {
    assert(!irrevocable);
    for (void* ptr : frees) {
        DEBUG("TaskAllocCtxt commit: free %p", ptr);
        dealloc(ptr);
    }
    reset();
}

void TaskAllocCtxt::handleAbort() {
    assert(!irrevocable);
    for (void* ptr : allocs) {
        DEBUG("TaskAllocCtxt abort: free %p", ptr);
        dealloc(ptr);
    }
    reset();
}

// [mcj] It seems that both lists should be cleared on commit and abort. E.g. on
// abort, if the allocs list wasn't also cleared, then we would repeatedly
// free() previously-freed memory.
void TaskAllocCtxt::reset() {
    frees.clear();
    allocs.clear();
}

void TaskAllocCtxt::makeIrrevocable() {
    assert(!irrevocable);
    handleCommit();
    irrevocable = true;
}

void* TaskAllocCtxt::allocMem(size_t minSz) {
    void* ptr = alloc(minSz);
    assert(ptr);
    if (!irrevocable) allocs.push_back(ptr);
    return ptr;
}

void* TaskAllocCtxt::allocUntrackedMem(size_t minSz) {
    void* ptr = untracked_alloc(minSz);
    assert(ptr);
    if (!irrevocable) allocs.push_back(ptr);
    return ptr;
}

void TaskAllocCtxt::freeMem(void* ptr) {
    // http://linux.die.net/man/3/malloc
    // "If ptr is NULL, no operation is performed."
    if (ptr != nullptr) {
        if (!irrevocable) frees.push_back(ptr);
        else dealloc(ptr);
    }
}

TaskAllocCtxt::~TaskAllocCtxt() {
    assert(allocs.empty());
    assert(frees.empty());
}


/* Replacement routines */

static inline bool useAllocCtxt(Task* t) {
    return t && !IsPriv(t->runningTid);
}

void* repl_malloc(size_t size) {
    Task* t = GetCurTask();
    void* ptr = useAllocCtxt(t) ? t->allocCtxt.allocMem(size) : alloc(size);
    DEBUG("[%d] alloc: malloc %p of size %ld from task %s",
            GetCurTid(),
            ptr, size,
            t? t->toString().c_str() : "null");
    return ptr;
}

void repl_free(void* ptr) {
    Task* t = GetCurTask();
    DEBUG("[%d] alloc: free %p from task %s",
            GetCurTid(),
            ptr,
            t? t->toString().c_str() : "null");
    useAllocCtxt(t) ? t->allocCtxt.freeMem(ptr) : dealloc(ptr);
}

void* repl_realloc(void* ptr, size_t newSz) {
    assert(isTrackedAddress(ptr));
    Task* t = GetCurTask();
    DEBUG("[%d] alloc: realloc %ld from task %s",
            GetCurTid(),
            newSz,
            t? t->toString().c_str() : "null");
    if (useAllocCtxt(t)) {
        size_t curSz = usable_size(ptr);
        void* newPtr = t->allocCtxt.allocMem(newSz);
        size_t copySz = std::min(curSz, newSz);
        memcpy(newPtr, ptr, copySz);
        t->allocCtxt.freeMem(ptr);
        return newPtr;
    } else {
#if USE_DLMALLOC
        return mspace_realloc(allocPools[0], ptr, newSz);
#else
        size_t curSz = usable_size(ptr);
        void* newPtr = alloc(newSz);
        size_t copySz = std::min(curSz, newSz);
        memcpy(newPtr, ptr, copySz);
        dealloc(ptr);
        return newPtr;
#endif
    }
}

int repl_posix_memalign(void** memptr, size_t alignment, size_t size) {
    if (alignment > CACHE_LINE_BYTES) warn("repl_posix_memalign() violating alignment req %ld", alignment);
    *memptr = repl_malloc(size);
    if (!*memptr) return ENOMEM;
    return 0;
}

size_t repl_malloc_usable_size(void* ptr) {
    assert(isTrackedAddress(ptr));
    return usable_size(ptr);
}

void* repl_untracked_malloc(size_t size) {
    Task* t = GetCurTask();
    void* ptr = useAllocCtxt(t)? t->allocCtxt.allocUntrackedMem(size) : untracked_alloc(size);
    DEBUG("[%d] alloc: untracked_malloc %p of size %ld from task %s",
            GetCurTid(),
            ptr, size,
            t? t->toString().c_str() : "null");
    return ptr;
}

void* app_mem_memalign(size_t alignment, size_t bytes) {
#ifdef USE_DLMALLOC
    return mspace_memalign(allocPools[0], alignment, bytes);
#else
    // FIXME: Why does this method exist? The only user is stack.h...
    assert((alignment & (alignment - 1)) == 0);  // ensure it's a power of 2
    uintptr_t uptr = (uintptr_t) alloc(bytes + alignment - 1);
    uptr = (uptr + alignment - 1) & (~(alignment - 1));
    return (void*) uptr;
#endif
}
