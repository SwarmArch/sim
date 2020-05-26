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

#include <ctime>

#include "sim/timestamp.h"

/* Launch this thread at the beginning of simulation. It will periodically wake
 * up & print a hearbeat file. This file keeps us alive.
 */
void WatchdogThread(void*);
void StopWatchdogThread();

/* Non-thread interface */
void InitWatchdog(time_t cycleTimeout, time_t taskTimeout, time_t gvtTimeout, time_t ffTimeout);
void TickWatchdog();

// Thread-safe method to inform the watchdog of a GVT update.
void UpdateWatchdogGvt(TimeStamp newGvt);
