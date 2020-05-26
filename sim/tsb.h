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

#include <queue>
#include <stdint.h>
#include <vector>
#include "sim/assert.h"
#include "sim/event.h"
#include "sim/log.h"
#include "sim/match.h"
#include "sim/object.h"
#include "sim/port.h"
#include "sim/spin_cv.h"
#include "sim/stats/stats.h"
#include "sim/task.h"

#define DEBUG_TSB(args...) //info(args)

struct TaskEnqueueReq {
    TaskPtr task;
    // All data below is for convenience (e.g., tsbPos ~ parent pointer)
    uint32_t robIdx;
    uint32_t tsbIdx;
    uint32_t tsbPos;
    // The task may be untied for enqueuing purposes, but it is helpful to know
    // the source parent for assertions and task queue invariants for
    // spillers.
    TaskPtr parent;

    bool isSpillerChild() const { return parent && parent->isSpiller(); }
    uint32_t bytes() const {
        // dsm: Assuming that we build a small tables of task pointers, then
        //      send the id w/ a few bits
        // Timestamp is 64-bits, no tiebreaker is sent.
        // Send the parent's tile ID (e.g. <= 8 bits) so the receiver can ack.
        return 8 /* timestamp */
            + 2  /* 16-bit hint hash */
            + 1  /* parent's tile ID */
            + 1  /* fcn ptr bits, few bits for tiedness, spiller child, etc */
            + (8 * task->args.size());
    }
};

struct TaskEnqueueResp {
    bool isAck;
    uint32_t robIdx;
    uint32_t tsbPos;
    uint32_t bytes() const {
        // Acks have child pointer
        return (isAck? 4 : 2);
    }
};

typedef Port<TaskEnqueueReq> ROBTaskPort;
typedef Port<TaskEnqueueResp> TSBPort;

class TSB;
typedef bool (*TSBFillCallback)(TSB*);  // returns true if done

class Task;

class TSB : public SimObject, public TSBPort::Receiver {
  private:
    enum class Status { IDLE, BLOCKED, SENT, SENT_ABORTED, HELD };
    typedef std::vector<uint32_t> FreeList;
    struct TSBEntry {
        TaskEnqueueReq req;
        TimeStamp ts;  // copied to compute lvt of SENT tasks
        uint32_t cid;
        Status status;
        uint64_t allocCycle;
        FreeList* freeList;
        TSBEntry() : ts(ZERO_TS), status(Status::IDLE), freeList(nullptr) {}
    };

    std::vector<TSBEntry> queue;

    // The TSB avoids deadlock/starvation in multiple circumstances by
    // reserving slots to tasks that meet different criteria.  These are all
    // implemented through multiple freelists.  The policy is to grab slots
    // from freelists and release them to the same freelists.
    // Crucially, tasks in high-priority slots are prioritized for sending
    // in sendBlockedTask().

    // [victory] This single slot for children of spillers is necessary to
    // prevent spillers from being blocked by other tasks. It is essential
    // that not even the GVT or other irrevocable tasks can block spillers.
    // Spillers must be prioritized over all other local tasks even if their
    // children have later timestamps, because otherwise the GVT task, which
    // may be at the same or another tile, could be blocked indefinitely on an
    // enqueue because task queues & buffers in this tile remain full, or the
    // GVT task could be blocked on a zoom-in request while tasks going out of
    // frame wait to be frame-spilled.  Based on a discussion with Daniel, it
    // doesn't seem like we need separate slots reserved for frame spillers:
    // if we guarantee *some* spiller can always make forward progress, the
    // set of all spillers should eventually progress and finish their jobs.
    // Also, while ordinary spillers often have lower timestamps, they can't
    // indefinitely block frame spillers, which are more critical to
    // prioritize: if there is a need for frame spillers, the ROB stops
    // dispatching ordinary spillers.
    FreeList spillerFreeList;

    // This single slot was originally intended to guarantee forward progress
    // of the GVT task, since it can enqueue untied into this slot, and then
    // the TSB could prioritize sending from this slot to the destination ROB
    // to free this slot to be reused by the GVT task.  Since then, we have
    // added support for more irrevocable tasks that all enqueue untied, like
    // requeuers and the parallel irrevocable tasks introduced by Espresso.  We
    // need to review what use of this slot makes sense.  Perhaps requeuers
    // should *not* be prioritized and should not have access to this slot,
    // since they can always yield if the TSB is full.
    FreeList untiedFreeList;

    // This per-core slot will prevent starvation and improve fairness (not
    // related to guaranteeing forward progress).
    std::vector<FreeList> coreFreeLists;

    // This holds all remaining slots not reserved for any special purpose.
    FreeList commonFreeList;

    std::vector<ROBTaskPort*> ports;
    std::vector<std::set<uint32_t>> blockedPortTasks;
    std::vector<uint32_t> pendingUnblocks;
    std::vector<uint32_t> nextBackoffDelays;
    std::vector<uint32_t> coreOccs;
    SPinConditionVariable waiters;
    const uint32_t selfId;
    const uint32_t firstCore;
    const uint32_t minBackoffDelay;
    const uint32_t maxBackoffDelay;

    TSBFillCallback fillCallback;

    // Stats
    RunningStat<size_t> cyclesPerEnqueue;
    Counter enqueues, aborts, acks, nacks;


  public:
    TSB(const std::string& _name, uint32_t _selfId, uint32_t _entries,
        uint32_t _minBackoff, uint32_t _maxBackoff, uint32_t coresPerTSB)
        : SimObject(_name),
          queue(_entries),
          coreFreeLists(coresPerTSB),
          selfId(_selfId),
          firstCore(selfId * coresPerTSB),
          minBackoffDelay(_minBackoff),
          maxBackoffDelay(_maxBackoff),
          fillCallback(nullptr)
    {
        assert(queue.size() >= coresPerTSB + 2);
        // One (for now) slot for each core and one for a spiller task
        uint32_t pos = 0;
        spillerFreeList.push_back(pos++);
        untiedFreeList.push_back(pos++);
        for (auto& fl : coreFreeLists) fl.push_back(pos++);
        while (pos < queue.size()) commonFreeList.push_back(pos++);
        assert(pos == queue.size());
        assert(maxBackoffDelay >= minBackoffDelay);
    }

    void initStats(AggregateStat* parentStat) {
        AggregateStat* tsbStat = new AggregateStat();
        tsbStat->init(name(), "TSB stats");

        cyclesPerEnqueue.init("cyclesPerEnqueue", "cycles from alloc to ACK");
        enqueues.init("enqueues", "enqueues from cores");
        aborts.init("aborts", "aborts of tasks in the TSB or in transit");
        acks.init("acks", "ACKs from ROBs");
        nacks.init("nacks", "NACKs from ROBs");

        tsbStat->append(&cyclesPerEnqueue);
        for (Stat* s : {&enqueues, &aborts, &acks, &nacks}) tsbStat->append(s);
        parentStat->append(tsbStat);
    }

    void setPorts(const std::vector<ROBTaskPort*> _ports) {
        assert(!ports.size() && _ports.size());
        ports = _ports;
        blockedPortTasks.resize(ports.size());
        pendingUnblocks.resize(ports.size(), 0);
        nextBackoffDelays.resize(ports.size(), minBackoffDelay);
    }

    bool reserve(ThreadID tid, uint32_t cid, bool isSpillerChild, bool isTied) {
        if (freeList(cid, isSpillerChild, isTied).empty()) {
            DEBUG_TSB("[%s] full for tid %d cid %d%s %stied",
                      name(), tid, cid,
                      isSpillerChild? " spiller's child" : "",
                      isTied? "" : "un");
            return false;
        }
        return true;
    }

    void block(ThreadID tid) { waiters.addWaiter(tid); }

    void enqueue(const TaskPtr& t, uint32_t robIdx, uint32_t cid,
                 const TaskPtr& parent, bool holdTask, uint64_t cycle) {
        TaskEnqueueReq req = {t, robIdx, selfId, -1u, parent};

        FreeList& fl = freeList(cid, req.isSpillerChild(), t->isTied());
        uint32_t tsbPos = allocPos(fl);
        req.tsbPos = tsbPos;

        DEBUG_TSB("[%s] [%ld] enqueue to rob %d pos %d task %s parent %s",
                  name(), cycle, robIdx, tsbPos, t->toString().c_str(),
                  parent? parent->toString().c_str() : "none"
                  );

        assert(queue[tsbPos].status == Status::IDLE);
        // [mcj] I concede that this copy is wasteful, but it lets us re-use the
        // TaskEnqueueReq::isSpillerChild() method.
        queue[tsbPos].req = req;
        queue[tsbPos].ts = t->ts;
        queue[tsbPos].cid = cid;
        queue[tsbPos].allocCycle = cycle;
        // The ROB has a slot reserved for spiller's children; just because
        // other tasks are blocked on the ROB, the spiller slot might be
        // available. This message doesn't go over the network, so there is
        // little down side.
        if (holdTask) {
            DEBUG_TSB("[%s] holding %s", name(), t->toString().c_str());
            queue[tsbPos].status = Status::HELD;
        } else if (req.isSpillerChild() || !blockedTasks(robIdx)) {
            sendTask(tsbPos, cycle);
        } else {
            blockTask(tsbPos, cycle);
        }
        enqueues.inc();
    }

    void receive(const TaskEnqueueResp& resp, uint64_t cycle) override {
        uint32_t robIdx = resp.robIdx;
        uint32_t tsbPos = resp.tsbPos;
        assert(tsbPos < queue.size());
        auto& e = queue[tsbPos];
        assert(tsbPos == e.req.tsbPos);
        assert(matchAny(e.status, Status::SENT, Status::SENT_ABORTED));
        DEBUG_TSB("[%s] [%ld] recv rob %d pos %d ack %d ubs %d free %ld",
                  name(), cycle, robIdx, tsbPos, resp.isAck,
                  pendingUnblocks[robIdx], queue.size());
        assert(pendingUnblocks[robIdx]);
        pendingUnblocks[robIdx]--;
        if (resp.isAck) {
            nextBackoffDelays[robIdx] = minBackoffDelay;
            // Send previously-blocked tasks. Send two, if available, to start
            // exponential growth of unblocking tasks.
            if (blockedTasks(robIdx)) {
                sendBlockedTask(robIdx, cycle);
                if (blockedTasks(robIdx)) sendBlockedTask(robIdx, cycle + 1);
            }
            freePos(tsbPos);
            acks.inc();
            assert(cycle >= e.allocCycle);
            cyclesPerEnqueue.push(cycle - e.allocCycle);
        } else {
            // Grow exponentially by 41% = sqrt(2); divide by 1024 for speed
            nextBackoffDelays[robIdx] = std::min(
                maxBackoffDelay, 1448 * nextBackoffDelays[robIdx] / 1024);
            if (e.status == Status::SENT) {
                blockTask(tsbPos, cycle);
            } else {
                freePos(tsbPos);
                schedUnblockEvent(robIdx, cycle);
            }
            nacks.inc();
        }
    }

    void wakeup(ThreadID tid) {
        waiters.notify(tid);
    }

    TimeStamp lvt() const {
        TimeStamp ts = INFINITY_TS;
        for (uint32_t i = 0; i < queue.size(); i++) {
            if (queue[i].status != Status::IDLE && queue[i].ts < ts) {
                ts = queue[i].ts;
            }
        }
        DEBUG_TSB("[%s] lvt %s", name(), ts.toString().c_str());
        assert(!ts.hasAssignedTieBreaker());
        return ts;
    }

    void abort(TaskPtr t) {
        assert(t->idle() && t->children.empty() && !t->abortedInTransit);
        for (uint32_t i = 0; i < queue.size(); i++) {
            if (queue[i].req.task == t) {
                auto& e = queue[i];
                DEBUG_TSB("[%s] from pos %d w status %d abort %s", name(),
                          i, e.status, t->toString().c_str());
                if (e.status == Status::BLOCKED) {
                    // Dispense with it
                    auto& bq = blockedPortTasks[e.req.robIdx];
                    assert(bq.count(i));
                    bq.erase(i);
                    freePos(i);
                } else if (e.status == Status::SENT) {
                    t->abortedInTransit = true;
                    e.status = Status::SENT_ABORTED;
                    e.req.task = nullptr;
                } else {
                    panic("Invalid abortable state %d", queue[i].status);
                }
                aborts.inc();
                t->abort(Task::AbortType::PARENT);
                return;
            }
        }
        assert(false);  // should not get here
    }

    /* Actions for held tasks */

    void discard(TaskPtr t) {
        DEBUG_TSB("[%s] discarding %s", name(), t->toString().c_str());
        for (uint32_t i = 0; i < queue.size(); i++) {
            if (queue[i].req.task == t) {
                auto& e = queue[i];
                assert(e.status == Status::HELD);
                freePos(i);
                return;
            }
        }
        assert(false);  // should not get here
    }

    void release(TaskPtr t, uint64_t cycle) {
        DEBUG_TSB("[%s] releasing %s", name(), t->toString().c_str());
        for (uint32_t i = 0; i < queue.size(); i++) {
            if (queue[i].req.task == t) {
                auto& e = queue[i];
                assert(e.status == Status::HELD);
                e.status = Status::IDLE;
                if (blockedTasks(e.req.robIdx)) {
                    blockTask(i, cycle);
                } else {
                    sendTask(i, cycle);
                }
                return;
            }
        }
        assert(false);  // should not get here
    }

    void registerFillCallback(TSBFillCallback fn) {
        assert(!fillCallback);
        fillCallback = fn;
    }

  private:
    void sendTask(uint32_t tsbPos, uint64_t cycle) {
        assert(matchAny(queue[tsbPos].status, Status::IDLE));
        uint32_t robIdx = queue[tsbPos].req.robIdx;
        DEBUG_TSB("[%s] [%ld] send rob %d pos %d ts %s%s %stied", name(), cycle,
                  robIdx, tsbPos, queue[tsbPos].req.task->ts.toString().c_str(),
                  queue[tsbPos].req.isSpillerChild()? " spiller's child" : "",
                  queue[tsbPos].req.task->isTied()? "" : "un"
                  );
        ports[robIdx]->send(queue[tsbPos].req, cycle + 1);
        queue[tsbPos].status = Status::SENT;
        pendingUnblocks[robIdx]++;
    }

    void blockTask(uint32_t tsbPos, uint64_t cycle) {
        assert(matchAny(queue[tsbPos].status, Status::IDLE, Status::SENT));
        uint32_t robIdx = queue[tsbPos].req.robIdx;

        blockedPortTasks[robIdx].insert(tsbPos);
        queue[tsbPos].status = Status::BLOCKED;

        schedUnblockEvent(robIdx, cycle);
    }

    void schedUnblockEvent(uint32_t robIdx, uint64_t cycle) {
        if (!pendingUnblocks[robIdx]) {
            pendingUnblocks[robIdx]++;
            uint64_t delay = nextBackoffDelays[robIdx];
            assert(minBackoffDelay <= delay && delay <= maxBackoffDelay);
            schedEvent(cycle + delay, [this, robIdx](uint64_t cycle) {
                assert(pendingUnblocks[robIdx]);
                pendingUnblocks[robIdx]--;
                if (blockedTasks(robIdx)) sendBlockedTask(robIdx, cycle);
            });
        }
    }

    uint32_t blockedTasks(uint32_t robIdx) const {
        return blockedPortTasks[robIdx].size();
    }

    void sendBlockedTask(uint32_t robIdx, uint64_t cycle) {
        auto& bq = blockedPortTasks[robIdx];
        assert(bq.size());
        // Select task: spiller child more urgent than not,
        // untied more urgent than tied, break ties by task/parent timestamp
        uint32_t bestPos = *bq.begin();
        for (uint32_t pos : bq) {
            const TaskPtr& bt = queue[bestPos].req.task;
            const TaskPtr& t = queue[pos].req.task;
            if (queue[bestPos].req.isSpillerChild()) {
                // If bestPos is a spiller child, only replace with other
                // spiller children. Even if another non-spiller-child
                // candidate has a lower timestamp than bestPos, it is
                // imperative to let spillers enqueue their child and commit.
                // This releases the core and TSB slot for other spillers.
                if (queue[pos].req.isSpillerChild() && t->ts < bt->ts) {
                    bestPos = pos;
                }
            } else {
                // The order is important: spiller > untied > TS
                // [victory] Mark has repeatedly expressed doubts to me about
                //           whether this prioritization is worth the added
                //           complexity/area in a hardware implementation, and
                //           we should perhaps simplify it to prioritize the
                //           reserved slots.
                bool isMoreUrgent = (
                        queue[pos].req.isSpillerChild()
                        || (!t->isTied() && (bt->isTied() || t->ts < bt->ts))
                        || (t->isTied() && bt->isTied()
                            && t->parent->ts < bt->parent->ts)
                        );
                if (isMoreUrgent) bestPos = pos;
            }
        }
        bq.erase(bestPos);
        assert(robIdx == queue[bestPos].req.robIdx);
        assert(matchAny(queue[bestPos].status, Status::BLOCKED));
        queue[bestPos].status = Status::IDLE;
        sendTask(bestPos, cycle);
    }

    /* FreeList management */

    FreeList& freeList(uint32_t cid, bool isSpillerChild, bool isTied) {
        if (isSpillerChild && !spillerFreeList.empty()) {
            return spillerFreeList;
        }
        if (!isTied && !untiedFreeList.empty()) return untiedFreeList;
        uint32_t flIdx = cid - firstCore;
        assert(flIdx < coreFreeLists.size());
        if (!coreFreeLists[flIdx].empty()) return coreFreeLists[flIdx];
        return commonFreeList;
    }

    uint32_t allocPos(FreeList& fl) {
        assert(!fl.empty());
        uint32_t tsbPos = fl.back();
        fl.pop_back();
        queue[tsbPos].freeList = &fl;
        return tsbPos;
    }

    void freePos(uint32_t pos) {
        assert(pos < queue.size());
        auto& e = queue[pos];
        assert(e.freeList);
        e.status = Status::IDLE;
        e.req.task.reset();
        e.freeList->push_back(pos);
        e.freeList = nullptr;

        // If we have a fill callback, call it and deregister it if it's done.
        // NOTE: freePos() is called in the middle of TSB methods, so calling a
        // method that enqueues here may create odd chains of events, e.g.,
        // calls to reserve() and enqueue() from the callback in the middle of
        // a receive(). This should be safe but debug statements may seem
        // confusing. However, it's important to do this immediately because
        // the callback may hold the GVT task---so doing it through events may
        // create races.
        if (fillCallback) {
            bool done = fillCallback(this);
            if (done) fillCallback = nullptr;
        }

        // Do the simple thing: Wake all cores, so each checks whether it
        // can enqueue (if this causes too many wakeups, we can wakeup more
        // selectively based on the status of the freeLists.
        waiters.notifyAll();
    }
};
