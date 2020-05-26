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

#ifndef MEM_CTRLS_H_
#define MEM_CTRLS_H_

#include <string>
#include <vector>
#include "sim/memory/memory_hierarchy.h"
#include "sim/bit_window.h"
#include "sim/object.h"
#include "sim/pad.h"
#include "sim/stats/stats.h"

class MemCtrl : public SimObject, public MemPort {
  public:
    explicit MemCtrl(const std::string& _name) : SimObject(_name) {}
    virtual void initStats(AggregateStat* parentStat) {}
};

/* Simple memory (or memory bank), has a fixed latency */
class SimpleMemory : public MemCtrl {
  protected:
    const uint32_t latency;
    Counter profReads;
    Counter profWrites;

  public:
    SimpleMemory(uint32_t _latency, std::string& _name)
        : MemCtrl(_name), latency(_latency) {}

    void initStats(AggregateStat* parentStat) override {
        AggregateStat* memStats = new AggregateStat();
        memStats->init(name(), "Memory controller stats");
        profReads.init("rd", "Read requests");
        memStats->append(&profReads);
        profWrites.init("wr", "Write requests");
        memStats->append(&profWrites);

        parentStat->append(memStats);
    }

    uint64_t access(const MemReq& req, MemResp& resp, uint64_t cycle) override;
};

// same as simple memory, but has queues and a minimum request-to-request latency
// to cap bandwidth
class LimitedSimpleMemory : public SimpleMemory {
  private:
    uint32_t megabytesPerSecond, megacyclesPerSecond;
    uint64_t advancePeriod = 10000;
    uint64_t prevAdvance; // sys cycles

    // operates in terms of reqNums ("mem cycles" but not really)
    BitWindow<2> reqWindow;

    RunningStat<size_t> readContentionCycles;
    RunningStat<size_t> writeContentionCycles;

    // as done in DDR mem, should make sure multiplication is first
    // cyclesBetweenRequests = 64 / ((MB/s) / (MC/s)) = 64 * MC/s / MB/s
    inline uint64_t sysCycleToReqNum(uint64_t sysCycle) { return (sysCycle * megabytesPerSecond / 64 / megacyclesPerSecond) + 1; }
    // produces a syscycle that when translated back, produces the same memCycle
    inline uint64_t matchingReqNumToSysCycle(uint64_t reqNum) { return (2*reqNum-1) * 64 * megacyclesPerSecond / megabytesPerSecond / 2; }

  public:
    LimitedSimpleMemory(uint32_t _latency, uint32_t _megaCyclesPerSecond, uint32_t _megabytesPerSecond, std::string& _name);

    void initStats(AggregateStat* parentStat) override {
        AggregateStat* memStats = new AggregateStat();
        memStats->init(name(), "Memory controller stats");
        profReads.init("rd", "Read requests");
        memStats->append(&profReads);
        profWrites.init("wr", "Write requests");
        memStats->append(&profWrites);
        readContentionCycles.init(
            "rdContentionCycles",
            "Delay per main memory read due to BW limit");
        memStats->append(&readContentionCycles);
        writeContentionCycles.init(
            "wrContentionCycles",
            "Delay per main memory write due to BW limit");
        memStats->append(&writeContentionCycles);

        parentStat->append(memStats);
    }

    uint64_t access(const MemReq& req, MemResp& resp, uint64_t cycle) override;
};

/* Implements a memory controller with limited bandwidth, throttling latency
 * using an M/D/1 queueing model.
 */
class MD1Memory : public MemCtrl {
  private:
    uint64_t lastPhase;
    double maxRequestsPerCycle;
    double smoothedPhaseAccesses;
    const uint32_t zeroLoadLatency;
    uint32_t curLatency;

    Counter profReads;
    Counter profWrites;
    Counter profTotalRdLat;
    Counter profTotalWrLat;
    Counter profLoad;
    Counter profUpdates;
    Counter profClampedLoads;
    uint32_t curPhaseAccesses;

  public:
    MD1Memory(uint32_t lineSize, uint32_t megacyclesPerSecond,
              uint32_t megabytesPerSecond, uint32_t _zeroLoadLatency,
              std::string& _name);

    void initStats(AggregateStat* parentStat) override {
        AggregateStat* memStats = new AggregateStat();
        memStats->init(name(), "Memory controller stats");
        profReads.init("rd", "Read requests");
        memStats->append(&profReads);
        profWrites.init("wr", "Write requests");
        memStats->append(&profWrites);
        profTotalRdLat.init("rdlat", "Latency experienced by read requests");
        memStats->append(&profTotalRdLat);
        profTotalWrLat.init("wrlat", "Latency experienced by write requests");
        memStats->append(&profTotalWrLat);
        profLoad.init("load", "Sum of load factors (0-100) per update");
        memStats->append(&profLoad);
        profUpdates.init("ups", "Number of latency updates");
        memStats->append(&profUpdates);
        profClampedLoads.init("clampedLoads",
            "Number of updates where the load was clamped to 95%");
        memStats->append(&profClampedLoads);
        parentStat->append(memStats);
    }

    uint64_t access(const MemReq& req, MemResp& resp, uint64_t cycle) override;

  private:
    void updateLatency();
};

//DRAMSIM does not support non-pow2 channels, so:
// - Encapsulate multiple DRAMSim controllers
// - Fan out addresses interleaved across banks, and change the address to a "memory address"
class SplitAddrMemory : public MemCtrl {
  private:
    const vector<MemCtrl*> mems;

  public:
    SplitAddrMemory(const vector<MemCtrl*>& _mems, const std::string& _name)
        : MemCtrl(_name), mems(_mems) {}

    uint64_t access(const MemReq& req, MemResp& resp, uint64_t cycle) override {
        Address addr = req.lineAddr;
        uint32_t mem = addr % mems.size();
        Address ctrlAddr = addr / mems.size();
        MemReq nextReq = req;
        nextReq.lineAddr = ctrlAddr;
        return mems[mem]->access(nextReq, resp, cycle);
    }

    void initStats(AggregateStat* parentStat) override {
        for (auto mem : mems) mem->initStats(parentStat);
    }
};

#endif  // MEM_CTRLS_H_
