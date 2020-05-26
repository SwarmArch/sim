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

#include <boost/dynamic_bitset.hpp>
#include <string>

#include "sim/canary.h"
#include "sim/conflicts/address_accessors.h"
#include "sim/conflicts/conflict_detection.h"
#include "sim/object.h"
#include "sim/stats/stats.h"
#include "sim/timestamp.h"

// TerminalCD handles conflict detection for a terminal cache (i.e. L1)
class FilterCache;

class TerminalCD : public ConflictDetectionBottom, public SimObject {
    // [mcj] Since integrating the Tile/Core-specific conflict detection with
    // AbortHandler, it's easier for the TerminalCD not to have its own
    // AddressSet for readSet_ and writeSet_, but instead dump its loads/stores
    // directly into the Tile's AddressAccessorTable. In fact this may be easier
    // in the long term, but I did prefer the encapsulation, of the TerminalCD
    // managing and detecting its own conflicts.
    AddressAccessorTable* readerTable;
    AddressAccessorTable* writerTable;

    FilterCache* cache;

    // Like in TSArray, this per-line bit is 1 if the lastAccTS was zero when
    // the line was installed, and 0 otherwise. Allows us to skip L2 accesses
    // on data that has no .
    boost::dynamic_bitset<> zeroTS;

    // Each acc bit is 1 if the line can be safely accessed without an L2 check.
    // Acc bits are set unconditionally when the line is installed, and
    // flash-cleared when we start executing a lower-timestamp task.
    // NOTE: If acc = 0 but zeroTs = 1, the line can still be safely accessed.
    struct Context {
        boost::dynamic_bitset<> acc;
        CanaryTS lastTS;
        Task* task;
        Context(uint32_t numLines)
            : acc(numLines, false), lastTS(ZERO_TS), task(nullptr) {}
    };

    std::vector<Context> ctxs;
    Context* ctx;

    Counter readerTableInstallations_;
    Counter writerTableInstallations_;
    Counter readerCellUpdates_;
    Counter writerCellUpdates_;

  public:
    TerminalCD(size_t numLines, uint32_t numContexts, std::string name);

    void setCache(FilterCache* c) {
        assert(!cache);
        cache = c;
    }

    // Bottom interface
    bool shouldSendUp(AccessType reqType, const TimeStamp* reqTs, uint32_t lineId, MESIState state) override;
    void lineInstalled(const MemReq&, const MemResp&, uint32_t, bool) override;
    void eviction(MemReq&, uint32_t) override;
    uint64_t invalidate(const InvReq&, InvResp&, int32_t, uint64_t) override;

    // Task Start/End from Core
    uint64_t startTask(Task* task, uint64_t cycle);
    uint64_t endTask(uint64_t cycle);
    void abortTask();

    // Called very frequently with multithreading, must be fast
    inline void setContext(uint32_t i) { ctx = &ctxs[i]; }
    inline void clearContext() { ctx = nullptr; }

    uint64_t recordAccess(Address, uint32_t bytes, bool isLoad, uint64_t);

    // Sigh delayed wiring
    void setAccessorTables(AddressAccessorTable* r, AddressAccessorTable* w);
    void initStats(AggregateStat*) override;

    // Adjust canaries resident in core
    void adjustCanaryOnZoomIn(const TimeStamp& frameMin,
                              const TimeStamp& frameMax);
    void adjustCanaryOnZoomOut(uint64_t maxDepth);

  private:
    uint64_t addToUndoLog(Address byteAddr, uint32_t bytes, uint64_t);
};
