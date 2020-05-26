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
#include <cstdint>
#include <map>
#include <utility>

#include "sim/log.h"
#include "sim/memory/hash.h"

#undef DEBUG
#define DEBUG(args...) //info(args)

class Range {
  public:
    Range(uint64_t item) : _low(item), _high(item) {}
    Range(uint64_t low, uint64_t high) : _low(low), _high(high) {}

    bool operator<(const Range& rhs) const {
        if (_high < rhs._low)
            return true;
        return false;
    }

    uint64_t low() const { return _low; }
    uint64_t high() const { return _high; }

  private:
    uint64_t _low;
    uint64_t _high;
};

class MemoryPartition {
public:
    MemoryPartition(uint32_t _lineBits) : hashFn(2, 64, 0xBAEBAD0FF), lineBits(_lineBits) {}

    int32_t getPartId(const Address lineAddr, const uint32_t range,
                      const bool noHash = false) const {
        auto it = chunkToPartMap.lower_bound(Range(lineAddr));
        if (it != chunkToPartMap.end() && it->first.low() <= lineAddr) {
            DEBUG(
                "[memPart] Found 0x%lx in range: (0x%lx, 0x%lx). Returning "
                "partId %lu ",
                lineAddr, it->first.low(), it->first.high(), it->second);
            if (noHash)
                return it->second % range;
            else
                return hashFn.hash(1, hashFn.hash(0, it->second)) % range;
        } else {
            return -1;
        }
    }

    void insert(uint64_t startAddr, uint64_t endAddr, uint64_t partId) {
        uint64_t startLineAddr = startAddr >> lineBits;
        uint64_t endLineAddr = endAddr >> lineBits;
        DEBUG("[memPart] Inserting start: 0x%lx, end: 0x%lx, part: %lu", startLineAddr,
              endLineAddr, partId);
        chunkToPartMap.insert(
            std::make_pair(Range(startLineAddr, endLineAddr), partId));
    }

private:
    H3HashFamily hashFn;
    std::map<Range, uint64_t> chunkToPartMap;
    const uint32_t lineBits;
};
