/** $lic$
 * Copyright (C) 2012-2021 by Massachusetts Institute of Technology
 * Copyright (C) 2010-2012 by The Board of Trustees of Stanford University
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

/* This file was adapted from zsim. */

#ifndef PREFETCHER_H_
#define PREFETCHER_H_

#include <bitset>
#include "sim/bithacks.h"
#include "sim/memory/memory_hierarchy.h"
#include "sim/memory/cache.h"
#include "sim/stats/stats.h"

/* Prefetcher models: Basic operation is to interpose between cache levels, issue additional accesses,
 * and keep a small table with delays; when the demand access comes, we do it and account for the
 * latency as when it was first fetched (to avoid hit latencies on partial latency overlaps).
 */

template <int32_t M, int32_t T, int32_t I>  // max value, threshold, initial
class SatCounterPF {
    private:
        int32_t count;
    public:
        SatCounterPF() : count(I) {}
        void reset() { count = I; }
        void dec() { count = MAX(count - 1, 0); }
        void inc() { count = MIN(count + 1, M); }
        bool pred() const { return count >= T; }
        uint32_t counter() const { return count; }
};

/* This is basically a souped-up version of the DLP L2 prefetcher in Nehalem: 16 stream buffers,
 * but (a) no up/down distinction, and (b) strided operation based on dominant stride detection
 * to try to subsume as much of the L1 IP/strided prefetcher as possible.
 *
 * FIXME: For now, mostly hardcoded; 64-line entries (4KB w/64-byte lines), fixed granularities, etc.
 * TODO: Adapt to use weave models
 */
class StreamPrefetcher : public Cache {
    public:

        /*
        The supported modes, named "_MULTIPLE" modes, correspond to
        topologies where each L1 or L2 has a unique Stream Prefetcher parent.
        Each prefetcher only has 1 bank, and does not share stream prediction
        data with other prefetchers.

        Different topologies (ie A single, banked prefetcher, serving
        as a parent to all L2's or all L1's of a tile), are not supported.
        This is because there are assumptions built into the Cache/coherence controls
        code that assume there are no prefetchers in between the cache groups.
            - The Cache assumes that the childId's used in `MemReq` are the childId's
            of caches in the case where there are no prefetchers.
            - The Cache also assumes that it will be sending invalidations directly to
            cache children, not prefetchers.
        MULTIPLE mode prefetchers sidestep these issues, since, in those topologies,
        the childId of the stream prefetchers are the childId's of their child cache,
        if there were not stream prefetchers, so, StreamPrefetchers can just send memory
        requests to their parent, using their own childId. 
        For invalidations, since each child cache only has one, unique Stream Prefetcher
        parent, sending an invalidation to a stream prefetcher will behave exactly like
        the parent cache sending an invalidation to their child cache, without the prefetcher
        in between.

        If the cache topology were different, the stream prefetcher would no longer be able
        to use their own childId for memreqs. In addition, depending on the topology, stream prefetecher
        banks may have common children. In this case, invalidates might be sent twice to a 
        child, breaking assertions in Cache/coherence_ctrls
            ie: coherence_ctrls assumes that if a InvReq is received for a line that is already
            invalid, then the InvReq must be used for conflict checking, and so, must be timestamped
            But, if the InvReq was sent twice, on the second receive,the line may already be
            Invalid, but the invalidation might not be ag conflict check invalidation.
        Also, since caches assume that invalidates are being sent directly to cache children,
        Stream Prefetchers in these different topologies will need to know how to forward
        these invalidates so that they are sent to the right child caches.
        */
        enum class Mode {
            UNSET         = 0,
            L1L2_MULTIPLE = 1,
            L2L3_MULTIPLE = 2,
        };

        // Stream detection information, for streams within a page
        // assumes 64 byte cache lines and 4kb pages
        struct Entry {
            // Two competing strides; at most one active
            int32_t stride;
            SatCounterPF<3, 2, 1> conf;
            bool allocated;

            struct AccessTimes {
                uint64_t startCycle;  // FIXME: Dead for now, we should use it for profiling
                uint64_t respCycle;

                void fill(uint32_t s, uint64_t r) { startCycle = s; respCycle = r; }
            };

            // There are 64 cache lines (size 64 byte) in 4kb page
            AccessTimes times[64];
            // bit set to true once corresponding cacheline has been prefetched
            std::bitset<64> valid;

            uint32_t lastPos;
            uint32_t lastLastPos;
            uint32_t lastPrefetchPos;
            uint64_t lastCycle;  // updated on alloc and hit
            uint64_t ts;

            void alloc(uint64_t curCycle) {
                stride = 1;
                lastPos = 0;
                lastLastPos = 0;
                lastPrefetchPos = 0;
                conf.reset();
                valid.reset();
                lastCycle = curCycle;
                allocated = true;
            }
        };
    private:
        uint64_t timestamp;  // for LRU
        const H3HashFamily* hashFn;

        Counter profAccesses, profPrefetches, profDoublePrefetches, profPageHits, profHits, profShortHits, profStrideSwitches, profLowConfAccs;
        Counter profNonCCAccessWithCCPrefetches;

        std::vector<MemPort*> parents;
        std::vector<InvPort*> children;

        // needed for conflict checking (deciding what level to do the check:
        // tile or global)
        Mode mode;
        uint32_t childId;

        // stream detection data
        // 16, fully associative stream buffers
        Address tag[16];
        Entry array[16];

        /*
        Make a prefetch memory request to the parent cache.
        Prefetch was triggered by memory request triggerReq
        */
        uint64_t makePrefetchAccess(MemPort* parent,
                                    const MemReq& triggerReq,
                                    const MemReq& req,
                                    MemResp& resp,
                                    uint64_t reqCycle);
    public:
        explicit StreamPrefetcher(const std::string& _name,
                                  const H3HashFamily* _hashFn)
            : Cache(0, nullptr, nullptr, nullptr, 0, 0, _name),
              timestamp(0),
              hashFn(_hashFn),
              mode(Mode::UNSET) {
            for(int i = 0; i < 16; ++i) {
                array[i].allocated = false;
            }
        }
        void initStats(AggregateStat* parentStat) override;
        void setParents(uint32_t _childId, const std::vector<MemPort*>& _parents) override;
        void setChildren(const std::vector<InvPort*>& _children) override;

        uint64_t access(const MemReq& req, MemResp& resp, uint64_t cycle) override;
        uint64_t access(const InvReq& req, InvResp& resp, uint64_t cycle) override;

        // mode is needed for conflict checking
        // (deciding what level to do the check: tile or global)
        void setMode(Mode _mode) {
            assert(_mode != Mode::UNSET);
            mode = _mode;
        }

        Mode getMode() {
            return mode;
        }
};

bool addrNeedsConflictCheck(Address addr);
bool lineNeedsConflictCheck(Address lineAddr);
/*
If the the triggering access for a prefetch was non-conlict-checked
but the prefetch request needs conflict checking
*/
bool nonCCAccessWithCCPrefetch(const MemReq& triggerReq, const MemReq& req);

#endif  // PREFETCHER_H_
