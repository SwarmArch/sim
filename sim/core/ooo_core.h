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

#ifndef OOOCORE_H_
#define OOOCORE_H_

#include <string>
#include <stdint.h>

#include "core_structures.h"
#include "sbcore.h"
#include "../thread.h"
#include "sim/spin_cv.h"
#include <unordered_map>
#include <map>

class OoOCore : public SbCore {
    private:

        // KNL http://www.agner.org/optimize/microarchitecture.pdf page 171
        // The register rename and reorder buffer has 72 entries. The reservation stations have 2x12
        // entries for the integer lines, 2x20 entries for the floating point and vector lines, and 12
        // entries shared for the two memory lines.
        WindowStructure<1024, 36 /*size*/> insWindow; //NOTE: IW width is implicitly determined by the decoder, which sets the port masks according to uop type
        // ReorderBuffer<32, 2> rob;
        ReorderBuffer* rob;

        uint64_t lastStoreCommitCycle;
        uint64_t lastStoreAddrCommitCycle; //tracks last store addr uop, all loads queue behind it

        //LSU queues are modeled like the ROB. Surprising? Entries are grabbed in dataflow order,
        //and for ordering purposes should leave in program order. In reality they are associative
        //buffers, but we split the associative component from the limited-size modeling.
        //NOTE: We do not model the 10-entry fill buffer here; the weave model should take care
        //to not overlap more than 10 misses.
        // ReorderBuffer<12, 1> loadStoreQueue;
        ReorderBuffer* loadStoreQueue;

        Counter extraSlots;

        bool stallIssueOnLSQ;

        void finishTask(ThreadID tid) override;
        void issueHead(ThreadID tid) override;
        void issueOne(CoreContext* issuectx) override;
        uint8_t selectIssuePort(DynUop*, uint64_t&) override;
        void join(uint64_t cycle, ThreadID tid) override;
        void leave(Core::State, ThreadID tid) override;
        std::tuple<CoreContext*, bool> getNextIssueCtx() override;

        bool access(Address addr, uint32_t size, bool isLoad, uint64_t* latency, ThreadID tid);

        void advanceCurCycle(CoreContext*);

        void updateStatsOnIssue(CoreContext* issuectx, uint64_t cycle, uint64_t slot, bool wrongPath=false) override;

        void updateActiveThread() override;
    public:

        OoOCore(const std::string& _name, uint32_t _cid,
                uint32_t numctxts, FilterCache* _l1d, IssuePriority priority,
                uint64_t issueWidth, uint64_t mispredictPenalty,
                uint32_t loadBufferEntries, uint32_t storeBufferEntries,
                uint32_t startThreads, bool stallIssueOnLSQ);

        inline uint64_t getCycle() const override {
            return curCycle;
        }
        inline uint64_t getCycle(ThreadID tid) const override {
            return std::max(context(tid)->nextIssueCycle, curCycle);
        }

        virtual void initStats(AggregateStat* parentStat) override;

        inline uint64_t pipelineFlushPenalty() override {
            // [maleen] No need for pipeline flush penalty. ooo doesn't start the next task until the previous one drains
            return 0;
        }

};
#endif
