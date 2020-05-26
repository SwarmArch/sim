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

#include "sim/object.h"
#include "sim/taskptr.h"
#include "sim/gvt_arbiter.h"

class TSB;

// Interfaces to a TimeStamp-ordered priority queue structure
// for tasks created while fast-forwarding (e.g., initial tasks
// enqueued before the ROI, or all tasks that are created during
// heartbeats-based fast-forwarding).

// Interface during steady-state fast-forwarding
void EnqueueFfTask(TaskPtr&& t);
TaskPtr DequeueFfTask();
bool IsFfQueueEmpty();

// For transitioning out of fast-forwarding into full simulation.
void DrainFfTaskQueue();  // Begins draining tasks to TSB #0
void FfTaskQueueNotifyZoomIn(uint32_t newFrameBaseDepth);
void FfTaskQueueSetPort(LVTUpdatePort* port);
void FfTaskQueueSendLvt(uint64_t epoch); // analogous to ROB::sendLvt()

// Dummy network object so we can send LVTUpdateMsgs to GVTArbiter
class FfTaskQueue : public SimObject {
  public:
    FfTaskQueue() : SimObject("ffTaskQueue") { }
};
