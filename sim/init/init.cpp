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

#include "sim/init/init.h"
#include <algorithm>
#include <boost/dynamic_bitset.hpp>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <list>
#include <malloc.h>
#include <regex>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>
#include <cstdlib>
#include "sim/sim.h"
#include "sim/alloc/alloc.h"
#include "sim/init/config.h"
#include "sim/init/builders.h"
#include "sim/conflicts/abort_handler.h"
#include "sim/conflicts/llc_cd.h"
#include "sim/conflicts/stallers.h"
#include "sim/core/core.h"
#include "sim/core/sbcore.h"
#include "sim/core/ooo_core.h"
#include "sim/domainprofiler.h"
#include "sim/event.h"
#include "sim/ff_queue.h"
#include "sim/gvt_arbiter.h"
#include "sim/load.h"
#include "sim/log.h"
#include "sim/memory/filter_cache.h"
#include "sim/memory/mem_ctrls.h"
#include "sim/memory/memory_partition.h"
#include "sim/network.h"
#include "sim/stack.h"
#include "sim/stats/clock_stat.h"
#include "sim/stats/stats.h"
#include "sim/task_balancer.h"
#include "sim/task_mapper.h"
#include "sim/tsarray.h"
#include "sim/tsb.h"
#include "sim/taskloadprofiler.h"
#include "sim/taskprofilingsummary.h"
#include "sim/thread_throttler.h"
#include "sim/watchdog.h"
#include "sim/memory/stream_prefetcher.h"

using std::string;
using std::stringstream;
using std::unordered_map;
using std::list;
using std::pair;

/* TRANSITIONAL HACK: Function to get non-sticky sharers of a given line */
static std::function<std::bitset<MAX_CACHE_CHILDREN>(Address, bool)> hackGetLLCSharersFn;
std::bitset<MAX_CACHE_CHILDREN> HackGetLLCSharers(Address lineAddr, bool checkReaders) { return hackGetLLCSharersFn(lineAddr, checkReaders); }
// FIXME(dsm): Remove as soon as we extricate sticky directories from abort handler (ASAP!)


#undef DEBUG
#define DEBUG(args...) //info(args)

#ifdef ATOMIC_GVT_UPDATES  // see gvt_arbiter.h
void updateAllRobGvts(const GVTUpdateMsg& msg, uint64_t cycle) {
    for (ROB* rob : ossinfo->robs) rob->updateGVT(msg, cycle);
}
#endif

bool builtTSArray = false;

// Non-const version of ossinfo; only used here to initialize it
static GlobSimInfo* initinfo = new GlobSimInfo();
LoadMetricConfig loadMetricConfig;

/* Stats */

// Call before any other components are initialized
static void PreInitStats() {
    initinfo->rootStat = new AggregateStat();
    initinfo->rootStat->init("root", "Stats");
    initinfo->tmStat = new AggregateStat();
    initinfo->tmStat->init("task-mapper", "Stats");
}

// Call after the rest of the system has been initialized
static void PostInitStats(const Config& config) {
    initinfo->rootStat->makeImmutable();
    initinfo->tmStat->makeImmutable();

    string pathStr = initinfo->outputDir + "/";

    // Absolute paths for stats files
    const char* pStatsFile   = strdup((pathStr + "sim.h5").c_str());
    const char* tmStatsFile  = strdup((pathStr + "sim-tm.h5").c_str());
    const char* cmpStatsFile = strdup((pathStr + "sim-cmp.h5").c_str());
    const char* statsFile    = strdup((pathStr + "sim.out").c_str());

    StatsBackend* periodicStatsBackend = new HDF5Backend(
        pStatsFile, initinfo->rootStat, (1 << 20) /* 1MB */, false, false);
    periodicStatsBackend->dump(true);  // must have a first sample
    initinfo->statsBackends.push_back(periodicStatsBackend);

    StatsBackend* taskMapperStatsBackend = new HDF5Backend(
        tmStatsFile, initinfo->tmStat, (1 << 20) /* 1MB */, false, false);
    taskMapperStatsBackend->dump(true);  // must have a first sample
    initinfo->statsBackends.push_back(taskMapperStatsBackend);

    uint64_t periodicStatsInterval =
        config.get<uint64_t>("sim.periodicStatsInterval", 0);
    if (periodicStatsInterval) {
        auto periodicDumpLambda = [=](uint64_t cycle) {
            DEBUG("Dumping stats @ %ld", getCurCycle());
            periodicStatsBackend->dump(true /*buffered*/);
        };
        schedPeriodicEvent(periodicStatsInterval, periodicStatsInterval,
                           periodicDumpLambda);
    }

    uint64_t taskMapperStatsInterval =
        config.get<uint64_t>("sim.taskMapperStatsInterval", 0);
    if (taskMapperStatsInterval) {
        auto taskMapperDumpLambda = [=](uint64_t cycle) {
            ossinfo->taskMapper->updateStats();
            taskMapperStatsBackend->dump(true /*buffered*/);
        };
        schedPeriodicEvent(taskMapperStatsInterval, taskMapperStatsInterval,
                           taskMapperDumpLambda);
    }

    uint32_t cycleTimeout = config.get<uint32_t>("sim.cycleTimeout", 60);
    assert(cycleTimeout >= 10U);
    uint32_t taskTimeout = config.get<uint32_t>("sim.taskTimeout", 0);
    // For backwards compatibility, support the old name of this config.
    if (!taskTimeout) taskTimeout = config.get<uint32_t>("sim.deadlockTimeout", 120);
    assert(taskTimeout >= cycleTimeout);
    uint32_t gvtTimeout =
        config.get<uint32_t>("sim.gvtTimeout", std::max(600U, taskTimeout));
    assert(gvtTimeout >= cycleTimeout);
    uint32_t ffTimeout = config.get<uint32_t>("sim.ffTimeout", 24U * 60U * 60U);
    InitWatchdog(cycleTimeout, taskTimeout, gvtTimeout, ffTimeout);
    TickWatchdog();
    PIN_SpawnInternalThread(WatchdogThread, nullptr, 64*1024, nullptr);

    // Convenience stats
    StatsBackend* compactStats =
        new HDF5Backend(cmpStatsFile, initinfo->rootStat,
                        0 /* no aggregation, this is just 1 record */, false,
                        true);  // don't dump a first sample
    StatsBackend* textStats = new TextBackend(statsFile, initinfo->rootStat);
    initinfo->statsBackends.push_back(compactStats);
    initinfo->statsBackends.push_back(textStats);
}

static void InitGlobalStats() {
    auto clockStat = new ClockStat();
    clockStat->init("time", "Simulation time (ns)");
    clockStat->start();
    auto cyclesLambda = [&]() { return getCurCycle(); };
    auto cyclesStat = makeLambdaStat(cyclesLambda);
    cyclesStat->init("cycles", "Simulated cycles");
    auto heartbeatsLambda = [&]() { return getHeartbeats(); };
    auto heartbeatsStat = makeLambdaStat(heartbeatsLambda);
    heartbeatsStat->init("heartbeats", "Application heartbeats");
    auto mallocCommitLambda = [&]() { return getSimMallocCommits(); };
    auto mallocCommitStat = makeLambdaStat(mallocCommitLambda);
    mallocCommitStat->init("mallocCommit", "malloc() calls that commit");
    auto mallocAbortLambda = [&]() { return getSimMallocAborts(); };
    auto mallocAbortStat = makeLambdaStat(mallocAbortLambda);
    mallocAbortStat->init("mallocAbort", "malloc() calls by tasks that abort");
    auto freeCommitLambda = [&]() { return getSimFreeCommits(); };
    auto freeCommitStat = makeLambdaStat(freeCommitLambda);
    freeCommitStat->init("freeCommit", "free() calls that commit");
    auto freeAbortLambda = [&]() { return getSimFreeAborts(); };
    auto freeAbortStat = makeLambdaStat(freeAbortLambda);
    freeAbortStat->init("freeAbort", "free() calls by tasks that abort");
    initinfo->rootStat->append(clockStat);
    initinfo->rootStat->append(cyclesStat);
    initinfo->rootStat->append(heartbeatsStat);
    initinfo->rootStat->append(mallocCommitStat);
    initinfo->rootStat->append(mallocAbortStat);
    initinfo->rootStat->append(freeCommitStat);
    initinfo->rootStat->append(freeAbortStat);
    initinfo->tmStat->append(cyclesStat);
}

static void BuildTimeStampArray(const Config& config, const string& name) {
    string prefix = "sys.caches." + name + ".";

    uint32_t size = config.get<uint32_t>(prefix + "size", 64 * 1024);
    uint32_t caches = config.get<uint32_t>(prefix + "caches", 1);
    uint32_t banks = config.get<uint32_t>(prefix + "banks", 1);
    uint32_t groupSize = config.get<uint32_t>(prefix + "linesPerCanary", 1);

    uint32_t lineSize = initinfo->lineSize;
    uint32_t numLines = size / lineSize;

    // FIXME Does not work for general cache hierarchy. Here we assert # l2
    // caches == # ROBs
    if (banks != 1) panic("L2 cache banks must be 1 (currently %u)", banks);
    if (caches != initinfo->numROBs)
        panic("L2 caches (%u) != ROBs (%lu)", caches, initinfo->numROBs);

    // # TSArray = # L2 caches
    initinfo->tsarray = static_cast<TSArray**>(
        memalign(CACHE_LINE_BYTES, caches * sizeof(TSArray*)));
    DEBUG("Building %u tsarrays (numLines=%u, groupSize=%u)", caches, numLines,
          groupSize);

    for (uint32_t i = 0; i < caches; ++i) {
        string name = "tsarray-" + Str(i);
        initinfo->tsarray[i] = new TSArray(name, numLines, groupSize);
    }
}

template <typename O, typename T>
static vector<O> UpcastVector(const vector<T>& in) {
    vector<O> out;
    for (T elem : in) out.push_back(elem);
    return out;
}

static void InitSystem(const Config& config) {
    // child -> parent
    unordered_map<string, string> parentMap;  // child -> parent
    // parent -> children (a parent may have multiple children, they are
    // ordered by appearance in the file)
    unordered_map<string, vector<string>> childMap;

    // child -> parent hash functions, shared across caches in the level
    unordered_map<string, const H3HashFamily*> parentHashMap;

    // Create memory partitioner
    const bool partitionMemory =
        config.get<bool>("sys.partitionMemory", false);
    if (partitionMemory)
        initinfo->memoryPartition = new MemoryPartition(initinfo->lineBits);
    else
        initinfo->memoryPartition = nullptr;

    // Build the network (for now we have a single net, but can easily extend
    // to multiple). The default single-node network adds no delays.
    uint32_t netNodes = config.get<uint32_t>("sys.net.nodes", 1);
    // This default produces minimum-distance tilings with x >= y. For
    // example, with 14 nodes this will use a 4x4 mesh instead of a 7x2 one.
    // *Note that these are not minimum-area*, but they keep network latency
    // monotonic with system size to avoid strange behavior.
    uint32_t defaultxdim = std::ceil(std::sqrt(1. * netNodes));
    uint32_t xdim = config.get<uint32_t>("sys.net.xdim", defaultxdim);

    Network* net = new Network("net", netNodes, xdim,
            config.get<uint32_t>("sys.net.routerDelay", 1),
            config.get<uint32_t>("sys.net.linkDelay", 1),
            config.get<uint32_t>("sys.net.expressLinkHops", 0),  // 0 disables
            config.get<uint32_t>("sys.net.expressLinkDelay", 2),
            config.get<uint32_t>("sys.net.linkBytes", 16),
            config.get<bool>("sys.net.straightFastpath", true),
            config.get<uint32_t>("sys.net.subnets", 1),
            config.get<bool>("sys.net.contention", true));

    // Build the caches
    vector<const char*> cacheGroupNames;
    config.subgroups("sys.caches", cacheGroupNames);
    string prefix = "sys.caches.";

    for (const char* grp : cacheGroupNames) {
        string group(grp);
        if (group == "mem") panic("'mem' is an invalid cache group name");
        if (parentMap.count(group))
            panic("Duplicate cache group %s", (prefix + group).c_str());
        string parent = config.get<const char*>(prefix + group + ".parent");
        parentMap[group] = parent;
        if (!childMap.count(parent)) childMap[parent] = vector<string>();
        childMap[parent].push_back(group);

        // Parent hash function (make randSeed depend on parent's name)
        size_t seed = std::hash<std::string>{}(parent);
        parentHashMap[group] = new H3HashFamily(1, 32, 0xF00BA7 + seed);
    }

    // Check that all parents are valid: Either another cache, or "mem"
    for (const char* grp : cacheGroupNames) {
        string group(grp);
        string parent = parentMap[group];
        if (parent != "mem" && !parentMap.count(parent))
            panic("%s has invalid parent %s", (prefix + group).c_str(),
                  parent.c_str());
    }

    // Get the (single) LLC
    if (!childMap.count("mem"))
        panic("One cache must have mem as parent, none found");
    if (childMap["mem"].size() != 1)
        panic("One cache must have mem as parent, multiple found");
    string llc = childMap["mem"][0];

    // Create sticky directory for LLC TODO For now, specific to Swarm
    LLCCD* llcCD = new LLCCD();

    // Build each of the groups, starting with the LLC
    unordered_map<string, CacheGroup*> cMap;
    list<string> fringe;  // FIFO
    fringe.push_back(llc);
    while (!fringe.empty()) {
        string group = fringe.front();
        fringe.pop_front();

        // FIXME(mcj) if the TSArray can only be built once, why keep it in the
        // loop?
        if (std::regex_match(group,
                             std::regex("(.*)(l2)"))) {  // is an L2 cache
            if (::builtTSArray)
                panic("Already built TSArray. Unable to build again for %s",
                      group.c_str());
            DEBUG("Building TSArray for %s", group.c_str());
            BuildTimeStampArray(config, group);
            ::builtTSArray = true;
        }

        bool isTerminal =
            (childMap.count(group) == 0);  // if no children, connected to cores
        if (cMap.count(group))
            panic("The cache 'tree' has a loop at %s", group.c_str());

        MemoryPartition* memoryPartition = nullptr;
        if ((group == "l2") || (group == "llc") || (group == "l3"))
             memoryPartition = initinfo->memoryPartition;
        cMap[group] = BuildCacheGroup(config, group, parentHashMap[group],
                                      isTerminal, memoryPartition);
        if (!isTerminal)
            for (string child : childMap[group]) fringe.push_back(child);

        string cgNodes = config.get<const char*>("sys.caches." + group + ".nodes", "");
        net->addEndpoints(group, FlattenGroups(*cMap[group]), cgNodes);

        // Don't include prefetcher in conflict detection
        bool isPrefetcher = config.get<bool>("sys.caches." + group + ".isPrefetcher", false);
        if (!isPrefetcher) {
            BuildConflictDetectionGroup(config, *cMap[group],
                    isTerminal,
                    (group == llc) ? llcCD : nullptr);
        }
    }
    // Check single LLC
    if (cMap[llc]->size() != 1)
        panic(
            "Last-level cache %s must have caches = 1, but %ld were specified",
            llc.c_str(), cMap[llc]->size());

    /* Since we have checked for no loops, parent is mandatory, and all parents
     * are checked valid,
     * it follows that we have a fully connected tree finishing at the LLC.
     */

    // Build the memory controllers
    uint32_t memControllers = config.get<uint32_t>("sys.mem.controllers", 1);
    assert(memControllers > 0);

    vector<MemCtrl*> mems;
    mems.resize(memControllers);

    for (uint32_t i = 0; i < memControllers; i++) {
        stringstream ss;
        ss << "mem-" << i;
        string name(ss.str().c_str());
        mems[i] = BuildMemoryController(config, initinfo->lineSize,
                                        initinfo->freqMHz, name);
    }
    string memNodes = config.get<const char*>("sys.mem.nodes", "edges");
    net->addEndpoints("mem", mems, memNodes);

    if (memControllers > 1) {
        bool splitAddrs = config.get<bool>("sys.mem.splitAddrs", false);
        if (splitAddrs) {
            MemCtrl* splitter = new SplitAddrMemory(mems, "mem-splitter");
            mems.resize(1);
            mems[0] = splitter;
        }
    }

    // find the cache group that is child of llc
    assert(childMap[llc].size() == 1);
    string cacheGroupChildOfLLC = childMap[llc][0];

    // Transitional hack (REMOVE ASAP)
    hackGetLLCSharersFn = [llcBanks = (*cMap[llc])[0], parentHash = parentHashMap[cacheGroupChildOfLLC]](Address lineAddr, bool checkReaders) {
        uint32_t parent = getCacheBankId(lineAddr, initinfo->memoryPartition,
                                         parentHash, llcBanks.size());

        return llcBanks[parent]->hackGetSharers(lineAddr, checkReaders);
    };

    // Connect everything

    // mem to llc is a bit special, only one llc
    uint32_t childId = 0;
    for (Cache* llcBank : (*cMap[llc])[0]) {
        auto ports = net->getAtomicPorts<MemPort>(llcBank, mems);
        llcBank->setParents(childId++, ports);
    }

    // Rest of caches
    for (const char* grp : cacheGroupNames) {
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
        if (children % parents != 0) {
            panic(
                "%s has %d caches and %d children, they are non-divisible. "
                "Use multiple groups for non-homogeneous children per parent!",
                grp, parents, children);
        }

        // HACK FIXME: This solves the L1I+D-L2 connection bug, but it's not
        // very clear.
        // A long-term solution is to specify whether the children should be
        // interleaved or concatenated.
        bool terminalChildren = true;
        for (string child : childMap[grp])
            terminalChildren &=
                (childMap.count(child) == 0 ||
                 config.get<bool>("sys.caches." + child + ".isPrefetcher",
                                  false));
        // dsm: Don't interleave. We have no icaches. This fixes nothing now.
        /*if (terminalChildren) {
            info("%s's children are all terminal OR PREFETCHERS, interleaving
        them", grp);
            CacheGroup tmp(childCaches);
            uint32_t stride = children/childrenPerParent;
            for (uint32_t i = 0; i < children; i++) childCaches[i] = tmp[(i %
        childrenPerParent)*stride + i/childrenPerParent];
        }*/

        /* Connect each cache and cache bank in the group with its children
         * - If bankToBankChildren == false (DEFAULT), connect (1) each cache to
         * its parent cache in a tree fashion, and (2)
         *   EXAMPLE: with l2 = { caches = 8; parent = "l3";}; and l3 = { caches
         * = 2; banks = 5; };,
         *      l2-0...3 have parent cache l3-0, and are connected to the 5
         * banks of l3-0 (l3-0b0...4)
         *      l2-4...7 have parent cache l3-1, and are connected to all its
         * banks
         *
         * - If bankToBankChildren == true, caches are connected bank-by-bank
         *   EXAMPLE: with l3 = { caches = 1; banks = 64; parent = "l4"; }; and
         * l4 = { caches = 1; banks = 64; bankToBankChildren = true; }
         *     l3-0b0 has parent l4-0b0, l3-0b1 has parent l4-0b1, etc.
         *  For now, bankToBank requires matching banks at parent and child.
         * Useful beyond coherence (e.g., S-NUCA)
         */
        bool bankToBankChildren = config.get<bool>(
            "sys.caches." + string(grp) + ".bankToBankChildren", false);
        for (uint32_t p = 0; p < parents; p++) {
            vector<Cache*> parentsVec;
            parentsVec.insert(parentsVec.end(), parentCaches[p].begin(),
                              parentCaches[p].end());

            if (bankToBankChildren) {
                // Connect bank to bank
                assert(!terminalChildren);  // untested, shouldn't want this...
                vector<Cache*> childrenVec;
                for (uint32_t c = p * childrenPerParent; c < (p + 1) * childrenPerParent; c++) {
                    for (Cache* bank : childCaches[c]) {
                        childrenVec.push_back(bank);
                    }
                }
                info("Connecting %s's children (%ld) bank to bank", grp, childrenVec.size());
                if (parentsVec.size() != childrenVec.size()) {
                    panic("Bank to bank mode needs matching number of banks");
                }
                for (uint32_t b = 0; b < parentsVec.size(); b++) {
                    vector<Cache*> pv = {parentsVec[b]};
                    vector<Cache*> cv = {childrenVec[b]};
                    auto parents = net->getAtomicPorts<MemPort>(childrenVec[b], pv);
                    auto children = net->getAtomicPorts<InvPort>(parentCaches[p][b], cv);
                    childrenVec[b]->setParents(0 /*single child*/, parents);
                    parentCaches[p][b]->setChildren(children);
                }
                // FIXME(mcj)
                panic("Conflict Resolution wiring assumes no bank-to-bank-children");
            } else {
                // Connect cache to cache
                uint32_t childId = 0;
                vector<Cache*> childrenVec;
                for (uint32_t c = p * childrenPerParent;
                     c < (p + 1) * childrenPerParent; c++) {
                    for (Cache* bank : childCaches[c]) {
                        auto parents = net->getAtomicPorts<MemPort>(bank, parentsVec);
                        bank->setParents(childId++, parents);
                        childrenVec.push_back(bank);
                    }
                }

                for (Cache* bank : parentCaches[p]) {
                    auto children = net->getAtomicPorts<InvPort>(bank, childrenVec);
                    bank->setChildren(children);
                }
            }
        }
    }

    // Check that all the terminal caches have a single bank
    for (const char* grp : cacheGroupNames) {
        if (childMap.count(grp) == 0) {
            uint32_t banks = (*cMap[grp])[0].size();
            if (banks != 1)
                panic(
                    "Terminal cache group %s needs to have a single bank, has "
                    "%d",
                    grp, banks);
        }
    }

    // Tracks how many terminal caches have been allocated to cores
    unordered_map<string, uint32_t> assignedCaches;
    for (const char* grp : cacheGroupNames)
        if (childMap.count(grp) == 0) assignedCaches[grp] = 0;

    // Initialize the abort handler(s) and abort event unit
    uint32_t bloomQueryLatency =
        config.get<uint32_t>("sys.robs.abortHandler.bloomQueryLatency", 3);
    uint32_t localRobAccessDelay =
        std::ceil(std::sqrt(initinfo->numCores / initinfo->numROBs));

    uint32_t robTiledAvgLatency;  // Avg latency from RoB --> RoB assuming RoBs
                                  // are arranged in a mesh
    uint32_t k = std::ceil(std::sqrt(initinfo->numROBs));  // kxk mesh
    if (k % 2 == 0)
        robTiledAvgLatency = std::ceil((float)2.0 * k / 3.0);
    else
        robTiledAvgLatency = std::ceil((float)2.0 * (k / 3.0 - 1 / (3.0 * k)));
    // dsm: Above is in hops --- assuming 4 cycles/hop + 2x for ACK
    //robTiledAvgLatency *= 4 * 2;
    // [mcj] Multiply by the cycle delay under no contention.
    // Do not double for ACKs, because ACK latencies are considered in the
    // AbortHandler.
    robTiledAvgLatency *= (
            config.get<uint32_t>("sys.net.routerDelay", 1) +
            config.get<uint32_t>("sys.net.linkDelay", 1));


    DEBUG("Latencies: %d %d %d", localRobAccessDelay, bloomQueryLatency,
          robTiledAvgLatency);

    initinfo->selectiveAborts =
        config.get<bool>("sys.robs.abortHandler.selectiveAborts", true);

    string stallPolicy = config.get<const char*>("sys.robs.abortHandler.stallPolicy", "Never");
    std::vector<Staller*> stallers;
    bool singleStaller = stallPolicy != "Adaptive";
    if (singleStaller) {
        Staller* staller;
        if (stallPolicy == "Never") staller = new NeverStall();
        else if (stallPolicy == "Always") staller = new AlwaysStall();
        else if (stallPolicy == "Running") staller = new StallIfRunning();
        else if (stallPolicy == "AdaptiveSingle") staller = new AdaptiveStaller();
        else panic("Invalid stallPolicy %s", stallPolicy.c_str());
        stallers.resize(initinfo->numROBs, staller);
    } else {
        assert(stallPolicy == "Adaptive");
        stallers.resize(initinfo->numROBs);
        for (Staller*& s : stallers) s = new AdaptiveStaller();
    }

    initinfo->abortHandler =
        new AbortHandler(initinfo->numROBs, bloomQueryLatency,
                         robTiledAvgLatency, localRobAccessDelay, stallers);
    initinfo->abortHandler->setLLCCD(llcCD);

    initinfo->oneIrrevocableThread =
        config.get<bool>("sys.robs.oneIrrevocableThread", true);
    // oneIrrevocableThread makes no sense with multiple threads.  Ignore it.
    if (initinfo->numCores * initinfo->numThreadsPerCore != 1)
        initinfo->oneIrrevocableThread = false;

    // Task mapper cfg
    string taskMapperType =
        config.get<const char*>("sys.robs.taskMapper.type", "Random");
    const uint32_t samplingInterval = config.get<uint32_t>(
        "sys.robs.taskMapper.samplingInterval", 10000);
    const uint32_t phaseEventWindow = config.get<uint32_t>(
        "sys.robs.taskMapper.phaseEventWindow", initinfo->numROBs * 64);
    string taskMapperPhaseEvent =
        config.get<const char*>("sys.robs.taskMapper.phaseEvent", "Commit");
    string loadMetricString = config.get<const char*>(
        "sys.robs.taskMapper.loadMetric", "CommitCycles");
    TaskMapper::PhaseEvent tmEvent = TaskMapper::PhaseEvent::CYCLE;
    LoadArray::LoadMetric loadMetric;
    if (taskMapperPhaseEvent == "Commit")
        tmEvent = TaskMapper::PhaseEvent::COMMIT;
    else if (taskMapperPhaseEvent == "Dequeue")
        tmEvent = TaskMapper::PhaseEvent::DEQUEUE;
    else if (taskMapperPhaseEvent == "Cycle")
        tmEvent = TaskMapper::PhaseEvent::CYCLE;
    else
        panic("Invalid task mapper phaseEvent %s",
              taskMapperPhaseEvent.c_str());

    if (loadMetricString == "CommitCycles")
        loadMetric = LoadArray::LoadMetric::COMMIT_CYCLES;
    else if (loadMetricString == "CommitInstrs")
        loadMetric = LoadArray::LoadMetric::COMMIT_INSTRS;
    else if (loadMetricString == "NumElements")
        loadMetric = LoadArray::LoadMetric::NUM_ELEMENTS;
    else
        panic("Invalid load metric %s", loadMetricString.c_str());

    if (loadMetricString == "CommitCycles")
        loadMetricConfig.metric = LoadMetricConfig::LoadMetric::CYCLES;
    else if (loadMetricString == "CommitInstrs")
        loadMetricConfig.metric = LoadMetricConfig::LoadMetric::INSTRS;
    else
        loadMetricConfig.metric = LoadMetricConfig::LoadMetric::INSTRS;

    if (partitionMemory) {
        assert_msg(matchAny(taskMapperType, "Random", "Hint"),
                   "partitionMemory is supported with Random, Hint task "
                   "mappers only.");
    }

    bool requireUpdate = false;
    if (taskMapperType == "Random") {
        initinfo->taskMapper = new RandomTaskMapper();
    } else if (taskMapperType == "Hint") {
        initinfo->taskMapper = new HintTaskMapper(initinfo->memoryPartition);
    } else if (taskMapperType == "Blind") {
        requireUpdate = true;
        initinfo->taskMapper =
            new BlindRedistributionTaskMapper();
    } else if (taskMapperType == "LeastLoaded") {
        requireUpdate = true;
        initinfo->taskMapper = new LeastLoadedTaskMapper(loadMetric);
    } else if (taskMapperType == "HintPow2") {
        requireUpdate = true;
        initinfo->taskMapper = new HintPow2TaskMapper(loadMetric);
    } else if ((taskMapperType == "ProportionalBucketTaskMapper") ||
               (taskMapperType == "DeadBeatBucketTaskMapper")) {
        requireUpdate = true;
        const uint32_t bucketResolution =
            config.get<uint32_t>("sys.robs.taskMapper.bucketResolution", 1024);
        const uint32_t bucketsPerRob = config.get<uint32_t>(
            "sys.robs.taskMapper.bucketsPerRob",
            std::max(16u, static_cast<uint32_t>(bucketResolution /
                                                initinfo->numROBs)));
        const uint32_t guard =
            config.get<uint32_t>("sys.robs.taskMapper.guard", 2);
        const uint32_t leash = config.get<uint32_t>(
            "sys.robs.taskMapper.leash",
            initinfo->numROBs);  // Tiling guaranteed to be <= intinfo->numROBs;
                                 // default ~ INF
        const bool donateAscending =
            config.get<bool>("sys.robs.taskMapper.donateAscending", false);
        const bool donateAll =
            config.get<bool>("sys.robs.taskMapper.donateAll", false);
        const bool reconfigure =
            config.get<bool>("sys.robs.taskMapper.reconfigure", true);
        const bool includeDonated =
            config.get<bool>("sys.robs.taskMapper.includeDonated", false);
        const string uPolicyType =
            config.get<const char*>("sys.robs.taskMapper.uPolicy.type", "MaxAbsDev");
        const uint32_t window =
            config.get<uint32_t>("sys.robs.taskMapper.window", 1);
        UniformityPolicy* uPolicy;
        if (uPolicyType == "Chisquare") {
            uPolicy = new ChiSquare();
        } else if (uPolicyType == "MaxAbsDev") {
            const double uPolicyThreshold = config.get<double>(
                "sys.robs.taskMapper.uPolicy.threshold", 0.15);
            uPolicy = new MaximumAbsoluteDeviation(uPolicyThreshold);
        } else
            panic("Invalid uniformity policy %s", uPolicyType.c_str());

        if (taskMapperType == "DeadBeatBucketTaskMapper") {
            initinfo->taskMapper = new DeadBeatBucketTaskMapper(
                tmEvent, phaseEventWindow, initinfo->numROBs, bucketsPerRob,
                guard, leash, window, donateAscending, donateAll,
                includeDonated, reconfigure, loadMetric, uPolicy, *net);
        } else {
            double Kp = double(config.get<uint32_t>("sys.robs.taskMapper.kp", 80)) / 100.0;
            bool adjustPhaseEventWindow = config.get<bool>(
                "sys.robs.taskMapper.adjustPhaseEventWindow", true);
            uint32_t adjustedPhaseEventWindow = phaseEventWindow;
            if (adjustPhaseEventWindow) adjustedPhaseEventWindow *= Kp;
            initinfo->taskMapper = new ProportionalBucketTaskMapper(
                Kp, tmEvent, adjustedPhaseEventWindow, initinfo->numROBs,
                bucketsPerRob, guard, leash, window, donateAscending, donateAll,
                includeDonated, reconfigure, loadMetric, uPolicy, *net);
        }
    } else if (taskMapperType == "HelpFirst") {
        initinfo->taskMapper = new HelpFirstTaskMapper(
            config.get<uint32_t>("sys.robs.taskMapper.underflowThreshold", 16));
    } else if (taskMapperType == "Self") {
        initinfo->taskMapper = new SelfTaskMapper();
    } else {
        panic("Invalid taskMapper type %s", taskMapperType.c_str());
    }

    // Periodic task mapper updates. This is triggerred only if the
    // task mapper requires re-configuring.
    if (requireUpdate) {
        TaskMapper* tm = initinfo->taskMapper;
        schedPeriodicEvent(samplingInterval, samplingInterval,
                           [=](uint64_t cycle) { tm->sample(); });
    }

    InitAllocPools(initinfo->numCores);
    initinfo->stackTracker = new StackTracker(initinfo->numCores *initinfo->numThreadsPerCore,
                                              initinfo->logStackSize);

    // Build the cores
    string coreprefix = string("sys.cores.");
    string type = config.get<const char*>(coreprefix + "type", "Simple");
    initinfo->isScoreboard = (type == "Scoreboard" || type == "OoO");

    string dcache = config.get<const char*>(coreprefix + "dcache");

    SbCore::IssuePriority priority;
    string prio = config.get<const char*>(coreprefix + "priority", "RoundRobin");
    if (prio == "RoundRobin") priority = SbCore::IssuePriority::ROUND_ROBIN;
    else if (prio == "Timestamp") priority = SbCore::IssuePriority::TIMESTAMP;
    else if (prio == "StartOrder") priority = SbCore::IssuePriority::START_ORDER;
    else if (prio == "SAI") priority = SbCore::IssuePriority::SAI;
    else if (prio == "Fixed") priority = SbCore::IssuePriority::FIXED;
    else if (prio == "TSFixed") priority = SbCore::IssuePriority::TS_FP;
    else if (prio == "ICount") priority = SbCore::IssuePriority::ICOUNT;
    else if (prio == "TSICount") priority = SbCore::IssuePriority::TS_ICOUNT;
    else panic("Unknown issue priority scheme %s", prio.c_str());

    uint64_t issueWidth = config.get<uint32_t>(coreprefix + "issueWidth", 1);
    if (!(issueWidth == 1 || issueWidth == 2 || issueWidth == 4))
        panic("issue width has to be either 1, 2, or 4: provided %ld", issueWidth);

    uint64_t mispredictPenalty = config.get<uint32_t>(coreprefix + "mispredictPenalty", 5);
    const bool stallOnLSQ = config.get<bool>(coreprefix + "stallIssueOnLSQ", true);

    // Global core info
    initinfo->cores = static_cast<Core**>(
        memalign(CACHE_LINE_BYTES, initinfo->numCores * sizeof(Core*)));
    uint32_t coreIdx;
    for (coreIdx = 0; coreIdx < initinfo->numCores; ++coreIdx) {
        stringstream ss;
        ss << "core-" << coreIdx;
        string name(ss.str().c_str());

        // Get the caches
        CacheGroup& dgroup = *cMap[dcache];
        if (assignedCaches[dcache] >= dgroup.size()) {
            panic(
                "%s: dcache group %s (%ld caches) is fully used, can't connect "
                "more cores to it",
                name.c_str(), dcache.c_str(), dgroup.size());
        }

        FilterCache* dc = dynamic_cast<FilterCache*>(dgroup[assignedCaches[dcache]][0]);
        assert(dc);
        dc->setSourceId(coreIdx);
        assignedCaches[dcache]++;

        string startThreads = config.get<const char*>("sys.cores.startThreads", "Max");
        (void) startThreads;
        // Build the core
        uint32_t lbe = config.get<uint32_t>(coreprefix + "loadBufferEntries", 16);
        uint32_t sbe = config.get<uint32_t>(coreprefix + "storeBufferEntries", 16);
        if (type == "Simple"){
            initinfo->cores[coreIdx] =
                new Core(name, coreIdx, dc);
        } else {
            assert(type == "Scoreboard" || type == "OoO");
            uint32_t coreStartThreads;
            if (startThreads == "One") {
                coreStartThreads = 1;
            } else if (startThreads == "Half") {
                coreStartThreads = static_cast<uint32_t>(
                    std::max(1ul, initinfo->numThreadsPerCore / 2));
            } else if (startThreads == "Max") {
                coreStartThreads =
                    static_cast<uint32_t>(initinfo->numThreadsPerCore);
            } else {
                panic("Unrecognized start threads %s", startThreads.c_str());
            }
            if (type == "Scoreboard") {
                initinfo->cores[coreIdx] = new SbCore(
                    name, coreIdx,
                    initinfo->numThreadsPerCore, dc, priority, issueWidth,
                    mispredictPenalty, lbe, sbe,
                    coreStartThreads /*only relevant if throttler is used*/);
            } else {
                initinfo->cores[coreIdx] = new OoOCore(
                    name, coreIdx,
                    initinfo->numThreadsPerCore, dc, priority, issueWidth,
                    mispredictPenalty, lbe, sbe,
                    coreStartThreads /*only relevant if throttler is used*/,
                    stallOnLSQ);
            }

        }
    }

    assert(initinfo->numCores == coreIdx);

    // Check that all the terminal caches are fully connected
    for (const char* grp : cacheGroupNames) {
         bool isTerminal = (childMap.count(grp) == 0);
        if (isTerminal && assignedCaches[grp] != cMap[grp]->size()) {
            panic(
                "%s: Terminal cache group not fully connected, %ld caches, %d "
                "assigned",
                grp, cMap[grp]->size(), assignedCaches[grp]);
        }
    }

    std::string mayspecMode = config.get<const char*>(
            "sys.robs.mayspecSpeculationMode", "Must");
    if (mayspecMode == "Must") initinfo->mayspecMode = RunCondition::MUSTSPEC;
    else if (mayspecMode == "May") initinfo->mayspecMode = RunCondition::MAYSPEC;
    else if (mayspecMode == "Cant") initinfo->mayspecMode = RunCondition::CANTSPEC;
    else panic("Invalid mayspecSpeculationMode %s", mayspecMode.c_str());

    bool serializeSpatialIDs =
        config.get<bool>("sys.robs.serializeSpatialIDs", false);
    const uint32_t taskQCapacity =
        config.get<uint32_t>("sys.robs.taskQ.capacity", UINT32_MAX);
    const uint32_t taskQOverflow =
        config.get<uint32_t>("sys.robs.taskQ.overflow", 7 * taskQCapacity / 8);
    const uint32_t tiedCapacity =
        config.get<uint32_t>("sys.robs.taskQ.tiedCapacity", 6 * taskQCapacity / 8);

    // FIXME(dsm): With proper thresholds, removableCapacity seems outdated...
    const uint32_t removableCapacity =
        config.get<uint32_t>("sys.robs.taskQ.removableCapacity", 2);
    const uint32_t commitQCapacity =
        config.get<uint32_t>("sys.robs.commitQ.capacity", taskQOverflow);
    initinfo->tasksPerSpiller =
        config.get<uint32_t>("sys.robs.taskQ.tasksPerSpiller",
                             "sys.robs.taskQ.tasksPerCoalescer", 16);
    initinfo->tasksPerRequeuer =
        config.get<uint32_t>("sys.robs.taskQ.tasksPerRequeuer",
                             "sys.robs.taskQ.tasksPerSplitter", 8);
    initinfo->extraChildren = config.get<uint32_t>("sys.robs.extraChildren", 0);

    // dsm: Underflows prioritize producers on low-capacity situations to avoid
    // running out of work or running high-timestamp tasks before
    // lower-timestamp ones are available
    uint32_t defaultUnderflow = serializeSpatialIDs ? 0 : (taskQCapacity - taskQOverflow);
    // [mcj] disable until we are stable on how to avoid deadlocks etc
    defaultUnderflow = 0;
    const uint32_t taskQUnderflow =
        config.get<uint32_t>("sys.robs.taskQ.underflow", defaultUnderflow);

    assert(initinfo->tasksPerSpiller);
    assert(initinfo->tasksPerRequeuer);
    assert(initinfo->tasksPerSpiller >= initinfo->tasksPerRequeuer);

    string tbStr = config.get<const char*>("sys.robs.tieBreakPolicy", "Lazy");
    rob::TBPolicy tbPolicy;
    if (tbStr == "Enqueue") tbPolicy = rob::TBPolicy::ENQUEUE;
    else if (tbStr == "Dequeue") tbPolicy = rob::TBPolicy::DEQUEUE;
    else if (tbStr == "Lazy") tbPolicy = rob::TBPolicy::LAZY;
    else panic("Invalid tieBreakPolicy %s", tbStr.c_str());

    assert(tbPolicy != rob::TBPolicy::LAZY || initinfo->selectiveAborts);

    bool clearTieBreakerOnAbort =
        config.get<bool>("sys.robs.clearTieBreakerOnAbort", false);

    if (clearTieBreakerOnAbort && tbPolicy == rob::TBPolicy::ENQUEUE)
        panic("Can't have enqueue-time tiebreaking + clearTieBreakOnAbort.");

    bool bulkSynchronousTasks =
        config.get<bool>("sys.robs.bulkSynchronousTasks", false);

    bool adaptiveThrottle =
        config.get<bool>("sys.robs.adaptiveThrottle", false);

    initinfo->usePreciseAddressSets = ("Precise" == std::string(
                config.get<const char*>("sys.robs.addressSet.type", "Bloom")));

    initinfo->relaxed = config.get<bool>("sys.robs.relaxedOrder", false);

    uint32_t maxFrameDepth =
        config.get<uint32_t>("sys.robs.maxFrameDepth", UINT32_MAX);
    //FIXME(victory): Remove this hack when we have a less-broken
    // implementation of triggering zoom-ins that does not require having
    // tasks and threads block for the duration of the zoom-in.
    if (maxFrameDepth != UINT32_MAX &&
        initinfo->numCores * initinfo->numThreadsPerCore == 1) {
        warn("Ignoring maxFrameDepth for single-threaded simulation.");
        maxFrameDepth = UINT32_MAX;
    }
    initinfo->maxFrameDepth = maxFrameDepth;
    assert(maxFrameDepth >= 2);
    initinfo->robs.resize(initinfo->numROBs);
    std::vector<ThreadCountController*> threadCountControllers;
    std::vector<RobStatsCollector*> robStatsCollectors;
    for (uint32_t i = 0; i < initinfo->numROBs; i++) {
        ThreadCountController* tcc = new ThreadCountController(i);
        initinfo->robs[i] = new ROB(
            commitQCapacity,
            stringToCQAdmissionPolicy(config.get<const char*>(
                "sys.robs.commitQ.admissionPolicy", "Half")),
            taskQCapacity, taskQOverflow, removableCapacity, tiedCapacity,
            taskQUnderflow, initinfo->numCores / initinfo->numROBs,
            initinfo->numThreadsPerCore, tbPolicy, clearTieBreakerOnAbort,
            serializeSpatialIDs, bulkSynchronousTasks, adaptiveThrottle,
            "rob-" + std::to_string(i), i, initinfo->taskMapper, tcc,
            initinfo->tsarray[i]->getGroupSize());
        RobStatsCollector* rsc = new RobStatsCollector(initinfo->robs[i]);
        tcc->setROB(initinfo->robs[i]);
        threadCountControllers.push_back(tcc);
        robStatsCollectors.push_back(rsc);
    }

    for (coreIdx = 0; coreIdx < initinfo->numCores; ++coreIdx) {
        threadCountControllers[getROBIdx(coreIdx)]->addCore(initinfo->cores[coreIdx]);
        robStatsCollectors[getROBIdx(coreIdx)]->addCore(initinfo->cores[coreIdx]);
    }

    ConfigureStreamPrefetchers(config, cMap, childMap, initinfo->numCores,
        initinfo->cores, initinfo->robs);

    const string threadThrottlerType =
        config.get<const char*>("sys.robs.throttler.type", "None");
    const string threadThrottlerMode =
        config.get<const char*>("sys.robs.throttler.mode", "Distributed");
    const uint32_t threadThrottlerSamplingInterval =
        config.get<uint32_t>("sys.robs.throttler.samplingInterval", 10000);
    const uint32_t threadThrottlerPhaseEventWindow =
        config.get<uint32_t>("sys.robs.throttler.phaseEventWindow", 128);
    const string threadThrottlerPhaseEventStr = config.get<const char*>(
        "sys.robs.throttler.phaseEvent", "Commit");
    const uint32_t deltaThreads =
        config.get<uint32_t>("sys.robs.throttler.deltaThreads", 1);
    const uint32_t samplesToWait = config.get<uint32_t>(
        "sys.robs.throttler.samplesToWait", 2);
    double ttK =
        double(config.get<uint32_t>("sys.robs.throttler.k", 80)) / 100.0;
    double guard =
        double(config.get<uint32_t>("sys.robs.throttler.guard", 15)) / 100.0;
    const string directionBiasString =
        config.get<const char*>("sys.robs.throttler.directionBias", "Down");
    const string perfMetric =
        config.get<const char*>("sys.robs.throttler.perfMetric", "CommitInstrs");

    throttler::PhaseEvent threadThrottlerPhaseEvent;
    if (threadThrottlerPhaseEventStr == "Cycle")
        threadThrottlerPhaseEvent = throttler::PhaseEvent::CYCLE;
    else if (threadThrottlerPhaseEventStr == "Dequeue")
        threadThrottlerPhaseEvent = throttler::PhaseEvent::DEQUEUE;
    else if (threadThrottlerPhaseEventStr == "Commit")
        threadThrottlerPhaseEvent = throttler::PhaseEvent::COMMIT;
    else
        panic("Invalid thread throttler phase event config %s",
              threadThrottlerPhaseEventStr.c_str());

    throttler::DirectionBias directionBias;
    if (directionBiasString == "Down")
        directionBias = throttler::DirectionBias::DOWN;
    else if (directionBiasString == "Up")
        directionBias = throttler::DirectionBias::UP;
    else if (directionBiasString == "Random")
        directionBias = throttler::DirectionBias::RANDOM;
    else
        panic("Invalid direction bias string %s", directionBiasString.c_str());

    // [ssub] Is there a better way to do this? Apparently lambda functions do not
    // support template specialization (although they do support type auto
    // based type inference). Even in C++14, despite a proposal for this:
    // http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2016/p0428r0.pdf
    // And no generic lambdas until C++14 either (only supported with gcc
    // >=4.9). https://gcc.gnu.org/projects/cxx-status.html
    auto createUintThreadThrottler = [&](
        uint32_t idx,
        std::function<std::vector<uint64_t>(RobStatsCollector*)> perfMetricFn,
        std::function<bool(double, double, double)> betterFn)
        -> ThreadThrottlerUnit* {
            ThreadThrottlerUnit* tt = nullptr;
            if (threadThrottlerPhaseEventStr == "ThresholdThreadThrottler") {
                tt = new ThresholdThreadThrottler<uint64_t>(
                    idx, threadThrottlerPhaseEvent,
                    threadThrottlerPhaseEventWindow, deltaThreads, ttK, guard,
                    perfMetricFn, betterFn);
            } else if (threadThrottlerType == "UpDown") {
                tt = new UpDownThreadThrottler<uint64_t>(
                    idx, threadThrottlerPhaseEvent,
                    threadThrottlerPhaseEventWindow, deltaThreads,
                    samplesToWait, guard, directionBias, perfMetricFn,
                    betterFn);
            } else if (threadThrottlerType == "GradientDescent") {
                tt = new GradientDescentThreadThrottler<uint64_t>(
                    idx, threadThrottlerPhaseEvent,
                    threadThrottlerPhaseEventWindow, deltaThreads,
                    samplesToWait, guard, directionBias, perfMetricFn,
                    betterFn);
            } else if (threadThrottlerType == "None") {
                tt = nullptr;
            }
            return tt;
        };

    auto createStallAndAbortsThreadThrottler = [&](
        uint32_t idx, std::function<std::vector<perfMetric::StallAndAborts>(
                          RobStatsCollector*)> perfMetricFn,
        std::function<bool(double, double, double)> betterFn)
        -> ThreadThrottlerUnit* {
            ThreadThrottlerUnit* tt = nullptr;
            if (threadThrottlerPhaseEventStr == "ThresholdThreadThrottler") {
                tt = new ThresholdThreadThrottler<
                    perfMetric::StallAndAborts>(
                    idx, threadThrottlerPhaseEvent,
                    threadThrottlerPhaseEventWindow, deltaThreads, ttK, guard,
                    perfMetricFn, betterFn);
            } else if (threadThrottlerType == "UpDown") {
                tt = new UpDownThreadThrottler<
                    perfMetric::StallAndAborts>(
                    idx, threadThrottlerPhaseEvent,
                    threadThrottlerPhaseEventWindow, deltaThreads,
                    samplesToWait, guard, directionBias, perfMetricFn,
                    betterFn);
            } else if (threadThrottlerType == "GradientDescent") {
                tt = new GradientDescentThreadThrottler<
                    perfMetric::StallAndAborts>(
                    idx, threadThrottlerPhaseEvent,
                    threadThrottlerPhaseEventWindow, deltaThreads,
                    samplesToWait, guard, directionBias, perfMetricFn,
                    betterFn);
            } else if (threadThrottlerType == "None") {
                tt = nullptr;
            }
            return tt;
        };

    auto buildThreadThrottler = [&](uint32_t idx)
        -> ThreadThrottlerUnit* {
            if (perfMetric == "CommitInstrs") {
                return createUintThreadThrottler(idx, getCommittedInstructions,
                                                 throttler::higherIsBetter);
            } else if (perfMetric == "AbortInstrs") {
                return createUintThreadThrottler(idx, getAbortedInstructions,
                                                 throttler::lowerIsBetter);
            } else if (perfMetric == "AbortToStallRatio") {
                return createStallAndAbortsThreadThrottler(
                    idx, getStallAndAborts, throttler::lowerIsBetter);
            } else panic("Invalid perf metric %s", perfMetric.c_str());
            return nullptr;
        };

    if (threadThrottlerType != "None") {
        AggregateStat* ttStat = new AggregateStat(true);
        // This could be generalized to having a throttler unit per
        // R RoBs if required. For now, just the two extremes.
        if (threadThrottlerMode == "Coordinated") {
            ttStat->init("threadThrottler", "Coordinated thread throttler stats");
            // Build one centralized throttler unit
            auto tt = buildThreadThrottler(0); assert(tt);
            initinfo->throttlers.push_back(tt);
            tt->initStats(ttStat);
        } else {
            ttStat->init("threadThrottler", "Distributed thread throttler stats");
            // Build a throttler unit at each RoB
            for (uint32_t i = 0; i < initinfo->robs.size(); ++i) {
                auto tt = buildThreadThrottler(i); assert(tt);
                initinfo->throttlers.push_back(tt);
                tt->initStats(ttStat);
            }
        }
        initinfo->rootStat->append(ttStat);

        // Connect the throttler unit(s) to the thread count controllers
        // and ROB stats collectors.
        assert(initinfo->robs.size() % initinfo->throttlers.size() == 0);
        uint32_t numRobsPerThrottler =
            initinfo->robs.size() / initinfo->throttlers.size();
        assert(numRobsPerThrottler > 0);
        for (uint32_t i = 0; i < initinfo->robs.size(); ++i) {
            uint32_t throttlerIdx = i / numRobsPerThrottler;
            initinfo->throttlers[throttlerIdx]->addStatsCollector(robStatsCollectors[i]);
            initinfo->throttlers[throttlerIdx]->addThreadCountController(threadCountControllers[i]);
        }

        // Sample thread throttler unit periodically.
        schedPeriodicEvent(threadThrottlerSamplingInterval,
                           threadThrottlerSamplingInterval,
                           [](uint64_t cycle) {
                               for (uint32_t i = 0; i < ossinfo->throttlers.size(); i++)
                                   ossinfo->throttlers[i]->sample(cycle);
                           });
    }

    string robNodes = config.get<const char*>("sys.robs.nodes", "");
    net->addEndpoints("rob", initinfo->robs, robNodes);

    // Task send buffers (for now == robs, but could make per-core)
    uint32_t tsbEntries = config.get<uint32_t>("sys.robs.tsbEntries", 32);
    uint32_t minBackoff = config.get<uint32_t>("sys.robs.tsbMinBackoff", 25);
    uint32_t maxBackoff = config.get<uint32_t>("sys.robs.tsbMaxBackoff", 1000);

    uint32_t coresPerTsb = initinfo->numCores / initinfo->numROBs;
    // NOTE: Each TSB must be large enough to hold all initial tasks
    for (uint32_t i = 0; i < initinfo->numROBs; i++) {
        TSB* tsb = new TSB("tsb-" + std::to_string(i), i, tsbEntries,
                           minBackoff, maxBackoff, coresPerTsb);
        initinfo->tsbs.push_back(tsb);
    }
    net->addEndpoints("tsb", initinfo->tsbs, robNodes);
    for (uint32_t i = 0; i < initinfo->numROBs; i++) {
        initinfo->tsbs[i]->setPorts(
            net->getPorts<ROBTaskPort>(initinfo->tsbs[i], initinfo->robs));
        ROB* rob = initinfo->robs[i];
        rob->setPorts(net->getPorts<TSBPort>(rob, initinfo->tsbs));
        rob->setPorts(net->getPorts<CutTiePort>(rob, initinfo->robs));
        rob->setPorts(net->getAtomicPorts<ChildAbortPort>(rob, initinfo->robs));
    }

    ConnectCDs(initinfo->robs, initinfo->tsbs, initinfo->cores);

    // Initialize GVT arbiters
    uint64_t gvtUpdatePeriod =
        config.get<uint32_t>("sys.robs.gvtUpdatePeriod", 200);

    std::vector<LVTUpdatePort*>* robLvtPorts = new std::vector<LVTUpdatePort*>();

    // Place FfTaskQueue at same network node as the root arbiter it will talk
    // to.  It should not traverse the network and cause contention.
    auto ffTaskQueue = new FfTaskQueue();
    std::vector<FfTaskQueue*> ffGroup = {ffTaskQueue};
    net->addEndpoints("ffTaskQueue", ffGroup, "center");
    LVTUpdatePort* ffLvtPort;

    if (config.get<bool>("sys.robs.centralizedGvtArbiter", false)) {
        // TODO: Eventually deprecate this option.

        auto gvtArbiter = new GVTArbiter("gvtArbiter");

        // Connect GVT arbiter to ROBs
        std::vector<GVTArbiter*> gvtGroup = {gvtArbiter};
        net->addEndpoints("gvtArbiter", gvtGroup, "center");

        gvtArbiter->setChildPorts(
            net->getPorts<Port<GVTUpdateMsg>>(gvtArbiter, initinfo->robs));

        uint32_t idx = 0;
        for (ROB* rob : initinfo->robs) {
            robLvtPorts->push_back(
                net->getPort<LVTUpdatePort>(rob, gvtArbiter));
            rob->setChildIdx(idx++);
        }
        ffLvtPort = net->getPort<LVTUpdatePort>(ffTaskQueue, gvtArbiter);
    } else {
        std::vector<GVTArbiter*> gvtArbiters(netNodes);
        for (uint32_t n = 0; n < netNodes; n++)
            gvtArbiters[n] = new GVTArbiter("gvtArbiter-" + Str(n));
        net->addEndpoints("gvtArbiter", gvtArbiters, "");

        // Build arbiter hierarchy by doing a BFS walk from the center node
        boost::dynamic_bitset<> visited(gvtArbiters.size(), false);
        uint32_t root = net->getCenterNode();
        visited[root] = true;
        std::vector<uint32_t> nodeFringe = {root};
        while (!nodeFringe.empty()) {
            // Tuple format: (prio, src, dst)
            // prio = abs(src - dst) to prioritize by dimension (to get a "spine")
            std::priority_queue<std::tuple<uint32_t, uint32_t, uint32_t>> pq;

            for (uint32_t i : nodeFringe)
                for (uint32_t n : net->getNeighbors(i))
                    if (!visited[n])
                        pq.push(std::make_tuple(i < n ? n - i : i - n, i, n));

            std::map<uint32_t, std::vector<uint32_t>> children;
            for (uint32_t i : nodeFringe) children[i] = {};
            nodeFringe.clear();

            while (!pq.empty()) {
                uint32_t prio, i, n;
                std::tie(prio, i, n) = pq.top();
                pq.pop();
                if (!visited[n]) {
                    children[i].push_back(n);
                    nodeFringe.push_back(n);
                    visited[n] = true;
                }
            }

            for (auto& e : children) {
                GVTArbiter* parent = gvtArbiters[e.first];
                std::vector<GVTArbiter*> childArbs;
                for (uint32_t n : e.second) childArbs.push_back(gvtArbiters[n]);

                auto gports = net->getPorts<Port<GVTUpdateMsg>>(parent, childArbs);

                // Local ports start at idx = 0.
                // We can have multiple ROBs per network node (in the general network config)
                // and hence there can be multiple local ports.
                uint32_t idx = 0;
                // Add ROBs of this node
                for (ROB* rob : initinfo->robs) {
                    if (e.first == net->getNode(rob)) {
                        DEBUG("[GVTArbiters] Connecting gvtArbiter-%u (ROB %u) -> %s", e.first,
                              rob->getIdx(), Str(e.second).c_str());
                        gports.push_back(
                            net->getPort<Port<GVTUpdateMsg>>(parent, rob));
                        rob->setChildIdx(idx++);
                    }
                }

                parent->setChildPorts(gports);

                // All local ports have been captured by idx.
                // Now start assigning child gvt arbiters their child idx.
                for (GVTArbiter* child: childArbs) {
                    child->setParentPort(
                        net->getPort<Port<LVTUpdateMsg>>(child, parent));
                    child->setChildIdx(idx++);
                }
            }
        }
        assert((~visited).none());  // TODO: Use visited.all() once we upgrade boost

        for (ROB* rob : initinfo->robs) {
            DEBUG("[GVTArbiters] ROB %u to %s", rob->getIdx(),
                  gvtArbiters[net->getNode(rob)]->name());
            robLvtPorts->push_back(
                net->getPort<LVTUpdatePort>(rob, gvtArbiters[net->getNode(rob)]));
        }

        ffLvtPort = net->getPort<LVTUpdatePort>(ffTaskQueue, gvtArbiters[root]);

        for (GVTArbiter* ga : gvtArbiters)
            ga->printConnections();
    }

    // Connect ROBs to GVT arbiters
    for (uint32_t i = 0; i < initinfo->robs.size(); i++)
        initinfo->robs[i]->setPort((*robLvtPorts)[i]);
    FfTaskQueueSetPort(ffLvtPort);

    // Schedule a periodic GVT update event (lvt reads do need to happen
    // atomically across all ROBSs, and this is hard to do other than with a
    // single event, though physically it's trivial to implement with
    // synchronized clocks)
    schedPeriodicEvent(gvtUpdatePeriod, gvtUpdatePeriod,
                       [epoch = 1ULL](uint64_t cycle) mutable {
                           for (ROB* r : ossinfo->robs) r->sendLVTUpdate(epoch);
                           FfTaskQueueSendLvt(epoch);
                           epoch++;
                       });

    // Task balancer (must be initialized after robs)
    string tbPrefix = "sys.robs.taskBalancer.";
    string tbType = config.get<const char*>(tbPrefix + "type", "None");
    if (tbType == "None") {  // nothing to do
    } else {
        assert(tbType == "Stealing");
        uint32_t cpr = initinfo->numCores / initinfo->numROBs;
        StealingTaskBalancer* tb = new StealingTaskBalancer(
            initinfo->robs,
            config.get<uint32_t>(tbPrefix + "victimThreshold", cpr),
            config.get<uint32_t>(tbPrefix + "stealThreshold", cpr),
            config.get<bool>(tbPrefix + "lifo", false));

        uint32_t period = config.get<uint32_t>(tbPrefix + "period", 200);
        schedPeriodicEvent(period, period,
                           [tb](uint64_t cycle) { tb->balance(cycle); });
    }

    // Init stats: caches, mem, net, core, rob, load balancer
    for (const char* group : cacheGroupNames) {
        AggregateStat* groupStat = new AggregateStat(true);
        groupStat->init(strdup(group), "Cache stats");
        for (vector<Cache*>& banks : *cMap[group])
            for (Cache* bank : banks) bank->initStats(groupStat);
        initinfo->rootStat->append(groupStat);
    }

    AggregateStat* memStat = new AggregateStat(true);
    memStat->init("mem", "Memory controller stats");
    for (auto mem : mems) mem->initStats(memStat);
    initinfo->rootStat->append(memStat);

    net->initStats(initinfo->rootStat);  // only one net for now...

    AggregateStat* coreStat = new AggregateStat(true);
    coreStat->init("core", "Core stats");
    for (coreIdx = 0; coreIdx < initinfo->numCores; ++coreIdx)
        (initinfo->cores[coreIdx])->initStats(coreStat);
    initinfo->rootStat->append(coreStat);

    AggregateStat* robStat = new AggregateStat(true);
    AggregateStat* tccStat = new AggregateStat(true);
    robStat->init("rob", "ROB stats");
    tccStat->init("threadCountController", "Thread count controller stats");
    for (size_t i = 0; i < initinfo->robs.size(); i++) {
        initinfo->robs[i]->initStats(robStat);
        threadCountControllers[i]->initStats(tccStat);
    }
    initinfo->rootStat->append(robStat);
    initinfo->rootStat->append(tccStat);

    AggregateStat* tsbStat = new AggregateStat(true);
    tsbStat->init("tsb", "TSB stats");
    for (TSB* tsb : initinfo->tsbs) tsb->initStats(tsbStat);
    initinfo->rootStat->append(tsbStat);

    AggregateStat* abortHandlerStat = new AggregateStat(true);
    abortHandlerStat->init("abort-handler", "Abort handler stats");
    initinfo->abortHandler->initStats(abortHandlerStat);
    initinfo->rootStat->append(abortHandlerStat);

    AggregateStat* taskMapperStat = new AggregateStat(true);
    taskMapperStat->init("task-mapper", "Task mapper stats");
    initinfo->taskMapper->initTmStats(taskMapperStat);
    initinfo->tmStat->append(taskMapperStat);

    AggregateStat* tmStat = new AggregateStat(true);
    tmStat->init("task-mapper", "Task mapper stats");
    initinfo->taskMapper->initStats(tmStat);
    initinfo->rootStat->append(tmStat);

    auto reconfigIntervalLambda = [&]() { return initinfo->taskMapper->getMeanReconfigInterval(); };
    auto reconfigIntervalStat = makeLambdaStat(reconfigIntervalLambda);
    reconfigIntervalStat->init("tm-reconfigIntrvl", "Task mapper average reconfiguration interval (cycles)");
    initinfo->rootStat->append(reconfigIntervalStat);

    // dsm: Make configurable if needed/slow
    InitDomainProfiling(initinfo->rootStat, 10, 20, 2);
    InitTaskProfiling(config.get<bool>("sim.profileByPc", true));

    // Odds and ends: BuildCacheGroup new'd the cache groups, we need to delete
    // them
    for (pair<string, CacheGroup*> kv : cMap) delete kv.second;
    cMap.clear();

    info("Initialized system");
}

void SimInit(const char* configFile) {
    Config config(configFile);

    ossinfo = initinfo;

    // System-wide params
    initinfo->lineSize = config.get<uint32_t>("sys.lineSize", 64);
    initinfo->lineBits = ilog2(initinfo->lineSize);
    assert(1ul << initinfo->lineBits == initinfo->lineSize);
    initinfo->freqMHz = config.get<uint32_t>("sys.frequency", 2000);
    initinfo->outputDir = getcwd(nullptr, 0);  // already absolute
    initinfo->phaseLength = config.get<uint32_t>("sim.phaseLength", 10000);
    initinfo->logIndirectTargets = config.get<bool>("sim.logIndirectTargets",
                                                    false);
    initinfo->maxHeartbeats = config.get<uint64_t>("sim.maxHeartbeats", 0L);
    initinfo->ffHeartbeats = config.get<uint64_t>("sim.ffHeartbeats", 0L);
    assert_msg(initinfo->maxHeartbeats == 0
               || initinfo->maxHeartbeats > initinfo->ffHeartbeats,
               "if both maxHeartbeats and ffHeartbeats are set,"
               " maxHeartbeats should be larger");

    // Termination conditions
    uint64_t maxCycles = config.get<uint64_t>("sim.maxCycles", 0L);
    if (maxCycles) {
        schedEvent(maxCycles, [](uint64_t cycle) {
            info("Max cycles reached (%ld), terminating simulation", cycle);
            SimEnd();
        });
    }

    uint64_t maxInstrs = config.get<uint64_t>("sim.maxInstrs", 0L);
    if (maxInstrs) {
        schedRecurringEvent(0, [maxInstrs](uint64_t cycle) {
            uint64_t totalInstrs = 0;
            for (uint32_t c = 0; c < ossinfo->numCores; c++)
                totalInstrs += ossinfo->cores[c]->getInstructionCount();
            if (totalInstrs >= maxInstrs) {
                info("Max instructions reached (%ld), terminating simulation",
                     totalInstrs);
                SimEnd();
                return 0ul;
            } else {
                uint64_t remaining = maxInstrs - totalInstrs;
                // Conservative estimate (IPC = 4)
                uint64_t maxRate = ossinfo->numCores * 4;
                uint64_t interval = std::max(1ul, remaining / maxRate);
                DEBUG("[%ld] maxInstrsEv: %ld < %ld, rate < %ld, interval %ld",
                    cycle, totalInstrs, maxInstrs, maxRate, interval);
                return interval;
            }
        });
    }

    PreInitStats();
    InitGlobalStats();

    // Create cores (TODO: why here?)
    initinfo->numCores = config.get<uint32_t>("sys.cores.cores", 1);
    initinfo->numThreadsPerCore = config.get<uint32_t>("sys.cores.threads", 1);

    // Create ROBs (TODO)
    initinfo->numROBs = config.get<uint32_t>("sys.robs.robs", 1);
    if (initinfo->numCores % initinfo->numROBs != 0) {
        panic("Cores must be a multiple of ROBs");
    }

    // FIXME For now local delay is assumed as the average Manhattan delay to
    // the center of the cluster
    uint32_t localDelay = std::ceil(std::sqrt(
        initinfo->numCores / initinfo->numROBs));  // this is approximate
    initinfo->enqueueDelay =
        config.get<uint32_t>("sys.robs.enqueueDelay", localDelay);
    initinfo->dequeueDelay =
        config.get<uint32_t>("sys.robs.dequeueDelay", localDelay);

    initinfo->numThreadsPerROB = (initinfo->numCores * initinfo->numThreadsPerCore) / initinfo->numROBs;
    initinfo->logStackSize = config.get<uint32_t>("sim.logStackSize", 18);

    // Build the system
    InitSystem(config);

    PostInitStats(config);

    // Write config out (if strict, unused variables cause a panic)
    bool strictConfig = config.get<bool>("sim.strictConfig", true);
    config.writeAndClose("out.cfg", strictConfig);

    // Initialize random number generator deterministically
    // Current users: thread_throttler
    std::srand(42);
}
