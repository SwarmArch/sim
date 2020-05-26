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

#ifdef ENABLE_TRACING

#include "sim/trace.h"
#include <fstream>
#include <unordered_map>
#include "sim/log.h"
#include "sim/sim.h"

#include "sim/task.h"
#include "sim/rob.h"
#include "spin.h"

namespace trace {

static uint64_t lastUid = 0;  // null (used when no task) is uid 0
static std::unordered_map<const Task*, uint64_t> uidMap;
static std::ofstream traceFile;

void init() {
    traceFile.open("trace.out");
    if (!traceFile.good()) panic("Could not open trace file");
}

void fini() {
    traceFile.close();
}

// Helpers to print records
template <typename T> static void printArg(const T& t) { traceFile << " " << t; }

template <typename... Args>
static void record(const char* evName, Args... args) {
    traceFile << getCurCycle() << " " << evName;

    // Print each argument
    // See http://stackoverflow.com/a/17340003
    int dummy[] = { (printArg(args), 0)... };
    (void) dummy;  // make gcc happy

    traceFile << std::endl;
}

// Helpers to manage task uids
static uint64_t createUid(const Task* t) {
    assert(t);
    assert(uidMap.find(t) == uidMap.end());
    uint64_t uid = ++lastUid;
    uidMap[t] = uid;
    return uid;
}

static uint64_t getUid(const Task* t) {
    if (t == nullptr) return 0;
    auto it = uidMap.find(t);
    assert(it != uidMap.end());
    return it->second;
}

static uint64_t unmapUid(const Task* t) {
    assert(t);
    auto it = uidMap.find(t);
    assert(it != uidMap.end());
    uint64_t uid = it->second;
    uidMap.erase(it);
    return uid;
}

static uint64_t remapUid(const Task* prev, const Task* cur) {
    assert(prev && cur);
    uint64_t uid = unmapUid(prev);
    auto it = uidMap.find(cur);
    assert(it == uidMap.end());
    uidMap[cur] = uid;
    return uid;
}

void taskCreated(const Task* t, const Task* parent) {
    record("T", createUid(t), getUid(parent), t->taskFn);
}

void taskStart(const Task* t, uint32_t tid) {
    record("B", getUid(t), tid);
}

void taskFinish(const Task* t) {
    record("E", getUid(t));
}

void taskCommitted(const Task* t) {
    record("C", unmapUid(t), t->lts());
}

void taskAborted(const Task* t, bool requeue) {
    uint64_t oldUid = unmapUid(t);
    uint64_t newUid = requeue? createUid(t) : 0;
    record("A", oldUid, newUid, t->lts());
}

void taskSpilled(const Task* t, const Task* spiller) {
    record("S", unmapUid(t), t->lts());
}

void taskStolen(const Task* oldTask, const Task* newTask) {
    record("M", remapUid(oldTask, newTask), newTask->container()->getIdx());
}

void taskYield(const Task* oldTask, const Task* newTask) {
    // Same format as "T", so it can be treated as a creation event
    record("Y", createUid(newTask), getUid(oldTask), newTask->taskFn);
}

void taskDiscarded(const Task* t) {
    // Same format as "X", since this is a specialized child abort
    // (for abort handlers only?)
    record("Z", unmapUid(t));
}

// Task abort profiling
void dataAbort(const Task* t, const Task* aborter, uintptr_t lineAddress, bool isWrite, bool isWriter) {
    uintptr_t aborterPc = 0;
    if (aborter->running() && !aborter->hasPendingAbort()) {
        aborterPc = spin::getReg(spin::getContext(aborter->runningTid), REG_RIP);
    }
    record("D", getUid(t), getUid(aborter), aborterPc, lineAddress, isWriter? "WSet" : "RSet", isWrite? "W" : "R");
}

void childAbort(const Task* t) {
    record("X", getUid(t));
}

void resourceAbort(const Task* t) {
    record("R", getUid(t));
}

void zoomingAbort(const Task* t) {
    record("F", getUid(t));
}

};

#endif
