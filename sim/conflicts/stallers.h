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

#include <unordered_map>
#include "sim/memory/memory_hierarchy.h"
#include "sim/task.h"
#include "sim/taskobserver.h"

#define DEBUG_STALL(args...)  //info(args)

class Staller {
  public:
    // A latter task is trying to access data already in this task's read or
    // write set. Stall latter task, or forward data and make it dependent?
    virtual bool shouldStall(Address lineAddr, bool exclReq,
                             Task& task, bool isWriter) = 0;
    virtual void recordAccess(const Task& task, Address lineAddr) {}
    virtual void recordForwardingAbort(Address lineAddr, bool exclReq,
                                       const Task& requester, bool isWriter) {}
};

class NeverStall : public Staller {
  public:
    bool shouldStall(Address lineAddr, bool exclReq, Task& task,
                     bool isWriter) override {
        return false;
    }
};

class AlwaysStall : public Staller {
  public:
    bool shouldStall(Address lineAddr, bool exclReq, Task& task,
                     bool isWriter) override {
        return task.hasPendingAbort() || isWriter || exclReq;
    }
};

class StallIfRunning : public Staller {
  public:
    bool shouldStall(Address lineAddr, bool exclReq, Task& task,
                     bool isWriter) override {
        return task.hasPendingAbort() || (task.running() && (isWriter || exclReq));
    }
};

// Saturating counter --- simple helper class for adaptive stalling, though it
// may be needed elsewhere, move if so
template <uint32_t MAX> class SatCounter {
  private:
    uint32_t val;

  public:
    SatCounter() : val(0) {}
    inline void inc() {
        if (val < MAX) val++;
    }
    inline void dec() {
        if (val) val--;
    }
    inline bool pred() const { return val >= (MAX + 1) / 2; }
};

// CIST-based staller from WaitNGoTM, Safri et al. ASPLOS 2013
// (simpler WaitNGo-wait variant, so we don't need to identify CISTend etc)
class AdaptiveStaller : public Staller {
  private:
    struct ConflictEntry {
        SatCounter<3> count;
        //SatCounter<3> rdCount; //TODO (See Section 2.2.5)
    };
    std::unordered_map<Address, ConflictEntry> conflictTable;
    std::unordered_map<const Task*, std::set<Address>> accTable;

  public:
    bool shouldStall(Address lineAddr, bool exclReq, Task& task,
                     bool isWriter) override {
        if (task.hasPendingAbort() && isWriter) return true;
        if (!task.running()) return false;
        auto cit = conflictTable.find(lineAddr);
        if (cit == conflictTable.end()) return false;
        ConflictEntry& e = cit->second;
        if (!e.count.pred()) return false;
        auto ait = accTable.find(&task);
        if (ait != accTable.end()) {
            ait->second.insert(lineAddr);
        } else {
            accTable.insert({&task, {lineAddr}});
            task.registerObserver(
                std::make_unique<StallerObserver>(task, *this));
        }
        DEBUG_STALL("Stalling on %lx", lineAddr);
        return true;
    }

    void recordAccess(const Task& task, Address lineAddr) override {
        auto ait = accTable.find(&task);
        if (ait == accTable.end()) return;
        auto it = ait->second.find(lineAddr);
        if (it != ait->second.end()) ait->second.erase(it);
    }

    void recordForwardingAbort(Address lineAddr, bool exclReq,
                               const Task& requester, bool isWriter) override {
        DEBUG_STALL("Preventable abort on %lx", lineAddr);
        auto cit = conflictTable.find(lineAddr);
        if (cit == conflictTable.end())
            cit = conflictTable.insert({lineAddr, ConflictEntry()}).first;
        assert(cit->first == lineAddr);
        cit->second.count.inc();
    }

  private:
    class StallerObserver : public TaskObserver {
      private:
        Task& task;
        AdaptiveStaller& staller;
      public:
        StallerObserver(Task& task, AdaptiveStaller& staller)
            : task(task), staller(staller) {}

        void start(ThreadID) override {}
        void commit() override {}
        void finish() override {
            staller.recordFinish(task);
            task.unregisterObserver(this);
        }

        void abort(bool requeue) override {
            staller.recordAbort(task);
            task.unregisterObserver(this);
        }
    };

    // Remove our accTable entry, but don't act on its information (no training
    // when we abort)
    void recordAbort(const Task& task)  {
        auto ait = accTable.find(&task);
        if (ait != accTable.end()) accTable.erase(ait);
    }

    // Unlearn lines that were not accessed before commit
    void recordFinish(const Task& task) {
        auto ait = accTable.find(&task);
        if (ait == accTable.end() || task.hasPendingAbort()) return;
        for (Address lineAddr : ait->second) {
            auto cit = conflictTable.find(lineAddr);
            if (cit == conflictTable.end()) continue;
            cit->second.count.dec();
            DEBUG_STALL("Needless stall on %lx", lineAddr);
        }
        accTable.erase(ait);
    }
};
