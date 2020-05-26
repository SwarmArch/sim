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

#include <sstream>
#include <stdint.h>
#include <string>
#include <tuple>
#include "sim/assert.h"
#include "sim/event.h"
#include "sim/log.h"
#include "sim/match.h"
#include "sim/task.h"
#include "sim/types.h"

#undef DEBUG
#define DEBUG(args...)  //info(args)

/* Execution FSM mimicks libspin's
 *  Starting state: INVALID
 *  Runqueue states: RUNNING, QUEUED
 *  Syscall state: UNCAPTURED
 *  Stalled/waiting state: BLOCKED
 */
enum ExecState : unsigned int {
    INVALID,
    UNCAPTURED,
    RUNNING,
    QUEUED,
    BLOCKED,
    BLOCKED_TOABORT,
};

class ThreadState : public BaseEvent {
    public:
        static constexpr const char* stateNames[] = {
            "INVALID", "UNCAPTURED", "RUNNING",
            "QUEUED",  "BLOCKED",    "BLOCKED_TOABORT"};

        // Preset pointers for task execution; must be set non-speculatively
        uintptr_t finishPc, abortPc, donePc;

        // Checkpointed rsp (0 if not checkpointed)
        uintptr_t rspCheckpoint;

        TaskPtr task;
        ExecState state;
        ThreadID tid;
        Core* core;
        bool setGvtInProgress;

    private:
        // Records the number of levels of privileged execution the thread is
        // currently on. A thread in privileged execution can be stripped of
        // its task and continue running.
        uint64_t privDepth;

        // Records whether this run of privileged execution can be aborted.
        // This is set on the first privileged call in the run (when privDepth
        // 0->1). It is false if the call is from outside a task or an
        // irrevocable task, true otherwise. If privAbortable == true and we
        // have no task, this run has been aborted and abort handlers should be
        // executed on each priv_ret.
        bool privAbortable;

        // Retains the hint of the task that called priv mode. Needed for
        // RUNONABORT && SAMEHINT tasks that are enqueued by doomed priv code.
        uint64_t privTaskHint;

    public:
        ThreadState() : BaseEvent(0), state(INVALID), setGvtInProgress(false), privDepth(0), privAbortable(false), privTaskHint(0) {}

        // Controlled transitions according to execution FSM
        void transition(ExecState newState) {
            DEBUG("[%u] Transition from %s to %s", tid,
                  stateNames[state], stateNames[newState]);
            switch(state) {
                case INVALID:           assert(matchAny(newState, UNCAPTURED));                                           break;
                case UNCAPTURED:        assert(matchAny(newState, QUEUED, RUNNING));                                      break;
                case RUNNING:           assert(matchAny(newState, QUEUED, BLOCKED, BLOCKED_TOABORT, UNCAPTURED));         break;
                case QUEUED:            assert(matchAny(newState, RUNNING, BLOCKED, BLOCKED_TOABORT));                    break;
                case BLOCKED:           assert(matchAny(newState, QUEUED));                                               break;
                case BLOCKED_TOABORT:   assert(matchAny(newState, QUEUED));                                               break;
                default:                panic("Unknown thread state");                                                    break;
            }
            state = newState;
        }

        // PrioQueue-related interface
        void setWakeupCycle(uint64_t cycle) {
            // assert(!isQueued());
            // assert(cycle_ <= cycle);
            cycle_ = cycle;
        }

        // Privileged-mode interface
        void privCall() {
            if (privDepth == 0) {
                assert(privAbortable == false);
                assert(!privTaskHint);
                assert(task);
                if (!task->isIrrevocable()) {
                    privAbortable = true;
                    privTaskHint = task->hint;
                }
            }
            privDepth++;
        }

        bool privRet() {
            assert(privDepth > 0);
            assert(privAbortable || (task && task->isIrrevocable()));
            privDepth--;
            if (privDepth == 0) {
                bool wasAbortable = privAbortable;
                privAbortable = false;
                privTaskHint = 0;
                return wasAbortable;
            } else {
                return false;  // more priv code to run
            }
        }

        inline bool isPrivDoomed() const {
            return privDepth && privAbortable && (!task || task->hasPendingAbort());
        }

        inline bool isPriv() const { return privDepth; }

        inline uint64_t getPrivTaskHint() const { return privTaskHint; }

        // Debugging methods
        std::ostream& operator<<(std::ostream& os) const;

        std::string toString() const {
            std::stringstream ss;
            this->operator<<(ss);
            return ss.str();
        }
} ATTR_LINE_ALIGNED;
