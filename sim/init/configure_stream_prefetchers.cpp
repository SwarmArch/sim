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

#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "sim/init/builders.h"
#include "sim/log.h"
#include "sim/memory/cache.h"
#include "sim/memory/stream_prefetcher.h"
#include "sim/core/core.h"

using std::string;
using std::unordered_map;
using std::map;
using std::vector;

#undef DEBUG
#define DEBUG(args...) //info(args)

/*
    Loop structure copied from InitSystem, where the caches are connected
    Returns a mapping between a Cache and a vector of its parent Caches
*/
map<const Cache*, vector<Cache*>> mapChildToParentCachePtrs(const Config& config,
                                                            unordered_map<string, CacheGroup*>& cMap,
                                                            unordered_map<string, vector<string>>& childMap) {
    map<const Cache*, vector<Cache*>> childToParentCachePtrs;
    for (const auto& kv : cMap) {
        const char* grp = kv.first.c_str();
        if (childMap.count(grp) == 0) continue;  // skip terminal caches

        CacheGroup& parentCaches = *cMap[grp];
        uint32_t parents = parentCaches.size();
        assert(parents);
        // Concatenation of all child caches
        CacheGroup childCaches;
        for (string child : childMap[grp]) {
            childCaches.insert(childCaches.end(), cMap[child]->begin(),
                               cMap[child]->end());
        }
        uint32_t children = childCaches.size();
        assert(children);
        uint32_t childrenPerParent = children / parents;

        for (uint32_t p = 0; p < parents; p++) {
            vector<Cache*> parentsVec;
            parentsVec.insert(parentsVec.end(), parentCaches[p].begin(),
                              parentCaches[p].end());
            for (uint32_t c = p * childrenPerParent;
                    c < (p + 1) * childrenPerParent; c++) {
                for (Cache* bank : childCaches[c]) {
                    if (childToParentCachePtrs.find(bank) == childToParentCachePtrs.end()) {
                        childToParentCachePtrs[bank] = parentsVec;
                    }
                }
            }
        }
    }
    return childToParentCachePtrs;
}

void ConfigureStreamPrefetchers(const Config& config, 
                                unordered_map<string, CacheGroup*>& cMap,
                                unordered_map<string, vector<string>>& childMap,
                                size_t numCores, Core** cores,
                                const std::vector<ROB*>& robs) {
    
    map<const Cache*, vector<Cache*>> childToParentCachePtrs 
        = mapChildToParentCachePtrs(config, cMap, childMap);

    // Map StreamPrefetchers to their L1/L2 children
    // to determine the "mode" of the prefetcher
    // Mode describes where the prefetchers are located and how
    // many there are in relation to child cache groups
    // More details in builders.h
    std::map<StreamPrefetcher*, std::vector<const Cache*>> spfToL1Children;
    std::map<StreamPrefetcher*, std::vector<Cache*>> spfToL2Children;
    for (size_t _coreIdx = 0; _coreIdx < numCores; ++_coreIdx) {
        Core* core = cores[_coreIdx];
        const Cache* l1 = core->getDataCache();
        std::vector<Cache*> l1Parents = childToParentCachePtrs[l1];
        StreamPrefetcher* firstParent = dynamic_cast<StreamPrefetcher*>(l1Parents[0]);
        bool parentsAreStreamPrefetchers = firstParent != nullptr;
        if (parentsAreStreamPrefetchers) {
            for (auto& par : l1Parents) {
                StreamPrefetcher* spf = dynamic_cast<StreamPrefetcher*>(par);
                assert_msg(spf, "If one L1 Parent is a StreamPrefetcher, all L1 Parents must be stream prefetchers");
                spfToL1Children[spf].push_back(l1);
            }
        }
    }
    for (const vector<Cache*>& l2s : *(cMap["l2"])) {
        for (auto& l2 : l2s) {
            std::vector<Cache*> l2Parents = childToParentCachePtrs[l2];
            StreamPrefetcher* firstParent = dynamic_cast<StreamPrefetcher*>(l2Parents[0]);
            bool parentsAreStreamPrefetchers = firstParent != nullptr;
            if (parentsAreStreamPrefetchers) {
                for (auto& par : l2Parents) {
                    StreamPrefetcher* spf = dynamic_cast<StreamPrefetcher*>(par);
                    assert_msg(spf, "If one L2 Parent is a StreamPrefetcher, all L2 Parents must be StreamPrefetchers");
                    spfToL2Children[spf].push_back(l2);
                }
            }
        }
    }

    for (auto& kv : spfToL1Children) {
        StreamPrefetcher* spf = kv.first;
        std::vector<const Cache*> l1s = kv.second;
        // a StreamPrefetcher btwn l1 and l2 must only have one l2 as its parent
        assert_msg(childToParentCachePtrs[spf].size() == 1,
            "A Stream Prefetcher between L1 and L2 must have only one L2"
            "as its parent.");
        assert_msg(l1s.size() == 1,
            "Stream Prefetcher must only have one L1 child, if any, since the only "
            "supported stream prefetcher configuration between L1 and L2 "
            "is L1L2_MULTIPLE, where each L1 has a unique Stream Prefetcher parent");
        spf->setMode(StreamPrefetcher::Mode::L1L2_MULTIPLE);
        DEBUG("StreamPrefetcher %s mode set to %s", spf->name(), "L1L2_MULTIPLE");
    }

    for (const auto& kv : spfToL2Children) {
        StreamPrefetcher* spf = kv.first;
        std::vector<Cache*> l2s = kv.second;
        assert_msg(l2s.size() == 1,
            "Stream Prefetcher must only have one L2 child, if any, since the only "
            "supported stream prefetcher configuration between L2 and L3 "
            "is L2L3_MULTIPLE, where each L2 has a unique Stream Prefetcher parent");
        spf->setMode(StreamPrefetcher::Mode::L2L3_MULTIPLE);
        DEBUG("StreamPrefetcher %s mode set to %s", spf->name(), "L2L3_MULTIPLE");
    }
}
