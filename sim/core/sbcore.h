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

#ifndef SBCORE_H_
#define SBCORE_H_

#include <string>
#include <stdint.h>

#include "core.h"
#include "../thread.h"
#include "sim/spin_cv.h"
#include <unordered_map>

template<uint32_t NB, uint32_t HB, uint32_t LB>
class BranchPredictorPAg {
    private:
        uint32_t bhsr[1 << NB];
        uint8_t pht[1 << LB];

    public:
        BranchPredictorPAg() {
            uint32_t numBhsrs = 1 << NB;
            uint32_t phtSize = 1 << LB;

            for (uint32_t i = 0; i < numBhsrs; i++) {
                bhsr[i] = 0;
            }
            for (uint32_t i = 0; i < phtSize; i++) {
                pht[i] = 1;  // weak non-taken
            }

            static_assert(LB <= HB, "Too many PHT entries");
            static_assert(LB >= NB, "Too few PHT entries (you'll need more XOR'ing)");
        }

        // Predicts and updates; returns false if mispredicted
        inline bool predict(Address branchPc, bool taken) {
            uint32_t bhsrMask = (1 << NB) - 1;
            uint32_t histMask = (1 << HB) - 1;
            uint32_t phtMask  = (1 << LB) - 1;

            // Predict
            // uint32_t bhsrIdx = ((uint32_t)( branchPc ^ (branchPc >> NB) ^ (branchPc >> 2*NB) )) & bhsrMask;
            uint32_t bhsrIdx = ((uint32_t)( branchPc >> 1)) & bhsrMask;
            uint32_t phtIdx = bhsr[bhsrIdx];

            // Shift-XOR-mask to fit in PHT
            phtIdx ^= (phtIdx & ~phtMask) >> (HB - LB); // take the [HB-1, LB] bits of bshr, XOR with [LB-1, ...] bits
            phtIdx &= phtMask;

            // If uncommented, behaves like a global history predictor
            // bhsrIdx = 0;
            // phtIdx = (bhsr[bhsrIdx] ^ ((uint32_t)branchPc)) & phtMask;

            bool pred = pht[phtIdx] > 1;

            // info("BP Pred: 0x%lx bshr[%d]=%x taken=%d pht=%d pred=%d", branchPc, bhsrIdx, phtIdx, taken, pht[phtIdx], pred);

            // Update
            pht[phtIdx] = taken? (pred? 3 : (pht[phtIdx]+1)) : (pred? (pht[phtIdx]-1) : 0); //2-bit saturating counter
            bhsr[bhsrIdx] = ((bhsr[bhsrIdx] << 1) & histMask ) | (taken? 1: 0); //we apply phtMask here, dependence is further away

            // info("BP Update: newPht=%d newBshr=%x", pht[phtIdx], bhsr[bhsrIdx]);
            return (taken == pred);
        }
};


class SbCore : public Core {
    protected:
        std::vector<CoreContext*> ctxts_;
        std::deque<ThreadID> availableThreads;
        std::deque<ThreadID> unavailableThreads;

        // Load/store buffer are priority queues thet record first available
        // cycle. If nullptr, infinite buffers are simulated.
        CycleQueue* loadBuffer;
        CycleQueue* storeBuffer;

        uint64_t curCycleIssuedUops;

        // To prevent this being passed around
        CoreContext* curCtx;

        Counter sbInstrs;

        const uint32_t startAvailableThreads; // relevant for throttler

    public:
        enum IssuePriority { ROUND_ROBIN, TIMESTAMP, START_ORDER, SAI, FIXED, TS_FP, ICOUNT, TS_ICOUNT };

        SbCore(const std::string& _name, uint32_t _cid,
               uint32_t numctxts, FilterCache* _l1d,
               IssuePriority priority, uint64_t issueWidth, uint64_t mispredictPenalty,
               uint32_t loadBufferEntries, uint32_t storeBufferEntries, uint32_t startThreads);

        inline uint64_t getCycle() const override { return curCycle; }
        inline uint64_t getCycle(ThreadID tid) const override {
            return context(tid)->nextIssueCycle;
        }

        bool load(Address addr, uint32_t size, ThreadID tid) override;
        bool load2(Address addr, uint32_t size, ThreadID tid) override;
        bool store(Address addr, uint32_t size, ThreadID tid) override;
        void handleMagicOp(uint32_t op, ThreadID tid) override;

        void join(uint64_t cycle, ThreadID tid) override;
        void leave(Core::State, ThreadID tid) override;
        void startTask(const TaskPtr& task, ThreadID tid) override;
        virtual void finishTask(ThreadID tid) override;

        bool recordBbl(BblInfo* bblInfo, ThreadID tid) override;
        bool simulate() override;

        void branch(Address pc, bool taken, ThreadID tid);

        void abortTask(uint64_t cycle, ThreadID tid, bool isPriv) override;
        void closeEpoch() override;

        virtual void initStats(AggregateStat* parentStat) override;

        void waitTillMemComplete(ThreadID tid) override;
        void setUnassignedCtx(ThreadID tid);

        virtual void issueHead(ThreadID tid) override;

        void setTerminalCD(TerminalCD* _cd) override;
        uint32_t getCtxId(ThreadID tid) const override {
            return context(tid)->id;
        }
        void unblockWaiter(ThreadID tid) override;
        void unblockWaiter();

        bool isAvailableThread(ThreadID tid) const override;

        std::pair<bool, ThreadID> increaseAvailableThreads() override;
        std::pair<bool, ThreadID> decreaseAvailableThreads() override;

        // [mcj] Why are there so many? What is the difference between
        // getInstructionCount and getCommittedInstrs? Why did I have to
        // accidentally realize that by pushing breakdown tables into the
        // CoreContext, I broke the following method. This is unmaintainable.
        // Why isn't the first method called getTotalInstrs like to mirror its
        // counterpart?
        uint64_t getInstructionCount() const override;
        uint64_t getAbortedInstrs() const override;
        uint64_t getStallSbCycles() const override;
        uint64_t getCommittedInstrs() const override;
        uint64_t getTotalCycles() const override { return getCycle(); }
        uint64_t getNumAvailableThreads() const override {
            if (availableThreads.size()) return availableThreads.size();
            else return startAvailableThreads;
        }

        void windUp(uint64_t endCycle) {
            if (!endCycle) return; // fast-forwarded full run, nothing to do
            assert(endCycle >= statsCycle);
            // FIXME(victory): Do we need to do anything if we've already
            // updated stats this cycle?
            if (endCycle == statsCycle) return;
            updateStatsOnFinishedTask(endCycle-1);
        };

    protected:
        CoreContext* context(ThreadID t) {
            size_t base = cid * ctxts_.size();
            CoreContext* ctx = thread2ctxt_.at(t - base);
            assert(ctx != nullptr);
            return ctx;
        }

        const CoreContext* context(ThreadID t) const {
            size_t base = cid * ctxts_.size();
            const CoreContext* ctx = thread2ctxt_.at(t - base);
            assert(ctx != nullptr);
            return ctx;
        }

        virtual uint8_t selectIssuePort(DynUop*, uint64_t&);
        virtual void issueOne(CoreContext* issuectx);
        virtual std::tuple<CoreContext*, bool> getNextIssueCtx();
        void recalculatePriorities(uint64_t cycle);
        void addWaiter(ThreadID tid);
        virtual void updateStatsOnIssue(CoreContext* issuectx, uint64_t cycle, uint64_t slot, bool wrongPath=false);
        void updateStatsOnFinishedTask(uint64_t);
        virtual void updateActiveThread();
        void updateActiveThread(ThreadID tid);

        // Only one thread from a core can be active at a time.
        ThreadID activeThread;
        // Others (if not already waiting elsewhere) wait here.
        SPinConditionVariable nonActiveThreads;

        IssuePriority priority;

        uint64_t lastIssueCycle;

        uint64_t issueWidth;
        uint8_t fullPortMask;
        uint8_t portMask;

        // This needs to be separate from nextIssueCycle because ctx priorities can change in the middle.
        // Stats should be uptodate before a priority change occurs
        uint64_t statsCycle;

        uint8_t lastPortMask = 0;
        uint8_t lastCycleIssuedUops = 0;

        // priority dimestions
        bool timestampPriority = false;
        bool taskStartPriority = false;
        bool fpOnTS = false;
        bool icount = false;

        Counter unsetTbIssues;
        Counter dualIssueCycles;
        Counter wrongPathIssues;

        BranchPredictorPAg<10, 10, 10> branchPred;
        uint64_t mispredictPenalty;

    private:
        bool access(Address, uint32_t size, bool isLoad, uint64_t* latency,
                    CoreContext*);

        std::vector<CoreContext*> thread2ctxt_;
};
#endif
