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

/* Tracing infrastructure. Designed to be:
 * (a) Detailed: Produces full-fledged, heavyweight event traces that aid
 *     understanding and optimizing applications (e.g., producing full
 *     distributions of task lengths, tracing back abort causes to specific data
 *     and PCs, narrowing down to specific executions, etc.)
 * (b) Zero overhead when disabled. Therefore, this code is enabled via a define,
 *     and all interface functions are stubs.
 */

#ifndef ENABLE_TRACING
// All functions are empty stubs
#define FNATTR inline
#define FNDECL {}
#else
// All functions defined in cpp
#define FNATTR
#define FNDECL
#endif

#include <stdint.h>

class Task;

namespace trace {

FNATTR void init() FNDECL;
FNATTR void fini() FNDECL;

// Task lifetime profiling
FNATTR void taskCreated(const Task* t, const Task* parent = nullptr) FNDECL;
FNATTR void taskStart(const Task* t, uint32_t tid) FNDECL;
FNATTR void taskFinish(const Task* t) FNDECL;
FNATTR void taskCommitted(const Task* t) FNDECL;
FNATTR void taskAborted(const Task* t, bool requeue) FNDECL;
FNATTR void taskSpilled(const Task* t, const Task* spiller) FNDECL;
FNATTR void taskStolen(const Task* oldTask, const Task* newTask) FNDECL;
FNATTR void taskYield(const Task* oldTask, const Task* newTask) FNDECL;
FNATTR void taskDiscarded(const Task* t) FNDECL;

// Task abort profiling
FNATTR void dataAbort(const Task* t, const Task* aborter, uintptr_t lineAddress, bool isWrite, bool isWriter) FNDECL;
FNATTR void childAbort(const Task* t) FNDECL;
// TODO: More detail (resource type, aborter)
FNATTR void resourceAbort(const Task* t) FNDECL;
FNATTR void zoomingAbort(const Task* t) FNDECL;

};

#undef FNATTR
#undef FNDECL
