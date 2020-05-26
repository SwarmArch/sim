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

#include "sim/watchdog.h"

#include "sim/assert.h"
#include "sim/interactive.h"
#include "sim/locks.h"
#include "sim/log.h"
#include "sim/rob.h"
#include "sim/sim.h"
#include "sim/taskprofilingsummary.h"
#include "sim/timestamp.h"

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

static time_t startTime;
static time_t lastTickTime;
static time_t lastCycleProgressTime;
static time_t lastTaskProgressTime;
static time_t lastGvtProgressTime;
static time_t lastFfTime;
static time_t lastNonFfTime;
static uint64_t lastCycle;
static uint64_t lastTaskCycles;
static TimeStamp gvt(0);
static TimeStamp lastGvt(0);
static lock_t watchdogLock;
static lock_t watchdogThreadLock;
static time_t cycleTimeout, taskTimeout, gvtTimeout, ffTimeout;
static pid_t pid, harnessPid;

void InitWatchdog(time_t cycleTimeout_, time_t taskTimeout_, time_t gvtTimeout_, time_t ffTimeout_) {
    startTime = time(nullptr);
    lastTickTime = time(nullptr);
    lastCycleProgressTime = lastTickTime;
    lastTaskProgressTime = lastTickTime;
    lastGvtProgressTime = lastTickTime;
    lastFfTime = lastTickTime;
    lastNonFfTime = lastTickTime;
    lastCycle = 0;
    lastTaskCycles = 0;
    gvt = ZERO_TS;
    lastGvt = ZERO_TS;
    cycleTimeout = cycleTimeout_;
    taskTimeout = taskTimeout_;
    gvtTimeout = gvtTimeout_;
    ffTimeout = ffTimeout_;
    pid = getpid();
    harnessPid = getppid();
    futex_init(&watchdogLock);
    futex_init(&watchdogThreadLock);
    futex_lock(&watchdogThreadLock);
}

void StopWatchdogThread() {
    // Will wake the thread immediately and cause its futex_trylock to succeed.
    // This avoids waiting for the thread to wake up, eg if sleep() was used.
    futex_unlock(&watchdogThreadLock);
}

// Collect a profile for several seconds from the Pin-instrumented application,
// including stack traces to show what the simulator pintool is doing.
// This is useful to debug when the simulator is stuck in an infinite loop.
// This is less useful in more complex deadlock/starvation scenarios.
static void DumpProfile() {
    futex_unlock(&watchdogLock); // Don't hold up the simulation.
    std::stringstream command;
    command << "perf record -F 99 -p " << pid << " -g -- sleep 5";
    warn("Attempting to record profile using: %s", command.str().c_str());
    // SIGCHLD is normally blocked in the Pintool-internal watchdog thread.
    // As the signal mask is inherited by child processes, we must unblock
    // SIGCHLD or perf will hang while waiting on sleep.
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    int status = pthread_sigmask(SIG_UNBLOCK, &mask, NULL);
    assert(status == 0);
    status = std::system(command.str().c_str());
    status = WEXITSTATUS(status);
    if (status) {
        warn("Command exit status is: %d", status);
    } else {
        warn("Run `perf report` in this directory to see recorded profile.");
    }
}

void TickWatchdog() {
    futex_lock(&watchdogLock);

    uint64_t cycle = getCurCycle();
    time_t curTime = time(nullptr);
    time_t elapsedSecs = curTime - startTime;
    time_t intervalSecs = curTime - lastTickTime;

    uint64_t cyclesPerSec = elapsedSecs? cycle/elapsedSecs : 0;
    uint64_t intervalCyclesPerSec = intervalSecs ? (cycle - lastCycle) / intervalSecs : 0;

    char time[128];
    char hostname[256];
    gethostname(hostname, 256);

    const TaskCyclesSummary& total = taskCyclesSummary();

    std::ofstream wd(ossinfo->outputDir + "/heartbeat");
    assert(wd.good());
    wd << "Running on: " << hostname << std::endl;
    wd << "PID: " << pid << std::endl;
    wd << "Harness PID: " << harnessPid << std::endl;
    wd << "Start time: " << ctime_r(&startTime, time);
    wd << "Current time: " << ctime_r(&curTime, time);
    wd << "In fast-forward: ";
    if (IsInFastForward()) wd << "yes, since " << ctime_r(&lastNonFfTime, time);
    else wd << "no, since " << ctime_r(&lastFfTime, time);
    wd << "Stats since start:" << std:: endl;
    wd.imbue(std::locale("")); // for thousands seperators, depending on environment.
    wd << " " << cycle << " cycles" << std::endl;
    wd << " " << cyclesPerSec << " cycles/s" << std::endl;
    wd << " " << total.commit.exec << " task-cycles committed" << std::endl;
    wd << " " << total.abort.exec << " task-cycles aborted" << std::endl;
    wd << " " << total.commit.tasks << " tasks committed" << std::endl;
    wd << " " << total.abort.tasks << " task aborts" << std::endl;
    wd << " " << getHeartbeats() << " application heartbeats committed" << std::endl;
    wd << "Current gvt:" << std:: endl;
    wd << " " << gvt.toString() << std::endl;
    wd << "Stats in last interval (" << intervalSecs << "s):" << std:: endl;
    wd << " " << cycle - lastCycle << " cycles" << std::endl;
    wd << " " << intervalCyclesPerSec << " cycles/s" << std::endl;
    wd.close();

    bool inSwarmRun = gvt != INFINITY_TS && gvt != ZERO_TS;
    if (AtInteractivePrompt()) {
        lastFfTime = curTime;

        // reset all timeouts, as they only apply when something is running
        lastNonFfTime = curTime;
        lastCycleProgressTime = curTime;
        lastTaskProgressTime = curTime;
        lastGvtProgressTime = curTime;
    } else if (IsInFastForward()) {
        lastFfTime = curTime;

        // reset simulation-related timeouts, as they only apply when actually simulating
        lastCycleProgressTime = curTime;
        lastTaskProgressTime = curTime;
        lastGvtProgressTime = curTime;
    } else if (!inSwarmRun) {
        // Reset Swarm-related timeouts, which only apply when tasks are running
        lastTaskProgressTime = curTime;
        lastGvtProgressTime = curTime;
    }

    if (!IsInFastForward()) {
        lastNonFfTime = curTime;
    } else if (curTime - lastNonFfTime >= ffTimeout) {
        warn("Reached timeout on fast-forwarding simulator.");
        DumpProfile();
        panic("Terminating simulation due to timeout on fast-forwarding");
    }

    if (cycle > lastCycle) {
        lastCycleProgressTime = curTime;
    }
    if (curTime - lastCycleProgressTime >= 10) {
        warn("Simulation has been stuck at cycle %ld for %ld seconds",
             cycle, curTime - lastCycleProgressTime);
    }
    if (curTime - lastCycleProgressTime >= cycleTimeout) {
        // Note, it is NOT safe to call HaltEvents() here, as the simulator may
        // be stuck in an infinite loop within a single event, so HaltEvents()
        // might never return.
        warn("Reached timeout on cycle forward progress.");
        for (auto &rob: ossinfo->robs) rob->dumpQueues();
        DumpProfile();
        panic("Terminating simulation due to timeout on cycle forward progress");
    }

    uint64_t taskCycles = total.commit.exec + total.abort.exec;
    if (taskCycles > lastTaskCycles) {
        lastTaskProgressTime = curTime;
    }
    if (curTime - lastTaskProgressTime >= 20) {
        // If the cycle progress warning above didn't trigger,
        // the cores seem to be making progress running instructions,
        // but the perhaps the GVT task is starved.
        warn("No task commits or aborts for %ld seconds",
             curTime - lastTaskProgressTime);
    }
    if (curTime - lastTaskProgressTime >= taskTimeout) {
        HaltEvents();
        for (auto &rob: ossinfo->robs) rob->dumpQueues();
        panic("Terminating simulation due to timeout on tasks finishing");
    }

    if (gvt > lastGvt) {
        lastGvtProgressTime = curTime;
    }
    if (curTime - lastGvtProgressTime >= 100) {
        // This can happen without triggering the task progress warning above
        // if pathological non-speculative tasks keep running (e.g.,
        // spillers and requeuers) while the GVT task starves.
        warn("No GVT progress for last %ld seconds", curTime - lastGvtProgressTime);
    }
    if (curTime - lastGvtProgressTime >= gvtTimeout) {
        HaltEvents();
        for (auto &rob: ossinfo->robs) rob->dumpQueues();
        panic("Terminating simulation due to timeout on GVT forward progress");
    }

    lastTickTime = curTime;
    lastCycle = cycle;
    lastTaskCycles = taskCycles;
    lastGvt = gvt;

    futex_unlock(&watchdogLock);
}

void UpdateWatchdogGvt(TimeStamp newGvt) {
    futex_lock(&watchdogLock);
    gvt = newGvt;
    futex_unlock(&watchdogLock);
}

void WatchdogThread(void* dummyArg) {
    const uint64_t period = 10 * 1000L * 1000L * 1000L;
    while (!futex_trylock_nospin_timeout(&watchdogThreadLock, period)) {
        TickWatchdog();
    }
    info("Watchdog thread done");
}
