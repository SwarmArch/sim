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

#include "sim/memory/mem_ctrls.h"
#include "sim/log.h"
#include "sim/sim.h"

#undef DEBUG_TIMING
#define DEBUG_TIMING(args...) //info(args)

#undef DEBUG
#define DEBUG(args...) //info(args)

uint64_t SimpleMemory::access(const MemReq& req, MemResp& resp,
                              uint64_t cycle) {
    if (req.type == PUTS) {
      //Not a real access -- memory must treat clean wbacks as if they never happened.
      resp.newState = I;
      return cycle;
    }

    DEBUG("[%s] Request received: %s", name(), req.toString().c_str());

    switch (req.type) {
        case PUTX:
            resp.newState = I;
            profWrites.inc();
            break;
        case GETS:
            resp.newState = req.is(MemReq::NOEXCL)? S : E;
            profReads.inc();
            break;
        case GETX:
            resp.newState = M;
            profReads.inc();
            break;

        default: panic("!?");
    }

    uint64_t respCycle = cycle + latency;
    assert(respCycle >= cycle);
    return respCycle;
}

LimitedSimpleMemory::LimitedSimpleMemory(uint32_t _latency, uint32_t _megacyclesPerSecond, uint32_t _megabytesPerSecond, std::string& _name)
    : SimpleMemory(_latency, _name),
    megabytesPerSecond(_megabytesPerSecond),
    megacyclesPerSecond(_megacyclesPerSecond)
{
    // these aren't stored anywhere; just for the assert/print
    double bytesPerCycle = ((double)_megabytesPerSecond)/((double)_megacyclesPerSecond);
    double cyclesBetweenRequests = ossinfo->lineSize / bytesPerCycle;
    assert(cyclesBetweenRequests > 0.0);
    DEBUG("Building bandwidth-limited memory: a request every %.2f cycles",
          cyclesBetweenRequests);
}

uint64_t LimitedSimpleMemory::access(const MemReq& req, MemResp& resp,
                                     uint64_t cycle) {
    if (req.type == PUTS) {
      //Not a real access -- memory must treat clean wbacks as if they never happened.
      resp.newState = I;
      return cycle;
    }

    DEBUG("[%s] Request received: %s", name(), req.toString().c_str());

    switch (req.type) {
        case PUTX:
            resp.newState = I;
            profWrites.inc();
            break;
        case GETS:
            resp.newState = req.is(MemReq::NOEXCL)? S : E;
            profReads.inc();
            break;
        case GETX:
            resp.newState = M;
            profReads.inc();
            break;

        default: panic("!?");
    }

    // reqNum enforces the bandwidth requirement;
    // (cyclesBetweenRequests) sysCycles = 1 "reqCycle" = reqNum
    // just need to call schedule() once
    uint64_t reqNum = sysCycleToReqNum(cycle);
    reqNum = reqWindow.schedule(reqNum);
    // sometimes scheduledCycle < cycle and i don't want to figure out the math
    uint64_t scheduledCycle = std::max(cycle, matchingReqNumToSysCycle(reqNum));

    // periodically advance; getCurCycle() is always a lower bound
    if (getCurCycle() - prevAdvance > advancePeriod) {
        uint64_t advanceReqNum = sysCycleToReqNum(getCurCycle());
        reqWindow.advance(advanceReqNum);
        prevAdvance = getCurCycle();
    }

    uint64_t contentionCycles = scheduledCycle - cycle;
    if (req.type == PUTX)
        writeContentionCycles.push(contentionCycles);
    else
        readContentionCycles.push(contentionCycles);

    uint64_t respCycle = scheduledCycle + latency;
    // printf("[limitedsimple] reqcycle=%ld, scheduled=%ld, resp=%ld\n", cycle, scheduledCycle, respCycle);
    assert(respCycle >= cycle);

    return respCycle;
}


MD1Memory::MD1Memory(uint32_t requestSize, uint32_t megacyclesPerSecond, uint32_t megabytesPerSecond, uint32_t _zeroLoadLatency, std::string& _name)
    : MemCtrl(_name), zeroLoadLatency(_zeroLoadLatency)
{
    lastPhase = 0;

    double bytesPerCycle = ((double)megabytesPerSecond)/((double)megacyclesPerSecond);
    maxRequestsPerCycle = bytesPerCycle/requestSize;
    assert(maxRequestsPerCycle > 0.0);

    smoothedPhaseAccesses = 0.0;
    curPhaseAccesses = 0;
    curLatency = zeroLoadLatency;
}

void MD1Memory::updateLatency() {
    uint32_t phaseCycles = (getCurCycle() - lastPhase*ossinfo->phaseLength);
    if (phaseCycles < 10000) return; //Skip with short phases

    smoothedPhaseAccesses =  (curPhaseAccesses*0.5) + (smoothedPhaseAccesses*0.5);
    double requestsPerCycle = smoothedPhaseAccesses/((double)phaseCycles);
    double load = requestsPerCycle/maxRequestsPerCycle;

    //Clamp load
    if (load > 0.95) {
        //warn("MC: Load exceeds limit, %f, clamping, curPhaseAccesses %d, smoothed %f, phase %ld", load, curPhaseAccesses, smoothedPhaseAccesses, zinfo->numPhases);
        load = 0.95;
        profClampedLoads.inc();
    }

    double latMultiplier = 1.0 + 0.5*load/(1.0 - load); //See Pollancek-Khinchine formula
    curLatency = (uint32_t)(latMultiplier*zeroLoadLatency);


    DEBUG_TIMING ("%s: Load %.2f, latency multiplier %.2f, latency %d", name(), load, latMultiplier, curLatency);
    uint32_t intLoad = (uint32_t)(load*100.0);
    profLoad.inc(intLoad);
    profUpdates.inc();

    curPhaseAccesses = 0;
    lastPhase = static_cast<uint64_t>((double) getCurCycle() / ossinfo->phaseLength);
}

uint64_t MD1Memory::access(const MemReq& req, MemResp& resp, uint64_t cycle) {
    if (req.type == PUTS) {
      //Not a real access -- memory must treat clean wbacks as if they never happened.
      resp.newState = I;
      return cycle;
    }

    DEBUG("[%s] Request received: %s", name(), req.toString().c_str());

    uint64_t curPhase = static_cast<uint64_t>((double)getCurCycle()/ossinfo->phaseLength);
    if (curPhase > lastPhase) {
        updateLatency();
    }

    switch (req.type) {
        case PUTX:
            //Dirty wback
            profWrites.inc();
            profTotalWrLat.inc(curLatency);
            //Note no break
        case GETS:
            profReads.inc();
            profTotalRdLat.inc(curLatency);
            resp.newState = req.is(MemReq::NOEXCL)? S : E;
            break;
        case GETX:
            profReads.inc();
            profTotalRdLat.inc(curLatency);
            resp.newState = M;
            break;

        default: panic("!?");
    }
    return cycle + curLatency;
}
