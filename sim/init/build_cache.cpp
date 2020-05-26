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

#include <functional>
#include <iostream>
#include <list>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include "sim/init/builders.h"
#include "sim/log.h"
#include "sim/memory/cache.h"
#include "sim/memory/cache_arrays.h"
#include "sim/memory/coherence_ctrls.h"
#include "sim/memory/filter_cache.h"
#include "sim/memory/hash.h"
#include "sim/memory/repl_policies.h"
#include "sim/memory/stream_prefetcher.h"

using std::string;
using std::stringstream;
using std::unordered_map;
using std::list;

#undef DEBUG
#define DEBUG(args...) //info(args)

static Cache* BuildCacheBank(const Config& config, const string& prefix,
                             const string& name, uint32_t bankSize,
                             const H3HashFamily* parentHashFn, bool isTerminal,
                             const MemoryPartition* memoryPartition) {
    string type = config.get<const char*>(prefix + "type", "Simple");

    uint32_t lineSize = ossinfo->lineSize;
    assert(lineSize > 0); //avoid config deps
    if (bankSize % lineSize != 0) panic("%s: Bank size must be a multiple of line size", name.c_str());

    uint32_t numLines = bankSize/lineSize;

    // Cache Array
    uint32_t numHashes = 1;
    uint32_t ways = config.get<uint32_t>(prefix + "array.ways", 4);
    string arrayType = config.get<const char*>(prefix + "array.type", "SetAssoc");
    uint32_t candidates = (arrayType == "Z")? config.get<uint32_t>(prefix + "array.candidates", 16) : ways;

    // Need to know number of hash functions before instantiating array
    if (arrayType == "SetAssoc") {
        numHashes = 1;
    } else if (arrayType == "Z") {
        numHashes = ways;
        assert(ways > 1);
    } else if (arrayType == "IdealLRU" || arrayType == "IdealLRUPart") {
        ways = numLines;
        numHashes = 0;
    } else {
        panic("%s: Invalid array type %s", name.c_str(), arrayType.c_str());
    }

    // Power of two sets check; also compute setBits, will be useful later
    uint32_t numSets = numLines/ways;
    uint32_t setBits = 31 - __builtin_clz(numSets);
    if ((1u << setBits) != numSets) panic("%s: Number of sets must be a power of two (you specified %d sets)", name.c_str(), numSets);

    // Hash function
    HashFamily* hf = nullptr;
    string hashType = config.get<const char*>(prefix + "array.hash", (arrayType == "Z")? "H3" : "None"); //zcaches must be hashed by default
    if (numHashes) {
        if (hashType == "None") {
            if (arrayType == "Z") panic("ZCaches must be hashed!");
            assert(numHashes == 1);
            hf = new IdHashFamily;
        } else if (hashType == "H3") {
            //STL hash function
            size_t seed = std::hash<string>{}(prefix);
            hf = new H3HashFamily(numHashes, setBits, 0xCAC7EAFFA1 + seed /*make randSeed depend on prefix*/);
        } else {
            panic("%s: Invalid value %s on array.hash", name.c_str(), hashType.c_str());
        }
    }

    // Replacement policy
    string replType = config.get<const char*>(prefix + "repl.type", (arrayType == "IdealLRUPart")? "IdealLRUPart" : "LRU");
    ReplPolicy* rp = nullptr;
    string nuca = config.get<const char*>(prefix + "nuca.type", "None");

    if (replType == "LRU" || replType == "LRUNoSh") {
        bool sharersAware = (replType == "LRU") && !isTerminal;
        if (sharersAware) {
            rp = new LRUReplPolicy<true>(numLines);
        } else {
            rp = new LRUReplPolicy<false>(numLines);
        }
    } else if (replType == "LFU") {
        rp = new LFUReplPolicy(numLines);
    } else if (replType == "TreeLRU") {
        rp = new TreeLRUReplPolicy(numLines, candidates);
    } else if (replType == "NRU") {
        rp = new NRUReplPolicy(numLines, candidates);
    } else if (replType == "Rand") {
        rp = new RandReplPolicy(candidates);
    } else {
        panic("%s: Invalid replacement type %s", name.c_str(), replType.c_str());
    }
    assert(rp);

    // Alright, build the array
    CacheArray* array = nullptr;
    if (arrayType == "SetAssoc") {
        array = new SetAssocArray(numLines, ways, rp, hf);
    } else if (arrayType == "Z") {
        array = new ZArray(numLines, ways, candidates, rp, hf);
    } else {
        panic("This should not happen, we already checked for it!"); //unless someone changed arrayStr...
    }

    // Latency
    uint32_t latency = config.get<uint32_t>(prefix + "latency", 10);
    uint32_t accLat = (isTerminal)? 0 : latency; //terminal caches has no access latency b/c it is assumed accLat is hidden by the pipeline
    uint32_t invLat = latency;

    // Finally, build the cache
    Cache* cache;
    CC* cc = NULL;

    if (isTerminal) {
        cc = new MESITerminalCC(numLines, name, parentHashFn, memoryPartition);
    } else {
        bool isDir = config.get<bool>(prefix + "isDir", false);
        assert(!isDir);
        assert(type == "Simple" || !isDir);
        assert_msg(!(type == "NonInclusive"
                    || type == "DRAM"
                    || type == "Alloy"
                    || type == "MissMap"
                    || type == "IdealLO"),
                "No support for type %s. FakeCC has been removed.", type.c_str());
        cc = new MESICC(numLines,
                        false /*FIXME: nonInclusiveHack is deprecated*/,
                        name, parentHashFn, memoryPartition);
    }
    rp->setCC(cc);


    if (!isTerminal) {
        if (type == "Simple") {
            cache = new Cache(numLines, cc, array, rp, accLat, invLat, name);
        } else {
            panic("Invalid cache type %s", type.c_str());
        }
    } else {
        //Filter cache optimization
        if (type != "Simple") panic("Terminal cache %s can only have type == Simple", name.c_str());
        if (arrayType != "SetAssoc" || hashType != "None" || replType != "LRU") panic("Invalid FilterCache config %s", name.c_str());
        uint32_t lineBits = ilog2(ossinfo->lineSize);
        cache = new FilterCache(lineBits, numSets, numLines, cc, array, rp, accLat, invLat, name);
    }

    return cache;
}

CacheGroup* BuildCacheGroup(const Config& config, const string& name,
                            const H3HashFamily* parentHashFn, bool isTerminal,
                            const MemoryPartition* memoryPartition) {
    CacheGroup* cgp = new CacheGroup;
    CacheGroup& cg = *cgp;

    string prefix = "sys.caches." + name + ".";

    bool isPrefetcher = config.get<bool>(prefix + "isPrefetcher", false);
    if (isPrefetcher) { //build a prefetcher group
        uint32_t prefetchers = config.get<uint32_t>(prefix + "prefetchers", 1);
        uint32_t banks = config.get<uint32_t>(prefix + "banks", 1);
        cg.resize(prefetchers);
        assert_msg(banks == 1, "Stream Prefetcher, %s should only have 1 bank", name.c_str());
        for (vector<Cache*>& bg : cg) bg.resize(banks);
        for (uint32_t i = 0; i < prefetchers; i++) {
            stringstream ss;
            ss << name << "-" << i;
            string pfName(ss.str().c_str());
            cg[i][0] = new StreamPrefetcher(pfName, parentHashFn);
        }
        return cgp;
    }

    uint32_t size = config.get<uint32_t>(prefix + "size", 64*1024);
    uint32_t banks = config.get<uint32_t>(prefix + "banks", 1);
    uint32_t caches = config.get<uint32_t>(prefix + "caches", 1);

    uint32_t bankSize = size/banks;
    if (size % banks != 0) {
        panic("%s: banks (%d) does not divide the size (%d bytes)", name.c_str(), banks, size);
    }

    uint32_t extraBanks = config.get<uint32_t>(prefix + "extraBanks.banks", 0);
    uint32_t extraBankSize = 0;
    uint32_t totalBanks = banks + extraBanks;

    // TODO I don't think we need this...
    if (extraBanks) {
        uint32_t totalExtraSize = config.get<uint32_t>(prefix + "extraBanks.size", 0);
        assert(totalExtraSize > 0);
        assert(totalExtraSize % extraBanks == 0);
        extraBankSize = totalExtraSize / extraBanks;

        auto buckets = config.get<uint32_t>(prefix + "repl.buckets", -1);
        if (buckets != (uint32_t)-1) {
            uint32_t bytesPerBucket = bankSize / buckets;

            auto bucketsExtra = config.get<uint32_t>(prefix + "extraBanks.repl.buckets");
            uint32_t bytesPerBucketExtra = extraBankSize / bucketsExtra;

            assert(bytesPerBucket == bytesPerBucketExtra);
        }
    }

    cg.resize(caches);
    for (vector<Cache*>& bg : cg) bg.resize(banks + extraBanks);

    for (uint32_t i = 0; i < caches; i++) {
        stringstream ss;
        ss << name << "-" << i;
        string cacheName(ss.str().c_str());

        for (uint32_t j = 0; j < totalBanks; j++) {
            stringstream ss;
            ss << name << "-" << i;
            if (totalBanks > 1) {
                ss << "b" << j;
            }
            string bankName(ss.str().c_str());
            if (j < banks) {
                cg[i][j] = BuildCacheBank(config, prefix, bankName, bankSize,
                        parentHashFn, isTerminal, memoryPartition);
            } else {
                string dramPrefix = prefix + "extraBanks.";
                cg[i][j] = BuildCacheBank(config, dramPrefix, bankName,
                        extraBankSize, parentHashFn, isTerminal, memoryPartition);
            }
        }
    }
    return cgp;
}
