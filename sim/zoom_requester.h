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

#include <unordered_set>
#include <vector>

#include "sim/spin_cv.h"
#include "sim/str.h"
#include "sim/taskptr.h"
#include "sim/timestamp.h"
#include "sim/types.h"

class Core;
class ROB;
class Task;

class ZoomRequester {
  public:
    ZoomRequester(uint32_t idx_, const ROB& rob_)
        : name_("zr-" + Str(idx_)),
          rob(rob_) {}

    bool requestZoomInIfNeeded(const ConstTaskPtr& t, ThreadID tid);
    bool requestZoomOutIfNeeded(const ConstTaskPtr& t, ThreadID tid);

    TimeStamp getZoomInRequesterTs() const;
    TimeStamp getZoomOutRequesterTs() const;
    void notifyMaxDepthIncreased();
    void notifyMinDepthDecreased();
    void notifyAbort(const ConstTaskPtr& task, ThreadID tid);

  private:
    const std::string name_;
    const ROB& rob;

    // If both IN and OUT requests are active
    // simultaneously, one of them will definitely be
    // killed (depending on which is appropriate "earlier").
    inline bool requestingZoomIn() const {
        return zoomInRequesterTs != INFINITY_TS;
    }
    inline bool requestingZoomOut() const {
        return zoomOutRequesterTs != INFINITY_TS;
    }
    TimeStamp zoomInRequesterTs = INFINITY_TS;
    TimeStamp zoomOutRequesterTs = INFINITY_TS;

    std::unordered_set<ConstTaskPtr> tasksWaitingForZoomIn;
    std::unordered_set<ConstTaskPtr> tasksWaitingForZoomOut;

    SPinConditionVariable zoomInBlockedThreads;
    SPinConditionVariable zoomOutBlockedThreads;

    inline const char* name() const { return name_.c_str(); }

    enum class ZoomCompleted { IN, OUT };
    void wakeupAllPossibleThreads(ZoomCompleted type);
};
