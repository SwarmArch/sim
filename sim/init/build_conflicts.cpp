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

#include <algorithm>
#include <regex>

#include "sim/sim.h"
#include "sim/network.h"

#include "sim/init/builders.h"

#include "sim/bithacks.h"
#include "sim/conflicts/abort_handler.h"
#include "sim/conflicts/bloom_address_accessors.h"
#include "sim/conflicts/precise_address_accessors.h"
#include "sim/conflicts/llc_cd.h"
#include "sim/conflicts/terminal_cd.h"
#include "sim/conflicts/tile_cd.h"
#include "sim/core/core.h"
#include "sim/memory/filter_cache.h"

// Keep pointers to tile/terminalCDs, to wire them up to other things later
// NOTE(dsm): A colossal hack, but at least way simpler than the previous hack.
// At this point we should be under no illusion that we are building a general
// hierarchy of conflict detection groups, and trying to shoehorn some
// hierarchical thing into it is just painful. This sort of hack always arises
// the moment you have circular dependences at initialization.
//
// TODO(dsm): Either refactor units to be more modular, or consider having a
// Tile object that has pointers to ROB/TSB/TileCD/other units
static std::vector<TileCD*> tileCDs;
static std::vector<TerminalCD*> terminalCDs;
static std::vector<std::tuple<AddressAccessorTable*, AddressAccessorTable*>> accessorTables;

static H3HashFamily* bloomHash = nullptr;

// FIXME(mcj) once TileCD is the sole owner of TSArray paritions, we can remove
// that argument. Same with the partitions of the sticky directory
static void BuildConflictDetection(const Config& config,
                            Cache* cache, bool isTerminal, TSArray* tsarray,
                            LLCCD* llcCD) {
    string name = cache->nameStr() + "-cd";
    if (isTerminal) {
        TerminalCD* cd = new TerminalCD(cache->numLines(), ossinfo->numThreadsPerCore, name);
        // TODO(mcj) if the network interposes components on the same node, then
        // use the network to connect these, e.g. to measure the number of GETX
        // from cd to the cache.
        cd->setCache(dynamic_cast<FilterCache*>(cache));
        cache->setConflictDetectionBottom(cd);
        terminalCDs.push_back(cd);
    } else if (tsarray) {
        assert(!llcCD);
        string addressSetType = config.get<const char*>("sys.robs.addressSet.type", "Bloom");
        uint32_t BITS = config.get<uint32_t>("sys.robs.addressSet.BITS", 2048);
        uint32_t K = config.get<uint32_t>("sys.robs.addressSet.K", 8);
        AddressAccessorTable* rTable;
        AddressAccessorTable* wTable;
        if (addressSetType == "Precise") {
            rTable = new PreciseAddressAccessorTable();
            wTable = new PreciseAddressAccessorTable();
        } else if (addressSetType == "Bloom") {
            if (!bloomHash) {
                // This hackery is why I wanted a Factory. Oh well
                bloomHash = new H3HashFamily(K, ilog2(BITS / K), 0xCAFE101A);
            }
            rTable = new BloomAddressAccessorTable(BITS, K, *bloomHash);
            wTable = new BloomAddressAccessorTable(BITS, K, *bloomHash);
        } else {
            panic("Invalid sys.robs.addressSet.type %s", addressSetType.c_str());
        }
        TileCD* cd = new TileCD(rTable, wTable, tsarray, name);

        cd->setCache(cache);
        cache->setConflictDetectionTop(cd);
        cache->setConflictDetectionBottom(cd);
        tileCDs.push_back(cd);
        accessorTables.push_back(std::make_tuple(rTable, wTable));
    } else {
        // FIXME(mcj) go back to building the LLCCD here, and keeping it out of
        // global state. [dsm] Note this is called once PER BANK. In fact
        // there'd be no problem with having one llccd per bank, but that
        // requires that we stop mucking with the sticky directories from
        // AbortHandler...
        cache->setConflictDetectionTop(llcCD);
        cache->setConflictDetectionBottom(llcCD);
    }
}

void BuildConflictDetectionGroup(const Config& config,
                                 const CacheGroup& cGroup,
                                 bool isTerminal,
                                 LLCCD* llcCD) {
    bool isTracker = (llcCD != nullptr);

    for (uint32_t i = 0; i < cGroup.size(); i++) {
        // FIXME(mcj) this is very presumptuous that L2's won't be multi-banked,
        // or if they are, they would somehow share the same tsarray. then again
        // this global TSArray should disappear with AbortController
        TSArray* tsarray =
            (!isTerminal && !isTracker) ? ossinfo->tsarray[i] : nullptr;
        for (uint32_t j = 0; j < cGroup[i].size(); j++) {
            Cache* cache = cGroup[i][j];
            BuildConflictDetection(config, cache, isTerminal, tsarray, llcCD);
        }
    }
}

void ConnectCDs(const std::vector<ROB*>& robs, const std::vector<TSB*>& tsbs,
                Core** cores /* <-- [mcj] Why isn't this a std::vector? */) {
    // Connect abort handler to all CDs
    ossinfo->abortHandler->setTileCDArray(tileCDs);
    ossinfo->abortHandler->setTerminalCDArray(terminalCDs);

    // Connect each tile CD to its ROB and TSB
    assert(tileCDs.size() == robs.size());
    assert(tileCDs.size() == tsbs.size());
    for (uint32_t l2 = 0; l2 < tileCDs.size(); l2++) {
        tileCDs[l2]->setROB(robs[l2]);
        tileCDs[l2]->setTSB(tsbs[l2]);
    }

    // Connect cores to terminal CDs
    assert(terminalCDs.size() == ossinfo->numCores);
    for (uint32_t c = 0; c < ossinfo->numCores; c++) {
        cores[c]->setTerminalCD(terminalCDs[c]);
    }

    // Connect terminal CDs to accessor tables
    uint32_t terminalCDsPerTile = terminalCDs.size() / tileCDs.size();
    assert(terminalCDs.size() % tileCDs.size() == 0);
    assert(accessorTables.size() == tileCDs.size());
    for (uint32_t l1 = 0; l1 < terminalCDs.size(); l1++) {
        AddressAccessorTable* rTable;
        AddressAccessorTable* wTable;
        std::tie(rTable, wTable) = accessorTables[l1 / terminalCDsPerTile];
        terminalCDs[l1]->setAccessorTables(rTable, wTable);
    }
}
