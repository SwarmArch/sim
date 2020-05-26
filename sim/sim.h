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

#include <stdint.h>
#include <array>
#include <memory>
#include <string>
#include <vector>

#include "sim/assert.h"
#include "sim/spin_cv.h" // FIXME(mcj) remove stallers
#include "sim/run_condition.h"

#define MAX_THREADS (2048)  // Pin limitation

/* NOTE (dsm): This file is included often. To avoid circular deps, try to have
 * class declarations here as needed instead of includes to other parts of the
 * simulator.
 */

class AggregateStat;
class AbortHandler;
class Core;
class Event;
class GVTArbiter;
class TaskMapper;
class ROB;
class StackTracker;
class StatsBackend;
class Task;
class TimeStamp;
class ThreadState;
class TSArray;
class TSB;
class ThreadThrottlerUnit;
class MemoryPartition;

enum ExecState : unsigned int;  // Forward declaring enum

// Used by non-sim parts of the program to get a pointer to the current task
// Returns NULL if no task is currently being simulated
Task* GetCurTask();
uint32_t GetCurTid();
uint32_t GetCoreIdx(uint32_t tid);
uintptr_t GetStackStart(uint32_t tid);
bool IsPriv(uint32_t tid);
bool IsPrivDoomed(uint32_t tid);

// Prepare the current thread to block
void BlockThreadAfterSwitch(ExecState state);
// Unblocks a currently blocked thread, re-enqueueing it in the priority queue
void UnblockThread(uint32_t tid, uint64_t cycle);

// Causes the thread running an aborting task to block on BLOCKED_TOABORT
// state. Works on the current thread.
void BlockAbortingTaskOnThread(uint32_t tid);

// Causes the thread with the given tid to abort its currently running task
// Called by the ROB. ROB claims the task pointer (and deletes it), so this
// should cause the thread running the task to relinquish its pointer.
void AbortTaskOnThread(uint32_t tid);

// Global simulation information
struct GlobSimInfo {
    // Simulation globals
    uint32_t freqMHz;
    uint32_t phaseLength;
    bool logIndirectTargets;

    // ROBs and threads
    size_t numROBs;
    uint32_t numThreadsPerROB;

    uint32_t logStackSize;

    AbortHandler* abortHandler;
    TSArray** tsarray;
    bool usePreciseAddressSets;
    bool selectiveAborts;
    bool oneIrrevocableThread;
    uint32_t maxFrameDepth;

    // Cores
    Core** cores;
    size_t numCores;
    size_t numThreadsPerCore;
    bool isScoreboard;

    // Memory hierarchy
    uint32_t lineSize;
    uint32_t lineBits;
    StackTracker* stackTracker;
    MemoryPartition* memoryPartition;

    // Needed because the program may chdir somewhere else
    std::string outputDir;

    // Stats
    AggregateStat* rootStat;
    AggregateStat* tmStat;
    std::vector<StatsBackend*> statsBackends;

    // Heartbeats
    uint64_t maxHeartbeats;
    uint64_t ffHeartbeats;

    // TODO  Make these RoB parameters (have a timing ROB?)
    // dsm: If these are core delays, they should be inside the core, not the
    // ROB
    uint32_t enqueueDelay;
    uint32_t dequeueDelay;

    uint32_t tasksPerSpiller;
    uint32_t tasksPerRequeuer;

    // Allows non-irrevocable tasks to enqueue tasks beyond swarm::max_children.
    // Useful to support commit/abort handlers needed by e.g. malloc and free
    // on tasks that already reach swarm::max_children.
    uint32_t extraChildren;

    // MAYSPEC tasks are unique in that they can be executed in any of the three
    // speculation modes:
    // * maybe speculate (by design of course), but also
    // * must speculate
    // * cant speculate
    // This config dictates which mode will be used for MAYSPEC tasks.
    RunCondition mayspecMode;

    TaskMapper* taskMapper;

    std::vector<ROB*> robs;
    std::vector<TSB*> tsbs;
    std::vector<ThreadThrottlerUnit*> throttlers;
};

extern const GlobSimInfo* ossinfo;
// FIXME(mcj) This "temporary" solution that lasted for months just got worse :)
extern SPinConditionVariable stallers;

uint64_t getCurCycle();
uint64_t getCurThreadLocalCycle();

uint64_t getHeartbeats();
uint64_t getSimMallocCommits();
uint64_t getSimMallocAborts();
uint64_t getSimFreeCommits();
uint64_t getSimFreeAborts();

// NOTE: Returns a const ref to the continuously updated curCycle; used for
// dependency injection into the ROB and other classes, do not substitute calls
// to this with getCurCycle()!
const uint64_t& getCurCycleRef();

bool IsInFastForward();
bool IsInSwarmROI();

inline uint32_t getROBIdx(uint32_t coreIdx) {
    assert(coreIdx < ossinfo->numCores);
    return coreIdx / (ossinfo->numCores / ossinfo->numROBs);
}

void SimEnd();

// Event interface
void pushEvent(Event* e);
void eraseEvent(Event* e);

void HaltEvents();

// FIXME(dsm): Hacky staller interface
void registerShouldStall(Task& task);

// Syscall side-effect interface. Lets virt validate the input data and account
// for the side-effects of syscalls.
void ReadRange(uint32_t tid, char* buf, size_t bytes);
void WriteRange(uint32_t tid, char* buf, size_t bytes);
