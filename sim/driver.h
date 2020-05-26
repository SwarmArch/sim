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

#include <tuple>

#include "sim/sim.h"
#include "sim/thread.h"
#include "spin.h"

// NOTE: Include this from sim.cpp only. driver.cpp and sim.cpp are fairly
// tightly integrated (they started as a single file), but only a tiny fraction
// of the driver code is visible to other entities.

spin::ThreadId NextThread();
std::tuple<spin::ThreadId, bool> SwitchThread(spin::ThreadId tid);
spin::ThreadId PreventFfStarvation(spin::ThreadId tid);

void Capture(spin::ThreadId tid, bool runsNext);
spin::ThreadId Uncapture(spin::ThreadId tid, spin::ThreadContext* tc);
void BlockThreadAfterSwitch(ExecState state);
void BlockThread(uint32_t tid, ExecState state);  // works for curThread too
void UnblockThread(uint32_t tid, uint64_t cycle);

void ThreadStart(spin::ThreadId tid);
void ThreadFini(spin::ThreadId tid);

uint32_t GetCurTid();
ThreadState* GetCurThread();
ThreadState* GetThreadState(uint32_t tid);
uint32_t GetCoreIdx(uint32_t tid);

bool IsInFastForward();
void ExitFastForward();
void EnterFastForward();

void InitDriver();
