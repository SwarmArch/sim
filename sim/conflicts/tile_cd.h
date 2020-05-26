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

#include <memory>
#include <vector>

#include "sim/conflicts/address_accessors.h"
#include "sim/conflicts/conflict_detection.h"
#include "sim/object.h"
#include "sim/stats/stats.h"
#include "sim/task.h"

class Cache;
class ROB;
class TSArray;
class TSB;

class TileCD : public ConflictDetectionTop,
               public ConflictDetectionBottom,
               public SimObject {
    AddressAccessorTable* readerTable_;
    AddressAccessorTable* writerTable_;
    TSArray* tsa_;
    const Cache* cache_;
    ROB* rob_;
    TSB* tsb_;

  public:
    TileCD(AddressAccessorTable* readerTable, AddressAccessorTable* writerTable,
           TSArray* tsa, std::string name);

    // Bottom interface
    bool shouldSendUp(AccessType reqType, const TimeStamp* reqTs, uint32_t lineId, MESIState state) override;
    uint64_t access(const MemReq&, uint64_t) override;
    void eviction(MemReq&, uint32_t) override;
    void lineInstalled(const MemReq&, const MemResp&, uint32_t, bool) override;
    uint64_t invalidate(const InvReq&, InvResp&, int32_t, uint64_t) override;
    void initStats(AggregateStat*) override;

    // Top interface
    void propagateLastAccTS(Address lineAddr, const TimeStamp* reqTs,
                            uint32_t lineId, const TimeStamp*& lastAccTS,
                            bool checkReaders) override;
    bool giveExclusive(const MemReq& req) const override;

    void startAbort(const TaskPtr& task, Task::AbortType type, uint32_t tileIdx,
                    uint64_t cycle);

    void setROB(ROB* rob);
    void setTSB(TSB* tsb);

    void clearReadSet(const Task& task) { readerTable_->clear(&task); }
    void clearWriteSet(const Task& task) { writerTable_->clear(&task); }
    void adjustCanariesOnZoomIn(const TimeStamp& frameMin,
                                const TimeStamp& frameMax);
    void adjustCanariesOnZoomOut(uint64_t maxDepth);

    void setCache(Cache* c) {
        assert(!cache_);
        assert(c);
        cache_ = c;
    }

    inline const Cache* getCache() const { return cache_; }

  private:
    // [mcj] The abort handler needs access to these methods that should
    // otherwise remain private.
    // It also needs access to the L2 cache of this Tile. Keeping the atomic
    // abort cascade is making this extremely hacked together.
    friend class AbortHandler;
    Accessors allAccessors(Address lineAddr, bool shouldCheckReaders) const;
    Accessors queryAllAccessors(Address lineAddr, bool shouldCheckReaders);
    bool anyReaders(Address lineAddr) const;
    bool anyWriters(Address lineAddr) const;
    bool isReader(const Task* t, Address lineAddr);
    bool isWriter(const Task* t, Address lineAddr);

    uint64_t abortOrderViolators(const Accessors& accessors,
                                 const TimeStamp& ts,
                                 uint64_t cycle);
    void finishAbort(const TaskPtr& task, uint64_t cycle);
    void scheduleFinishAbort(TaskPtr, uint64_t);
    void scheduleStartAbort(TaskPtr, Task::AbortType, uint64_t);

    // [ssub] These three stats do not capture all bloom filter
    // checks for now. See AbortHandler::abortFutureTasks(...).
    // This should change once the AbortHandler is cleaned up.
    Counter tileChecks_;
    Counter allGlobalChecks_;
    Counter hitGlobalChecks_;

    Counter readTableChecks_;
    Counter writeTableChecks_;
};
