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

#include "sim/conflicts/tile_cd.h"

#include "sim/conflicts/address_accessors.h"
#include "sim/conflicts/terminal_cd.h"
#include "sim/assert.h"
#include "sim/log.h"
#include "sim/rob.h"
#include "sim/task.h"
#include "sim/timestamp.h"
#include "sim/tsarray.h"
#include "sim/tsb.h"
#include "sim/driver.h"
#include "sim/core/core.h"

#include <tuple>

#undef DEBUG
#define DEBUG(args...) //info(args)

TileCD::TileCD(AddressAccessorTable* readerTable,
               AddressAccessorTable* writerTable, TSArray* tsa,
               std::string name)
    : SimObject(name),
      readerTable_(readerTable),
      writerTable_(writerTable),
      tsa_(tsa),
      cache_(nullptr),
      rob_(nullptr),
      tsb_(nullptr) {
    assert(readerTable_ && writerTable_);
    assert(tsa_);
}

bool TileCD::shouldSendUp(AccessType reqType, const TimeStamp* reqTs, uint32_t lineId, MESIState state) {
    if (!IsGet(reqType) || !reqTs) return false;

    bool isMiss = !IsValid(state) || (reqType == GETX && !IsExclusive(state));

    if (isMiss) {
        // Need a global conflict check, up-propagate the timestamped request
        // FIXME(dsm): This doesn't incur a timestamp check, why does it increase the counter?
        allGlobalChecks_.inc();
        return true;
    } else if (tsa_->getTS(lineId) < *reqTs || tsa_->isZeroTS(lineId)) {
        // The request is from a task that *strictly* succeeds the task that
        // installed the canary. The canary's installer aborted all global
        // future writers, so a tile-level conflict check is sufficient.
        // If the canary is zero, an irrevocable installed it, and aborted
        // future speculative accessors. Importantly, irrevocable tasks can
        // access data installed by other irrevocable tasks.
        tileChecks_.inc();
        return false;
    } else {
        // The request's task does not strictly succeed the canary, so a global
        // conflict check is required.
        hitGlobalChecks_.inc();
        allGlobalChecks_.inc();
        return true;
    }
}

uint64_t TileCD::access(const MemReq& req, uint64_t cycle) {
    // [mcj] Temporarily disable aborts here
    if (!req.timestamped() || !IsGet(req.type)) {
        return cycle;
    } else {
        uint64_t respCycle = cycle;
        // FIXME(mcj) disable aborts for now.
        if (false) {
            bool shouldCheckReaders = (req.type == GETX);
            Accessors accessors = allAccessors(req.lineAddr, shouldCheckReaders);
            // TODO(mcj)
            // respCycle += BF_CHECK_LATENCY;
            respCycle = abortOrderViolators(accessors, *req.ts, respCycle);
        }
        return respCycle;
    }
}

void TileCD::eviction(MemReq& req, uint32_t lineId) {
    assert(IsPut(req.type) || req.is(MemReq::GETS_WB));
    if (readerTable_->anyAccessors(req.lineAddr)) req.set(MemReq::TRACKREAD);
    if (writerTable_->anyAccessors(req.lineAddr)) req.set(MemReq::TRACKWRITE);
}

void TileCD::lineInstalled(const MemReq& req, const MemResp& resp,
                           uint32_t lineId, bool wasGlobalCheck) {
    if (!req.timestamped() || !IsGet(req.type) || !wasGlobalCheck) {
        // If tile-only conflict check was sufficient, there is no need to
        // update the canary timestamp.
        assert(!resp.timestamped());
    } else {
        if (resp.timestamped()) {
            // ZERO_TS implies committed data. No tracked access should have ZERO_TS
            assert(rob_->GVT() <= *resp.lastAccTS);
            assert(*resp.lastAccTS <= *req.ts || *req.ts == IRREVOCABLE_TS);
            // ^ Ideally this would be
            //assert(*resp.lastAccTS < *req.ts
            //       || *req.ts == IRREVOCABLE_TS
            //       || resp.isIrrevocable());
            // But that needs a reduction of isIrrevocable for the responses.
            tsa_->setTS(lineId, *resp.lastAccTS, rob_->GVT());
        } else {
            tsa_->clearTS(lineId);
        }
        DEBUG("[tilecd-%d] line 0x%lx lineId %d set canary = %s | %s %s -> %s ts %s",
              rob_->getIdx(),
              req.lineAddr, lineId,
              tsa_->getTS(lineId).toString().c_str(), AccessTypeName(req.type),
              MESIStateName(req.state), MESIStateName(resp.newState),
              req.ts->toString().c_str());
    }

    // Enable to check that the line will be installed in a valid state w.r.t. other lines
#if 0
    auto getTileCD = [](uint32_t tileIdx) {
        return ossinfo->abortHandler->getTileCD(tileIdx);
    };
    uint32_t numTiles = ossinfo->numROBs;

    TimeStamp canary = tsa_->getTS(lineId);
    for (uint32_t tileIdx = 0; tileIdx < numTiles; tileIdx++) {
        const TileCD* tcd = getTileCD(tileIdx);
        if (tcd == this) continue;

        bool checkReaders = IsExclusive(resp.newState);
        auto accessors = tcd->allAccessors(req.lineAddr, checkReaders);
        for (const Task* t : accessors) {
            if (!t->hasPendingAbort() && t->cts() > canary) {
                bool writer = tcd->allAccessors(req.lineAddr, false).count(t);

                //auto sdall = HackGetLLCSharers(req.lineAddr, true);
                //auto sdws = HackGetLLCSharers(req.lineAddr, false);
                panic(
                    "[%s] Wrong canary %s on line %lx (response %s): Task "
                    "%s @ tile %d conflicts (writer %d stickyAll %d "
                    "stickyWriter %d) req %s",
                    "tcd", canary.toString().c_str(),
                    req.lineAddr, resp.toString().c_str(),
                    t->toString().c_str(), tileIdx, writer,
                    false, //(bool)sdall[tileIdx],
                    false, //(bool)sdws[tileIdx],
                    req.toString().c_str());
            }
        }
    }
#endif
}

uint64_t TileCD::invalidate(const InvReq& req, InvResp& resp, int32_t lineId,
                            uint64_t cycle) {
    assert(req.type == INV || req.type == INVX);
    uint64_t respCycle = cycle;
    // FIXME(mcj) disable aborts for now.
    if (false) {
        bool shouldCheckReaders = (req.type == INV);
        Accessors accessors = allAccessors(req.lineAddr, shouldCheckReaders);
        // TODO(mcj)
        // respCycle += BF_CHECK_LATENCY;
        respCycle = abortOrderViolators(accessors, *req.ts, respCycle);
    }

    // Notify the parent if this tile still has speculative accessors
    // NOTE(dsm): If this invalidation has aborted all readers, TRACKREAD
    // won't be set because read sets are cleared immediately. But aborted
    // writers retain their lines locked till they finish aborting, so
    // TRACKWRITE will be set even if all writers have been aborted.
    if (anyReaders(req.lineAddr)) resp.set(InvResp::TRACKREAD);
    if (anyWriters(req.lineAddr)) resp.set(InvResp::TRACKWRITE);

    resp.lastAccTS = nullptr;
    if (req.timestamped()) {
        // Find the highest timestamp of the last accessor that precedes or
        // matches the request (reader or writer if INV, writer if INVX)
        bool checkReaders = (req.type == INV);
        Accessors accessors = allAccessors(req.lineAddr, checkReaders);
        const Task* lbTask = nullptr;
        for (const Task* t : accessors) {
            // [mcj] At this point there should be a total order among tasks.
            // The AbortHandler is responsible to call notifyDependence().
            assert(t->cts() != *req.ts || t->isIrrevocable());
            if (t->cts() <= *req.ts) {
                if (!lbTask || t->lts() > lbTask->lts()) lbTask = t;
            }
        }
        if (lbTask) {
            assert(lbTask->cts() <= *req.ts);
            assert(lbTask->lts() < *req.ts ||
                   (lbTask->isIrrevocable() &&
                    (lbTask->lts() == *req.ts || IRREVOCABLE_TS == *req.ts)));
            // Canaries must store logical timestamps.
            // If we stored IRREVOCABLE_TS as the canary, it would be
            // overwritten immediately as it precedes the GVT
            // (see // TSArray::setTS)
            // Keeping the lts() as a canary ensures that we detect dependences
            // on the irrevocable up until it commits.
            //
            // TODO(mcj) this doesn't pass...
            //assert(!lbTask->isIrrevocable() || lbTask->lts().hasTieBreaker());
            resp.lastAccTS = &lbTask->lts();
        }
    }

    return respCycle;
}

void TileCD::propagateLastAccTS(Address lineAddr, const TimeStamp* reqTs,
                                uint32_t lineId, const TimeStamp*& lastAccTS,
                                bool checkReaders) {
    // FIXME(dsm): Using lack of a timestamp to short-circuit exits everywhere
    // is eventually going to bite us when we mix speculative and
    // non-speculative requests. Not being timestamped should be considered
    // equivalent to ts == 0,0.
    if (!reqTs) return;

    auto propagate = [&lastAccTS](const TimeStamp& ts) {
        if (!lastAccTS || *lastAccTS < ts) lastAccTS = &ts;
    };

    // Propagate canary timestamp, if any (covers all remote accessors)
    if (!tsa_->isZeroTS(lineId)) {
        const TimeStamp& canary = tsa_->getTS(lineId);
        if (canary >= rob_->GVT()) propagate(canary);
    }

    // Propagate highest timestamp of local accessor
    Accessors accessors = allAccessors(lineAddr, checkReaders);
    for (const Task* t : accessors) {
        // FIXME(dsm): When AbortHandler is gone, this should also use
        // notifyDependence() to do lazy tiebreaking and stalling among tasks
        // of the same tile. In fact, there was code here to that effect.
        // However, reqTs is insufficient---we also need the task pointer of
        // the requester. Otherwise, we can tiebreak with/stall ourselves!
        if (t->cts() <= *reqTs) propagate(t->lts());
    }

#if 0
    // Sanity check: There shoudln't be any future writers at this point (this
    // is only a partial check; if we're responding to a write, there also
    // should not be any future readers).
    // FIXME: Remove this once coalescer/splitter tasks are handled correctly
    Accessors writers = writerTable_->allAccessors(lineAddr);
    for (const Task* t: writers) {
        if (t->cts() > *reqTs && !t->hasPendingAbort()) {
            panic("Future writer without pending abort %lx reqTs %s writer %s",
                  lineAddr, reqTs->toString().c_str(), t->toString().c_str());
        }
    }
#endif
}

bool TileCD::giveExclusive(const MemReq& req) const {
    assert(req.type == GETS);
    // If it's timestamped, use MSI for now --- MESI hits strange corner cases
    // for lines in E (NOTE: To use MESI, you must check there are no future
    // readers, see previous code)
    return !req.timestamped();
}

/**
 * Abort events are broken into two parts: start and finish.
 * The AbortHandler schedules start events
 * start events schedule finish events
 * A finish event may find that a higher-priority start event needs to be
 * scheduled.
 *
 * This is expected to be called at exactly the cycle when the requested task is
 * expected to start its abort, i.e. not as part of a large atomic abort cascade.
 */
void TileCD::startAbort(const TaskPtr& task, Task::AbortType type,
                        uint32_t tileIdx, uint64_t cycle) {
    assert(task);
    assert(tileIdx == rob_->getIdx());
    assert(!task->hasContainer() || rob_ == task->container());

    // Because the AbortHandler's atomic abort cascade clears read sets, then it
    // should already be empty here. Ideally the clear would be delayed until
    // now, but the simulator isn't there yet.
    // Tasks with an exception don't get their readsets cleared
    assert(task->idle() || type == Task::AbortType::EXCEPTION ||
           !readerTable_->anyAddresses(task.get()));

    uint64_t finishCycle = cycle;
    if (task->hasContainer()) finishCycle = rob_->startAbort(task, cycle);
    if (finishCycle == cycle) finishAbort(task, cycle);
    else scheduleFinishAbort(task, finishCycle);
}


void TileCD::finishAbort(const TaskPtr& task, uint64_t cycle) {
    assert(task);
    Task::AbortType type = task->abortTypeBefore(cycle);

    assert(!task->idle() || (
           type == Task::AbortType::PARENT
           && !readerTable_->anyAddresses(task.get())
           && !writerTable_->anyAddresses(task.get())));
    assert(task->idle()
           || type == Task::AbortType::EXCEPTION
           || !readerTable_->anyAddresses(task.get()));

    writerTable_->clear(task.get());

    if (task->hasContainer()) {
        // Clean up local tile state for the task.
        rob_->finishAbort(task, type);

        uint64_t nextCycle = task->nextAbortCycle();
        assert(cycle <= nextCycle);
        if (nextCycle < UINT64_MAX) {
            // Schedule the next pending abort for this task
            Task::AbortType nexttype = task->abortTypeBefore(nextCycle);
            assert((uint32_t) nexttype > (uint32_t) type);
            scheduleStartAbort(task, nexttype, nextCycle);
        }
    } else {
        // The task hasn't made it to a task queue yet and lives in the
        // local TSB.
        assert(type == Task::AbortType::PARENT);
        tsb_->abort(task);
    }
}


void TileCD::scheduleFinishAbort(TaskPtr task, uint64_t cycle) {
    Event* ev = schedEvent(cycle, [this,task] (uint64_t cycle) {
            this->finishAbort(task, cycle);
    });
    task->recordFinishAbortEvent(ev);
}


void TileCD::scheduleStartAbort(TaskPtr task, Task::AbortType type,
                                uint64_t cycle) {
    if (task->isAborting(cycle)) return;
    // For now...
    assert(task->hasContainer());
    Event* ev = schedEvent(cycle, [this, task, type] (uint64_t cycle) {
        this->startAbort(task, type, this->rob_->getIdx(), cycle);
    });

    DEBUG("Scheduled abort event %p on task %s, cycle: %lu",
          ev, task->toString().c_str(), cycle);
    task->recordStartAbortEvent(ev);
}


Accessors TileCD::allAccessors(Address lineAddr,
                               bool shouldCheckReaders) const {
    DEBUG("[%s] conflict check 0x%lx checkreaders %s",
          name(), lineAddr,
          shouldCheckReaders ? "true" : "false");

    //totalBloomFilterChecks_.inc();
    Accessors accessors = writerTable_->allAccessors(lineAddr);
    if (shouldCheckReaders) {
        // Union the readers and writers
        Accessors readers = readerTable_->allAccessors(lineAddr);
        accessors.insert(readers.begin(), readers.end());
    }

    // [mcj] Hopefully relevant.
    // https://blog.knatten.org/2011/08/26/dont-be-afraid-of-returning-by-value-know-the-return-value-optimization/
    return accessors;
}

Accessors TileCD::queryAllAccessors(Address lineAddr,
                               bool shouldCheckReaders) {
    if (shouldCheckReaders) readTableChecks_.inc();
    writeTableChecks_.inc();
    return allAccessors(lineAddr, shouldCheckReaders);
}

bool TileCD::anyReaders(Address lineAddr) const {
    return readerTable_->anyAccessors(lineAddr);
}

bool TileCD::anyWriters(Address lineAddr) const {
    return writerTable_->anyAccessors(lineAddr);
}

bool TileCD::isReader(const Task* t, Address lineAddr) {
    return readerTable_->test(t, lineAddr);
}

bool TileCD::isWriter(const Task* t, Address lineAddr) {
    return writerTable_->test(t, lineAddr);
}


/**
 * Abort data-dependence-violating tasks in the local commit queue
 */

uint64_t TileCD::abortOrderViolators(const Accessors& accessors,
                                     const TimeStamp& ts,
                                     uint64_t cycle) {
    DEBUG("[%s] abort after checking %ld accessors", name(), accessors.size());

    panic("unimplemented");

    uint64_t maxAbortCycle = cycle;
    for (const Task* task : accessors) {
        if (task->cts() > ts) {
            uint64_t aCycle = 0;
            // dsm: This code was wrong. It was calling access(), which would
            // have performed a finishAbort(). This code needs to incorporate
            // what is now in AbortHandler::abortIfSuccessor().
            maxAbortCycle = std::max(maxAbortCycle, aCycle);
        }
        //timestampChecks_.inc();
    }
    return maxAbortCycle;
}

void TileCD::setROB(ROB* rob) {
    assert(rob);
    assert(!rob_);
    rob_ = rob;
    rob_->setTileCD(this);
}

void TileCD::setTSB(TSB* tsb) {
    assert(tsb);
    assert(!tsb_);
    tsb_ = tsb;
}

void TileCD::initStats(AggregateStat* parentStat) {
    auto addStat = [&](Counter& cs, const char* name, const char* desc) {
        cs.init(name, desc);
        parentStat->append(&cs);
    };
    addStat(tileChecks_, "tileChecks", "Tile-local conflict checks");
    addStat(allGlobalChecks_, "allGlobalChecks",
            "Global conflict checks (spawned from this tile)");
    addStat(hitGlobalChecks_, "hitGlobalChecks",
            "Global conflict checks despite a hit in the L2");
    addStat(readTableChecks_, "readTableChecks", "Read address accessor table conflict checks");
    addStat(writeTableChecks_, "writeTableChecks", "Write address accessor table conflict checks");
}

void TileCD::adjustCanariesOnZoomIn(const TimeStamp& frameMin,
                                    const TimeStamp& frameMax) {
    tsa_->adjustOnZoomIn(frameMin, frameMax);
}

void TileCD::adjustCanariesOnZoomOut(uint64_t maxDepth) {
    tsa_->adjustOnZoomOut(maxDepth);
}
