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

#include <bitset>
#include <string>
#include <unordered_map>
#include "sim/assert.h"
#include "sim/log.h"
#include "sim/memory/constants.h"
#include "sim/types.h"

#undef DEBUG
#define DEBUG(args...) //info(args)

using std::string;

class StickyDirectory {
private:
    struct Entry {
        std::bitset<MAX_CACHE_CHILDREN> sharers;

        void clear() { sharers.reset(); }
        Entry() { sharers.reset(); }
    };

    const std::bitset<MAX_CACHE_CHILDREN> emptySet;

public:
    StickyDirectory(const string& _name) : name(_name) {}

    void add(Address lineAddr, TileId tile) {
        DEBUG("[%s] Adding 0x%lx to tile %u", name.c_str(), lineAddr, tile);
        auto it = sharerTable.find(lineAddr);
        if (it != sharerTable.end()) {
            Entry& e = sharerTable[lineAddr];
            DEBUG("prev value: %s, new value: true", e.sharers[tile]? "true" : "false");
            e.sharers[tile] = true;
        } else {
            Entry e;
            e.sharers[tile] = true;
            sharerTable[lineAddr] = e;
        }
    }

    void remove(Address lineAddr, TileId tile) {
        DEBUG("[%s] Removing 0x%lx from tile %u", name.c_str(), lineAddr, tile);
        auto it = sharerTable.find(lineAddr);
        if (it != sharerTable.end()) {
            Entry& e = it->second;
            e.sharers[tile] = false;
            if (e.sharers.none()) { // No sharers set, remove TODO Is this optimization required?
                DEBUG("[%s] No sharers for lineAddr: 0x%lx, removing from sticky directory", name.c_str(), lineAddr);
                sharerTable.erase(it);
            }
        }
    }

    const std::bitset<MAX_CACHE_CHILDREN>& getSharers(Address lineAddr) const {
        DEBUG("[%s] Querying lineAddr: 0x%lx", name.c_str(), lineAddr);
        auto it = sharerTable.find(lineAddr);
        if (it != sharerTable.end()) {
            const Entry& e = it->second;
            return e.sharers;
        } else return emptySet;
    }

private:
    std::unordered_map<Address, Entry> sharerTable;
    const string name;
};
