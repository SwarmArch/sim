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

#pragma once

/* Type and interface definitions of memory hierarchy objects */

#include <stdint.h>
#include <string>
#include <memory>

#include "sim/atomic_port.h"

/** TYPES **/

/* Addresses are plain 64-bit uints. This should be kept compatible with PIN addrints */
typedef uint64_t Address;

/* Types of Access. An Access is a request that proceeds from lower to upper
 * levels of the hierarchy (core->l1->l2, etc.)
 */
typedef enum {
    GETS, // get line, exclusive permission not needed (triggered by a processor load)
    GETX, // get line, exclusive permission needed (triggered by a processor store or atomic access)
    PUTS, // clean writeback (lower cache is evicting this line, line was not modified)
    PUTX, // dirty writeback (lower cache is evicting this line, line was modified)
    NUMMEMREQTYPES, // used by network.h HACKY! But better than hard-coding a number
} AccessType;

/* Types of Invalidation. An Invalidation is a request issued from upper to lower
 * levels of the hierarchy.
 */
typedef enum {
    INV,  // fully invalidate this line
    INVX, // invalidate exclusive access to this line (lower level can still keep a non-exclusive copy)
    FWD,  // don't invalidate, just send up the data (used by directories). Only valid on S lines.
    NUMINVREQTYPES, // used by network.h HACKY! But better than hard-coding a number
} InvType;

/* Coherence states for the MESI protocol */
typedef enum {
    I, // invalid
    S, // shared (and clean)
    E, // exclusive and clean
    M  // exclusive and dirty
} MESIState;

//Convenience methods for clearer debug traces
const char* AccessTypeName(AccessType t);
const char* InvTypeName(InvType t);
const char* MESIStateName(MESIState s);

inline bool IsGet(AccessType t) { return t == GETS || t == GETX; }
inline bool IsPut(AccessType t) { return t == PUTS || t == PUTX; }
inline bool IsValid(MESIState s) { return s != I; }
inline bool IsExclusive(MESIState s) { return s == M || s == E; }

class TimeStamp;
class Task;
typedef std::shared_ptr<Task> TaskPtr;

/* Memory request */
struct MemReq {
    Address lineAddr;
    AccessType type;
    uint32_t childId;
    MESIState state;

    //Requester id --- used for contention simulation
    uint32_t srcId;

    //Flags propagate across levels, though not to evictions
    //Some other things that can be indicated here: Demand vs prefetch accesses, TLB accesses, etc.
    enum Flag {
        IFETCH        = (1<<1), //For instruction fetches. Purely informative for now, does not imply NOEXCL (but ifetches should be marked NOEXCL)
        NOEXCL        = (1<<2), //Do not give back E on a GETS request (turns MESI protocol into MSI for this line). Used on e.g., ifetches and NUCA.
        NONINCLWB     = (1<<3), //This is a non-inclusive writeback. Do not assume that the line was in the lower level. Used on NUCA (BankDir).
        PUTX_KEEPEXCL = (1<<4), //Non-relinquishing PUTX. On a PUTX, maintain the requestor's E state instead of removing the sharer (i.e., this is a pure writeback)
        PREFETCH      = (1<<5), //Prefetch GETS access. Only set at level where prefetch is issued; handled early in MESICC
        GETS_WB       = (1<<6), //In Swarm, memory accesses may need to issue a GETS when they already have the line in M (to check agains potential speculative sharers). If so, the directory may need to reply in S (that's the whole point of having to go to the directory). With this flag (1) the requester issues a writeback and must be prepared to lose X (ie it must downgrade exclusive child beforehand), and (2) the directory may reply in S or E, but not M.
        TRACKREAD     = (1<<7), //On writebacks only. Requester still has tracked readers.
        TRACKWRITE    = (1<<8), //On writebacks only. Requester still has tracked writers.
    };
    uint32_t flags;

    // For abort handling
    const TimeStamp* ts;

    // The task that made the access resulting in this MemReq.
    // Used for conflict checking in StreamPrefetcher.
    // invariant: (!ts && !task) || (ts && task && ts == &task->cts()))
    // TODO: [mha21] Because of the invariant, ts is redundant, should remove.
    // In all places where the ts might need to be accessed, it could be
    // accessed through task->cts(). A null task would equate to a null ts. 
    const TaskPtr task;

    inline void set(Flag f) {flags |= f;}
    inline bool is (Flag f) const {return flags & f;}
    bool timestamped() const { return ts != nullptr; }

    std::ostream& operator<<(std::ostream& os) const;
    std::string toString() const;

    uint32_t bytes() const;
};

struct MemResp {
    MESIState newState;

    // Canary timestamp state: If there are outstanding speculative accesses to
    // this line, the parent must reply with the timestamp of the last one
    const TimeStamp* lastAccTS;
    inline bool timestamped() const { return lastAccTS != nullptr; }
    bool carriesData(const MemReq& req) const;

    explicit MemResp() : newState(I), lastAccTS(nullptr) {}
    std::ostream& operator<<(std::ostream& os) const;
    std::string toString() const;
    uint32_t bytes(const MemReq& req) const;
};

/* Invalidation/downgrade request */
struct InvReq {
    Address lineAddr;
    InvType type;
    uint32_t srcId; // [mcj] which class uses this? [dsm] That's the requester (core) id, see above. No users in ordspecsim for now....

    // Conflict-checking state
    // -----------------------
    const TimeStamp* ts;
    // If the following is set to a reasonable integer, don't invalidate the
    // child with that ID---it instigated the request.
    // We could avoid sending this value with the request if, before
    // invalidating children upon a GET{S,X}, a conflict detection unit
    // *removed* the instigator from the sharers list, before adding it again
    // after the invalidations complete. I fear that could really bite us in the
    // move toward non-atomic conflict detection/resolution.
    uint32_t skipChildId;

    bool timestamped() const { return ts != nullptr; }
    std::ostream& operator<<(std::ostream& os) const;
    std::string toString() const;

    uint32_t bytes() const;
};

struct InvResp {
    enum Flag {
        WRITEBACK     = (1<<1), //The invalidation of the responder induced writeback
        TRACKREAD     = (1<<2), //The responder has tracked reads to the invalidated address
        TRACKWRITE    = (1<<3), //The responder has tracked writes to the invalidated address
    };
    uint32_t flags;

    // Canary timestamp state: If the tile may have outstanding speculative
    // accesses to this line, it must reply with the timestamp of the last
    // relevant accessor (last write if INVX, last read or write if INV).
    // False positives are allowed (e.g., through Bloom filter checks).
    const TimeStamp* lastAccTS;
    inline bool timestamped() const { return lastAccTS != nullptr; }
    bool carriesData() const;

    explicit InvResp() : flags(0), lastAccTS(nullptr) {}

    inline void set(Flag f) {flags |= f;}
    inline bool is (Flag f) const {return flags & f;}
    uint32_t bytes(const InvReq& req) const;
    std::ostream& operator<<(std::ostream& os) const;
    std::string toString() const;
};

typedef AtomicPort<const MemReq, MemResp> MemPort;
typedef AtomicPort<const InvReq, InvResp> InvPort;
