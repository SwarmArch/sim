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

#include "sim/conflicts/terminal_cd.h"

#include "sim/assert.h"
#include "sim/conflicts/address_accessors.h"
#include "sim/log.h"
#include "sim/memory/filter_cache.h"
#include "sim/rob.h"
#include "sim/sim.h"
#include "sim/task.h"

#include "pin/pin.H"

#undef DEBUG
#define DEBUG(args...) //info(args)

TerminalCD::TerminalCD(size_t numLines, uint32_t numContexts, std::string name)
    : SimObject(name),
      readerTable(nullptr),
      writerTable(nullptr),
      cache(nullptr),
      zeroTS(numLines, false),
      ctxs(numContexts, numLines),
      ctx(nullptr) {}

// Bottom interface
bool TerminalCD::shouldSendUp(AccessType reqType, const TimeStamp* reqTs,
                              uint32_t lineId, MESIState state) {
    assert(IsGet(reqType));
    if (!reqTs) return false;
    assert(ctx);
    return !ctx->acc[lineId] && !zeroTS[lineId];
}

void TerminalCD::lineInstalled(const MemReq& req, const MemResp& resp,
                               uint32_t lineId, bool) {
    assert(IsGet(req.type));
    assert(static_cast<size_t>(lineId) <= zeroTS.size());
    // NOTE: Valid whether this was a timestamped request or not
    assert(ctx);
    assert(req.type == GETX || !zeroTS[lineId]);
    assert(req.type == GETX || !ctx->acc[lineId]);
    zeroTS[lineId] = (!resp.timestamped() || *resp.lastAccTS == ZERO_TS);
    ctx->acc[lineId] = true;
}

void TerminalCD::eviction(MemReq& req, uint32_t lineId) {
    zeroTS[lineId] = false;
    for (Context& c : ctxs) c.acc[lineId] = false;
}

uint64_t TerminalCD::invalidate(const InvReq& req, InvResp& resp,
                                int32_t lineId, uint64_t cycle) {
    DEBUG("[%s] Invalidate %s cycle %lu",
          name(), req.toString().c_str(), cycle);
    assert(req.type == INVX || req.type == INV);

    if (req.type == INV && lineId >= 0) {
        zeroTS[lineId] = false;
        for (Context& c : ctxs) c.acc[lineId] = false;
    }
    return cycle;
}

uint64_t TerminalCD::recordAccess(Address byteAddr, uint32_t bytes, bool isLoad,
                                  uint64_t cycle) {
    uint64_t respCycle = cycle;
    Task* task = ctx->task;

    // NOTE: Access may not be word-aligned, but fits into a single cache
    // line (the core splits line-crossing accesses)
    Address lineAddr = byteAddr >> ossinfo->lineBits;
    if (isLoad) {
        if (task) {
            readerTable->update(task, lineAddr);
            readerCellUpdates_.inc();
        }
    } else {
        int32_t lineId = cache->probe(lineAddr);
        assert(lineId >= 0);
        assert(cache->getState(lineId) == M);

        // NOTE(dsm): All writes must invalidate zeroTS. Otherwise, a
        // predecessor task that runs later in the same core will see an L1 hit
        // and will not conflict-check against this write (which would abort
        // this task).
        // FIXME(dsm): Double lookup!

        // NOTE: The first speculative write on a line with no other speculative
        // accessors (readers or writers, since we have it in M) must set acc to
        // true. Subsequent speculative writes should see acc already true.
        assert(!task || zeroTS[lineId] || ctx->acc[lineId]);
        ctx->acc[lineId] = true;
        zeroTS[lineId] = false;
        for (Context& c: ctxs) if (&c != ctx) c.acc[lineId] = false;

        if (task) {
            writerTable->update(task, lineAddr);
            writerCellUpdates_.inc();
            if (!task->isIrrevocable()) {
                // Irrevocable tasks can't abort, but their read/write sets are
                // used to force an order with dependent speculative tasks.
                respCycle = addToUndoLog(byteAddr, bytes, cycle);
            }
        }
    }
    return respCycle;
}

void TerminalCD::setAccessorTables(AddressAccessorTable* r,
                                   AddressAccessorTable* w) {
    assert(r && w);
    assert(!readerTable && !writerTable);
    readerTable = r;
    writerTable = w;
}

uint64_t TerminalCD::startTask(Task* task, uint64_t cycle) {
    assert(ctx);
    assert(!ctx->task);
    assert(!readerTable->anyAddresses(task));
    assert(!writerTable->anyAddresses(task));
    ctx->task = task;
    if (task->cts() < ctx->lastTS) {
        DEBUG("[%s] %s->%s Decreasing timestamp, clearing access bits", name(),
              ctx->lastTS.toString().c_str(), task->cts().toString().c_str());
        ctx->acc.reset();
    }
    ctx->lastTS = task->cts();
    return cycle;
}

uint64_t TerminalCD::endTask(uint64_t cycle) {
    Task* task = ctx->task;
    assert(task);
    // Always tiebreak a finishing task. The ROB will give it a tiebreaker
    // anyway, and when we don't flush, we introduce potential dependences
    // between a task and its successor. This way, the succeeding task never
    // has the same timestamp.

    task->container()->notifyDependence(task->ptr(), task->cts());
    assert(ctx->lastTS >= task->cts());
    ctx->lastTS = task->cts();
    // [mcj] Note the read/write sets are already in the parent's commit queue.
    if (readerTable->anyAddresses(task)) readerTableInstallations_.inc();
    if (writerTable->anyAddresses(task)) writerTableInstallations_.inc();
    ctx->task = nullptr;
    return cycle;
}

void TerminalCD::abortTask() {
    assert(ctx->task);
    ctx->task = nullptr;
}

uint64_t TerminalCD::addToUndoLog(Address byteAddr, uint32_t bytes, uint64_t cycle) {
    uint64_t respCycle = cycle;
    assert(ctx->task);
    assert(bytes);
    // Log in 64-bit chunks. endAddr is inclusive, and may be the same as startAddr.
    // This code handles both unaligned and wide accesses.
    // Masking is only possible is uint64_t's are a power-of-2 bytes, so check :)
    static_assert((sizeof(uint64_t) & (sizeof(uint64_t) - 1)) == 0,
            "Enjoying your PDP-8? :)");
    constexpr uint64_t u64mask = ~((uint64_t)(sizeof(uint64_t) - 1));
    Address startAddr = byteAddr & u64mask;
    Address endAddr = (byteAddr + bytes - 1) & u64mask;
    assert(startAddr <= endAddr); // check for overflow

    for (Address a = startAddr; ; a += sizeof(uint64_t)) {
        // SafeCopy does not trigger a segfault if the address is invalid
        uint64_t val;
        size_t copiedBytes = PIN_SafeCopy(&val, (void*)a, sizeof(uint64_t));

        // Check if the application is writing to writeable memory.
        // Misspeculating tasks may perform indirect writes to bad locations.
        size_t testWriteBytes = PIN_SafeCopy((void*)a, &val, sizeof(uint64_t));
        if (testWriteBytes != sizeof(uint64_t)) {
            DEBUG("Write to invalid address 0x%lx copiedBytes %ld", a, copiedBytes);
            assert(testWriteBytes == 0);
            // [dsm] No need to do anything; we'll catch the SIGSEGV and deal
            // with it when the instrumented code performs the write (we could
            // do something here, but we also need to catch load SIGSEGVs,
            // SIGBUS, SIGILL, etc).
        } else {
            assert(copiedBytes == sizeof(uint64_t)); (void)copiedBytes;

            DEBUG("[%s] Add (0x%lx,%ld) to undolog", name(), a, val);
            Address undoLogAddr = ctx->task->undoLog.push(a, val);

            // Issue a non-conflict-checked store to the undo log
            // [mcj; requested by dsm] Don't update the respCycle. This assumes
            // that any outstanding store requests to the undo log don't need to
            // block the instigating store request.
            cache->store(undoLogAddr, cycle, nullptr);
        }

        // Carefully exit the loop before overflow, if endAddr is the max value.
        if (a == endAddr) break;
    }
    return respCycle;
}

void TerminalCD::adjustCanaryOnZoomIn(const TimeStamp& frameMin,
                                      const TimeStamp& frameMax) {
    for (Context& ctx : ctxs) {
        ctx.lastTS.adjustOnZoomIn(frameMin, frameMax);
        if (ctx.lastTS == INFINITY_TS) {
            DEBUG("[%s] Timestamp went out of range, clearing access bits",
                  name());
            ctx.acc.reset();
        }
    }
}

void TerminalCD::adjustCanaryOnZoomOut(uint64_t maxDepth) {
    for (Context& ctx : ctxs)
        ctx.lastTS.adjustOnZoomOut(maxDepth);
}

void TerminalCD::initStats(AggregateStat* parentStat) {
    auto addStat = [&](Counter& cs, const char* name, const char* desc) {
        cs.init(name, desc);
        parentStat->append(&cs);
    };
    addStat(readerTableInstallations_, "readerTableInstallations",
            "Number of read address sets (i.e. rows) inserted into tile CQ "
            "(reader address accessor table)");
    addStat(writerTableInstallations_, "writerTableInstallations",
            "Number of write address sets (i.e. rows) inserted into tile CQ "
            "(writer address accessor table)");
    addStat(readerCellUpdates_, "readerCellUpdates",
            "Number of cell updates to core's reader address set");
    addStat(writerCellUpdates_, "writerCellUpdates",
            "Number of cell updates to core's writer address set");
}
