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
#include <cstdint>

#include "sim/memory/memory_hierarchy.h"
#include "sim/memory/constants.h"
#include "sim/log.h"

class AggregateStat;
class TimeStamp;

/**
 * The positions of the following pair of interfaces are analogous the coherence
 * controller top/bottom
 *
 * --------------------------
 * |    CD Bottom           |
 * |  LEVEL N CACHE         |
 * |    CD Top              |
 * --------------------------
 * --------------------------
 * |    CD Bottom           |
 * |  LEVEL N-1 CACHE       |
 * |    CD Top              |
 * --------------------------
 *  ...
 * --------------------------
 * |    CD Bottom           |
 * |  LEVEL 1 CACHE         |
 * --------------------------
 */
class ConflictDetectionBottom {
  public:
    // Should this level of cache send a conflict check upward?
    virtual bool shouldSendUp(AccessType reqType, const TimeStamp* reqTs, uint32_t lineId, MESIState state) { return false; }
    // Called first thing on processAccess().
    // Concurrent with parent access, if any
    virtual uint64_t access(const MemReq& req, uint64_t cycle) { return cycle; }
    // Called before every eviction or permissions downgrade (eg GETS_WB).
    // Can modify request (flags)
    virtual void eviction(MemReq& req, uint32_t lineId) {}
    // After the line has been installed in this cache (called only on misses)
    virtual void lineInstalled(const MemReq& req, const MemResp& resp,
                               uint32_t lineId, bool wasGlobalCheck) {}

    virtual uint64_t invalidate(const InvReq& req, InvResp& resp,
                                int32_t lineId, uint64_t cycle) {
        return cycle;
    }
    virtual void initStats(AggregateStat* parentStat) {}
};

class ConflictDetectionTop {
  public:
    // Update local state, given the invalidation response from a child
    virtual uint64_t invalidated(uint32_t childId,
                                 const InvReq& req, const InvResp& resp,
                                 uint64_t cycle) { return cycle; };

    // Return the speculative sharers to invalidate given the incoming request
    virtual std::bitset<MAX_CACHE_CHILDREN> sharers(Address lineAddr, bool checkReaders) const { return 0; }

    // Return true if we can give exclusive permissions to this request
    virtual bool giveExclusive(const MemReq& req) const { panic("Unimplemented"); }

    // Propagate last-accessor timestamp to GET response
    virtual void propagateLastAccTS(Address lineAddr, const TimeStamp* reqTs, uint32_t lineId, const TimeStamp*& lastAccTS, bool checkReaders) {}
};
