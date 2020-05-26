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

#include "driver.h"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <queue>
#include <random>
#include <string>

#include "sim/collections/prio_queue.h"
#include "sim/core/core.h"
#include "sim/core/sbcore.h"
#include "sim/interactive.h"
#include "sim/thread.h"
#include "spin.h"

#undef DEBUG
#ifdef ENABLE_INTERACTIVE
// Interactive mode is needs info about cycles & events to be usable
#define DEBUG(args...) info(args)
#else
#define DEBUG(args...) //info(args)
#endif

/* Data structures */

// Holds the cycle of the currently running event.
// All prior cycles have finished simulation.
// While individual cores may run further ahead in time,
// no part of the system can run behind curCycle.
static uint64_t curCycle;

// Fast-forwarding is enabled until ROI_BEGIN is reached
static bool ff = true;

// dsm: Use array, not vector; this gives us fixed pointers & no resize races, which we'll need
static std::array<ThreadState, MAX_THREADS> threadStates;

// Need a priority queue to execute first available
// Guess what simulator has a bithacks-level optimized priority queue
static PrioQueue<BaseEvent, 1024> eventQueue;

// Queue to hold non-thread events in fast-forwarding mode
static PrioQueue<BaseEvent, 1024> ffEventQueue;

// We want this comparison to be fast, so no dynamic_cast
static bool isThreadEvent(BaseEvent* ev) {
    // NOTE: I know about ptrdiff_t, but this needs to be unsigned. This is
    // relying on how unsigned overflow wraps around. Note signed overflow is
    // undefined behavior.
    uintptr_t offset = ((uintptr_t) ev) - ((uintptr_t) &threadStates[0]);
    return offset < (threadStates.size() * sizeof(ThreadState));
}

static ThreadState* curThread;

// Holds newly captured threads (e.g., newly created threads or threads
// associated with asynchronously executing syscalls (see virt/virt.cpp) that
// have returned) to be inserted into eventQueue.
static std::vector<uint32_t> joinList;

static volatile uint32_t haltFlag ATTR_LINE_ALIGNED;

// For synchronization
static lock_t asyncEventLock;
static volatile uint32_t asyncEventFlag ATTR_LINE_ALIGNED;

/* Event queue routines */

// NOTE: Overwrites curThread. Caller should first handle what it wants to do
// with curThread (requeue, uncapture, block, ...)
spin::ThreadId NextThread() {
    // This is the event loop of our event-driven simulator!
    while (true) {
        if (unlikely(asyncEventFlag)) {
            futex_lock(&asyncEventLock);
            if (unlikely(haltFlag)) {
                haltFlag = false;
                while (true) _mm_pause();  // infinite loop
            }
            for (auto tid : joinList) {
                uint64_t joinCycle =
                    std::max(getCurCycle(), threadStates[tid].core->getCycle(tid));
                threadStates[tid].core->join(joinCycle, tid);
                threadStates[tid].setWakeupCycle(joinCycle);
                threadStates[tid].transition(QUEUED);
                eventQueue.push(&threadStates[tid]);
            }
            joinList.clear();
            asyncEventFlag = false;
            futex_unlock(&asyncEventLock);
        }

#ifdef ENABLE_INTERACTIVE
        InteractivePrompt();
#endif

        if (!eventQueue.empty()) {
            BaseEvent* ev = eventQueue.pop();
            DEBUG("Next Event : Thread %d at %ld",
                  isThreadEvent(ev) ? static_cast<ThreadState*>(ev)->tid : -1,
                  ev->cycle());
            assert(ev->cycle() >= curCycle);
            curCycle = ev->cycle();
            if (isThreadEvent(ev)) {
                // We've found our next thread
                curThread = static_cast<ThreadState*>(ev);
                curThread->transition(RUNNING);
                curThread->core->closeEpoch();
                return curThread->tid;
            } else {
                // This is a normal event
                Event* event = static_cast<Event*>(ev);
                event->process();  // may generate other events
                delete event;
            }
        }
    }
}

std::tuple<spin::ThreadId, bool> SwitchThread(spin::ThreadId tid) {
    assert(curThread->tid == tid);

    if (curThread->state == RUNNING) {
        if (curThread->core->getCycle(tid) == getCurCycle()) {
            // Stay with this thread until our timing model advances the cycle.
            // This means the timing model must regularly advance to avoid
            // starving other threads.
            return std::make_tuple(tid, false);
        }
        DEBUG("[%d] SwitchThread curCycle %ld", tid, curThread->core->getCycle(tid));
        curThread->setWakeupCycle(curThread->core->getCycle(tid));
        curThread->transition(QUEUED);
        eventQueue.push(curThread);
    }

    // NextThread() may cause curThread to switch PC, even we get the
    // same one again. So include this in the return condition.
    spin::ThreadContext* tc = spin::getContext(tid);
    uint64_t pc = spin::getReg(tc, REG_RIP);

    uint32_t nextTid = NextThread();
    assert(!curThread->task || !curThread->task->hasPendingAbort() || curThread->isPriv());

    bool mustReturn = nextTid != tid || pc != spin::getReg(tc, REG_RIP);
    return std::make_tuple(nextTid, mustReturn);
}

// This is a lot like SwitchThread, but with different switching conditions
spin::ThreadId PreventFfStarvation(spin::ThreadId tid) {
    assert(IsInFastForward());
    assert(curThread->tid == tid);
    assert(curThread->state == RUNNING);

    // To prevent one spin-waiting thread from starving all other threads,
    // perform context switches if one thread has been running for a while.
    static spin::ThreadId prevTid = 0;
    static uint32_t runLength = 0;
    if (tid == prevTid) runLength++;
    else runLength = 0;
    if (unlikely(runLength > 1000)) {
        DEBUG("[%d] Switch thread for fairness", tid);
        runLength = 0;
        curThread->transition(QUEUED);
        eventQueue.push(curThread);
        return NextThread();
    } else {
        prevTid = tid;
        return tid;
    }
}

// Wait for the currently executing event to finish, then halt the event loop
// so that no more events run.  It is NOT safe to call this from inside any
// (thread or non-thread) event.
void HaltEvents() {
    info("Halting simulation...");
    futex_lock(&asyncEventLock);
    haltFlag = true;
    asyncEventFlag = true;
    futex_unlock(&asyncEventLock);
    while (haltFlag) _mm_pause();
    info("Simulation halted.");
    return;
}

/* SPIN scheduling routines */

void Capture(spin::ThreadId tid, bool runsNext) {
    futex_lock(&asyncEventLock);
    if (!runsNext) {
        joinList.push_back(tid);
        asyncEventFlag = true;
    } else {
        // This newly captured thread is the first and only captured thread.
        assert(!curThread);
        curThread = &threadStates[tid];
        curThread->transition(RUNNING);
        uint64_t joinCycle = std::max(getCurCycle(), threadStates[tid].core->getCycle(tid));
        curThread->core->join(joinCycle, curThread->tid);
    }
    futex_unlock(&asyncEventLock);
}

spin::ThreadId Uncapture(spin::ThreadId tid, spin::ThreadContext* tc) {
    assert(curThread);
    assert(curThread->tid == tid);
    curThread->transition(UNCAPTURED);

    // FIXME(dsm): Due to delayed uncaptures, we sometimes get the uncapture
    // after threadEnd. In SPIN, we should delay the threadEnd as well.
    if (curThread->core)
        curThread->core->leave(Core::State::IDLE, curThread->tid);

    DEBUG("[%d] Uncapture", tid);
    return NextThread();
}

void BlockThreadAfterSwitch(ExecState state) {
    curThread->transition(state);
    spin::blockAfterSwitch();
}

void BlockThread(uint32_t tid, ExecState state) {
    DEBUG("[%u] Blocking thread at %lu (curThread %d)",
          tid, getCurCycle(), curThread->tid);
    if (tid == curThread->tid) {
        BlockThreadAfterSwitch(state);
        if (curThread->isQueued()) eventQueue.erase(curThread);
    } else {
        assert(threadStates[tid].isQueued());
        eventQueue.erase(&threadStates[tid]);
        threadStates[tid].transition(state);
        spin::blockIdleThread(tid);
    }
}

void UnblockThread(uint32_t tid, uint64_t cycle) {
    DEBUG("[%u] Unblocking thread at %lu", tid, cycle);
    // could be in UNCAPTURED right after thread starting but before it gets a chance to run
    if (threadStates[tid].state == QUEUED || threadStates[tid].state == UNCAPTURED) return;

    assert(tid < MAX_THREADS);
    assert(cycle >= getCurCycle());
    spin::unblock(tid);

    Core* core = threadStates[tid].core;
    uint64_t joinCycle = std::max(cycle, core->getCycle(tid));
    core->join(joinCycle, tid);

    ThreadState* ts = &threadStates[tid];
    assert(ts->state == BLOCKED || ts->state == BLOCKED_TOABORT);

    threadStates[tid].transition(QUEUED);
    threadStates[tid].setWakeupCycle(joinCycle);
    eventQueue.push(&threadStates[tid]);
}

/* Thread start/end and scheduling */

static std::deque<Core*> coreFreelist;

void ThreadStart(spin::ThreadId tid) {
    assert(tid < MAX_THREADS);
    assert(tid == threadStates[tid].tid);
    if (coreFreelist.empty()) {
        panic("Thread %d started, but no free core left (build a scheduler?)", tid);
    }
    threadStates[tid].core = coreFreelist.front();
    SbCore* sbcore = dynamic_cast<SbCore*>(threadStates[tid].core);
    if (sbcore) sbcore->setUnassignedCtx(tid);
    coreFreelist.pop_front();

    threadStates[tid].transition(UNCAPTURED);
    DEBUG("Thread %d started (cycle %ld, core %d)",
          tid, getCurCycle(), threadStates[tid].core->getCid());
}

void ThreadFini(spin::ThreadId tid) {
    DEBUG("Thread %d finished (%ld)", tid, getCurCycle());
    coreFreelist.push_back(threadStates[tid].core);
    SbCore* sbcore = dynamic_cast<SbCore*>(GetThreadState(tid)->core);
    if (sbcore) {
        // [maleen] Since sbcore does not follow the FSM based stats, need to
        // explicit notify and record the last few cycles
        // [mcj] Not sure if std::max is correct? But getCurCycle()
        // probably isn't right. Maleen, please weigh in.
        uint64_t cycle = std::max(
                getCurCycle(),
                sbcore->getCycle(tid));
        sbcore->windUp(cycle);
    }
    threadStates[tid].core = nullptr;

    // If this is the main thread, finish the simulation
    // NOTE: This used to wait until all threads were done. In fact, POSIX
    // specifies that the main thread exiting terminates the whole process.
    // Some apps (eg Cilk) take advantage of this to avoid joining all threads.
    // In theory, it may be possible to use the clone syscall to have a
    // non-main thread survive a main-thread exit, so this may need more
    // refinement, but it should handle all of our current apps correctly.
    if (tid == 0) SimEnd();
}

/* Enumeration helpers */

uint32_t GetCurTid() { return curThread? curThread->tid : INVALID_TID; }

ThreadState* GetCurThread() { return curThread; }

ThreadState* GetThreadState(uint32_t tid) {
    assert(tid <= threadStates.size());
    return &threadStates[tid];
}

uint32_t GetCoreIdx(uint32_t tid) {
    assert(threadStates[tid].state != INVALID);
    return threadStates[tid].core->getCid();
}

uintptr_t GetStackStart(uint32_t tid) {
    assert(tid < threadStates.size());
    return threadStates[tid].rspCheckpoint;
}

bool IsPriv(uint32_t tid) {
    assert(tid < threadStates.size());
    return threadStates[tid].isPriv();
}

bool IsPrivDoomed(uint32_t tid) {
    assert(tid < threadStates.size());
    return threadStates[tid].isPrivDoomed();
}

/* Fast-forwarding routines */

bool IsInFastForward() { return ff; }

void ExitFastForward() {
    assert(IsInFastForward());
    if (!coreFreelist.empty()) {
        info("More cores exist than threads: %u hardware contexts unused",
             coreFreelist.size());
    }
    ff = false;
    while (!ffEventQueue.empty()) eventQueue.push(ffEventQueue.pop());
    ffEventQueue.reset();
}

void EnterFastForward() {
    assert(!IsInFastForward());
    if (!coreFreelist.empty()) {
        info("More cores exist than threads: %u hardware contexts unused",
             coreFreelist.size());
    }
    ff = true;
    // FIXME(dsm): Now that we use events to drive time, and cores can block on
    // events, we need to define strict invariants about what happens on FF
    // transitions. Do we run all timing events? Put them on the back-burner?
    // Modify all timing models to become aware of FF?
    std::vector<BaseEvent*> threadEvents;
    while (!eventQueue.empty()) {
        BaseEvent* e = eventQueue.pop();
        if (isThreadEvent(e)) {
            threadEvents.push_back(e);
            DEBUG("Queued thread %u for ff", static_cast<ThreadState*>(e)->tid);
        } else {
            ffEventQueue.push(e);
        }
    }
    eventQueue.reset();
    for (auto e : threadEvents) eventQueue.push(e);
    threadEvents.clear();
}

void InitDriver() {
    curCycle = 0;
    curThread = nullptr;
    futex_init(&asyncEventLock);

    ThreadID idx = 0;
    for (auto& ts : threadStates) ts.tid = idx++;
    assert(idx == MAX_THREADS);

    for (uint32_t c = 0; c < ossinfo->numCores; c++) {
        for (uint32_t t = 0; t < ossinfo->numThreadsPerCore; t++) {
            coreFreelist.push_back(ossinfo->cores[c]);
        }
    }
}

/* External interface (in sim.h) */
uint64_t getCurCycle() { return curCycle; }
const uint64_t& getCurCycleRef() { return curCycle; }

void pushEvent(Event* e) { IsInFastForward()? ffEventQueue.push(e) : eventQueue.push(e); }
void eraseEvent(Event* e) { IsInFastForward()? ffEventQueue.erase(e) : eventQueue.erase(e); }
