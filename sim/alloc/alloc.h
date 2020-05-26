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

// 0-cycle transactional malloc; frees us from malloc-induced conflicts & false sharing
// NOTE: This does not model allocation overheads; for a proper allocator, use plsalloc

#include <list>
#include <stddef.h>
#include <stdint.h>

// Each task should get one of these, and use it for all allocs and frees
class TaskAllocCtxt {
    private:
        std::list<void*> allocs;
        std::list<void*> frees;
        bool irrevocable = false;
        void reset();
    public:
        virtual ~TaskAllocCtxt();
        void makeIrrevocable();
        void handleCommit();
        void handleAbort();
        bool empty() const { return allocs.empty() && frees.empty(); }

        void* allocMem(size_t minSz); // always cleared & cache-aligned
        void freeMem(void* ptr);
        void* allocUntrackedMem(size_t minSz);
};

void InitAllocPools(uint32_t numCores);

#define ALLOC_TRACKED_BASEADDR (0x0a0000000000ul)
#define ALLOC_UNTRACKED_BASEADDR (0x0b0000000000ul)
#define ALLOC_TRACKED_BITS (40ul)
#define ALLOC_UNTRACKED_BITS ALLOC_TRACKED_BITS
#define ALLOC_BITS (ALLOC_TRACKED_BITS+1)  // 2 TB, 1 per pool

inline bool isAllocAddress(void* addr) {
    return (((uintptr_t)addr) >> ALLOC_BITS) ==
           (ALLOC_TRACKED_BASEADDR >> ALLOC_BITS);
}
inline bool isTrackedAddress(void* addr) {
    return (((uintptr_t)addr) >> ALLOC_TRACKED_BITS) ==
           (ALLOC_TRACKED_BASEADDR >> ALLOC_TRACKED_BITS);
}
inline bool isUntrackedAddress(void* addr) {
    return (((uintptr_t)addr) >> ALLOC_UNTRACKED_BITS) ==
           (ALLOC_UNTRACKED_BASEADDR >> ALLOC_UNTRACKED_BITS);
}

void* repl_malloc(size_t);
void repl_free(void*);
void* repl_realloc(void*, size_t);
int repl_posix_memalign(void**, size_t, size_t);
size_t repl_malloc_usable_size(void*);

// dsm: Used to allocate stacks from app's memory segment
void* app_mem_memalign(size_t alignment, size_t bytes);

// dsm: Untracked memory interface (limited due to limited users; frees are
// automatically routed to the right pool).
void* repl_untracked_malloc(size_t size);
