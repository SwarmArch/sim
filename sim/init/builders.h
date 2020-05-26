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

#include <string>
#include <vector>
#include <unordered_map>
#include "sim/init/config.h"

class Cache;
class Core;
class H3HashFamily;
class LLCCD;
class MemCtrl;
class Network;
class ROB;
class TSB;
class MemoryPartition;

typedef std::vector<std::vector<Cache*>> CacheGroup;

// Functions to build the system
CacheGroup* BuildCacheGroup(const Config& config, const std::string& name,
                            const H3HashFamily* parentHashFn, bool isTerminal,
                            const MemoryPartition* memPart);
void BuildConflictDetectionGroup(const Config& config, const CacheGroup& cGroup,
                                 bool isTerminal, LLCCD*);
MemCtrl* BuildMemoryController(const Config& config, uint32_t lineSize, uint32_t frequency, std::string& name);

void ConnectCDs(const std::vector<ROB*>& robs, const std::vector<TSB*>& tsbs,
                Core** cores);

/*
    Currently StreamPrefetching supports 2 modes:
    L1L2_MULTIPLE
        Each L1 cache has a unique StreamPrefetcher between itself and its L2.
        These StreamPrefetchers will detect strides by observing L1 misses
        l1d.caches == StreamPf.prefetchers
        l1d.parent = "StreamPf"
        StreamPf.parent = "l2"
        StreamPf.banks = 1
    L2L3_MULTIPLE
        Each l2 cache has a unique StreamPrefetcher between itself and the L3.
        These StreamPrefetchers will detect strides by observing L2 misses
        l2.caches == StreamPf.prefetchers
        l2.parent = "StreamPf"
        StreamPf.parent = "l3"
        StreamPf.banks = 1
*/
void ConfigureStreamPrefetchers(const Config& config, 
                                std::unordered_map<std::string, CacheGroup*>& cMap,
                                std::unordered_map<std::string, std::vector<std::string>>& childMap,
                                size_t numCores, Core** cores,
                                const std::vector<ROB*>& robs);

template <class T>
std::vector<T> FlattenGroups(const std::vector<std::vector<T>>& groups) {
    std::vector<T> flattenedGroup;
    for (auto& v : groups)
        for (T t : v) flattenedGroup.push_back(t);
    return flattenedGroup;
}
