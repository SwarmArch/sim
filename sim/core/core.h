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

#ifndef CORE_H_
#define CORE_H_

#include <stdint.h>
#include <string>

#include "sim/match.h"
#include "sim/memory/filter_cache.h"
#include "sim/stats/breakdown_stat.h"
#include "sim/stats/stats.h"
#include "sim/stats/table_stat.h"
#include "sim/timestamp.h"
#include "sim/types.h"
#include "sim/core/decoder.h"
#include "core_context.h"

class Cache;
class FilterCache;
class Task;
class CoreContext;
class TerminalCD; // FIXME(mcj) too much of the interface

namespace rob {
enum class Stall;
}

class Core {
    public:
        // The Core::State is public because leave(...) can be called for
        // different reasons
        // FIXME(mcj) does IDLE imply in a syscall? Should I rename it? [dsm] No, it's broader than in a syscall, eg no thread
        enum class State {IDLE, EXEC, EXEC_TOABORT,
            STALL_EMPTY, STALL_RESOURCE, STALL_THROTTLE, STALL_NACK,
        };

        static State stallToState(rob::Stall);

    protected:
        const std::string name;
        const uint32_t cid;

        // Memory & conflict-detection state
        FilterCache* l1d;
        // FIXME(mcj) seems wrong to have the entire class interface here.
        TerminalCD* cd;

        /* Timing state
         *
         * To support out-of-order aborts, this core model adopts epoch-based
         * counting. The driver of this core calls bbl(), load(), and store()
         * to drive the timing model. These store the memory cycles and
         * instructions over curCycle and instrs in temporary variables.
         * Periodically, the driver calls closeEpoch(), and those are
         * incorporated into curCycle and the various stats. Aborts are
         * supported on any cycle between curCycle and getCycle().
         */
        uint64_t curCycle;
        uint64_t instrs;

        uint64_t epochMemCycles;
        uint64_t epochInstrs;

        // FIXME: Delete TaskType, it's redundant. We already have a pointer to
        // the task, which has all the info we need.
        CoreContext::TaskType taskType;
        State state;

        // Task state
        TaskPtr curTask;  // not const b/c stores add to the log

        // Stats
        AggregateStat* coreStats; // Can inherit and add stuff

    private:
        using TaskType = CoreContext::TaskType;
        // [mcj] These should have been in CoreContext all along. In fact, there
        // is so much of the simple core that should be in CoreContext. Since
        // there is little design of Core/SbCore/CoreContext, I'll just hack
        // more in.
        BreakdownStat allCycles, allInstrs;
        TableStat committedCycles, committedInstrs;
        TableStat abortedDirectCycles, abortedDirectInstrs;
        TableStat abortedIndirectCycles, abortedIndirectInstrs;
    protected:
        Counter taskStarts, taskFinishes, runningTaskAborts;
        Counter branches, mispredicts;
    private:
        BblInfo* bbl_;

    public:
        Core(const std::string& _name, uint32_t _cid, FilterCache* _l1d);

        inline uint32_t getCid() const { return cid; }
        virtual inline uint64_t getCycle() const {
            return curCycle + epochMemCycles + epochInstrs;
        }
        virtual inline uint64_t getCycle(ThreadID) const { return getCycle(); }

        void bbl(uint32_t instrs); /* TODO: with more sophisticated core models, pass BBL info object */

        virtual bool recordBbl(BblInfo* bblInfo, ThreadID tid) {
            bbl_ = bblInfo;
            return true;
        }
        virtual bool simulate() { return true; }

        virtual bool load(Address addr, uint32_t size, ThreadID tid);
        virtual bool load2(Address addr, uint32_t size, ThreadID tid);
        virtual bool store(Address addr, uint32_t size, ThreadID tid);

        virtual void handleMagicOp(uint32_t op, ThreadID tid) {};

        virtual void join(uint64_t cycle, ThreadID tid );
        virtual void leave(Core::State, ThreadID tid);
        virtual void startTask(const TaskPtr& task, ThreadID tid);
        virtual void finishTask(ThreadID tid);
        virtual void waitTillMemComplete(ThreadID tid) {};

        virtual void abortTask(uint64_t cycle, ThreadID tid, bool isPriv);

        // FIXME(mcj) closeEpoch shouldn't be publicly called in driver.cpp. All
        // it does is reset state that probably could be achieved in
        // Core::join()?
        virtual void closeEpoch();

        virtual void initStats(AggregateStat* parentStat);

        virtual uint64_t getInstructionCount() const { return allInstrs.total(); }

        virtual void issueHead(ThreadID tid) { bbl(bbl_->instrs); };

        // FIXME(mcj) This is only virtual so that the multithreaded cores can
        // hackily interact with TerminalCD. If TerminalCD must be
        // multithreading-aware, why don't we just pass the context ID to all
        // TerminalCD methods? Why do this cd->clearContext, cd->setContext
        // dance every time? That seems more error-prone
        virtual void setTerminalCD(TerminalCD*);
        TerminalCD* getTerminalCD() const { return cd; }
        virtual uint32_t getCtxId(ThreadID tid) const { return 0; }

        // FIXME(mcj) assert(false) => this shouldn't be part of the interface
        virtual void unblockWaiter(ThreadID tid) { assert(false); }

        inline const Cache* getDataCache() const { return l1d; }
        virtual bool isAvailableThread(ThreadID tid) const { return true; }
        virtual uint64_t getAbortedInstrs() const { return 0; }
        virtual uint64_t getStallSbCycles() const { return 0; }
        virtual uint64_t getCommittedInstrs() const { return 0; }
        virtual uint64_t getTotalCycles() const { return 0; }
        virtual uint64_t getNumAvailableThreads() const { return 1ul; }
        virtual std::pair<bool, ThreadID> increaseAvailableThreads() { return std::make_pair(false, INVALID_TID); }
        virtual std::pair<bool, ThreadID> decreaseAvailableThreads() { return std::make_pair(false, INVALID_TID); }

        virtual uint64_t pipelineFlushPenalty() {
            return 5;
        }

    protected:
        // cycleData is input/output. cycleConflicts is output only
        template <bool isLoad, bool lineCrossingsGoParallel>
        bool access(Address addr, uint32_t size, ThreadID tid, uint64_t* cycleData, uint64_t* cycleConflicts);

    private:
        bool needConflictCheck(Address addr, ThreadID tid) const;
        void transition(Core::TaskType, Core::State);
};
#endif
