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

#include "sim/task.h"
#include <algorithm>
#include <functional>
#include <string>
#include "sim/assert.h"
#include "sim/rob.h"
#include "sim/sim.h"
#include "sim/taskobserver.h"
#include "sim/taskprofilingsummary.h"
#include "sim/trace.h"

#undef DEBUG
#define DEBUG(args...) //info(args)

const uint32_t DepthCounters::MAX_DEPTH;
uint64_t Task::ORDINARY_SPILLER_PTR = 0ul;
uint64_t Task::ORDINARY_REQUEUER_PTR = 0ul;
uint64_t Task::FRAME_SPILLER_PTR = 0ul;
uint64_t Task::FRAME_REQUEUER_PTR = 0ul;
std::vector<Task::SpillerKind> Task::customSpillers;

Task::Task(uint64_t _taskFn, TimeStamp _ts, uint64_t _hint, bool _noHash,
           bool _nonSerialHint, bool _producer, bool _requeuer, uint64_t _softTs,
           RunCondition _runCond, bool _runOnAbort,
           const std::vector<uint64_t>& _args)
    : runningTid(INVALID_TID),
      container_(nullptr),
      taskFn(_taskFn),
      ts(_ts),
      args(_args),
      hint(_hint),
      noHashHint(_noHash),
      nonSerialHint(_nonSerialHint),
      producer(_producer),
      requeuer(_requeuer),
      runOnAbort(_runOnAbort),
      noTimestamp(_ts == NO_TS),
      softTs(_softTs),
      // Requeuers typically don't fit into the {MUST,MAY,CANT}SPEC
      // trichotomy.  We have an obscure "ANYTIME" category for these
      // always-runnable, non-speculative tasks.  However, in the case that a
      // requeuer wraps exclusively CANTSPEC tasks, there is no point in
      // running the requeuer very early, as its enclosed tasks cannot safely
      // run.  So, as an optimization, the requeuer may be tagged CANTSPEC to
      // delay its execution.
      runCond((isRequeuer() && _runCond != RunCondition::CANTSPEC)
              ? RunCondition::ANYTIME
              : _runCond),
      pendingStartAbort(nullptr),
      pendingFinishAbort(nullptr),
      state(IDLE),
      irrevocable(false),
      enqueuedChildToSuperdomain(false),
      specSyscall(false),
      deepenState(DeepenState::NO_SUBDOMAIN),
      createdDepth(0),
      startDepth(MAX32_T),
      startQuartile(MAX32_T),
      numAborts(0),
      depths(nullptr) {
    assert(isRequeuer() || !(isOrdinaryRequeuer() || isFrameRequeuer()));

    std::fill(minimumFinishAbortCycles.begin(),
              minimumFinishAbortCycles.end(),
              UINT64_MAX);
}

Task::~Task() {
    assert_msg(state == UNREACHABLE, "deleting reachable task %s",
               toString().c_str());
}

// dsm: We can't do this in the constructor anymore; can't call ptr() in the
// constructor, because the internal weak_ptr is not yet initialized
void Task::tie(TaskPtr _parent) {
    assert(!parent);
    parent = std::move(_parent);
    if (parent) parent->children.push_back(ptr());

    assert(createdDepth == 0);
    createdDepth = currentDepth();

    DEBUG("Tied task %s at depth: %u, parent: %s",
          toString().c_str(), createdDepth,
          parent ? parent->toString().c_str() : "none");
}

void Task::setDepthCounters(DepthCounters* dc) {
    assert(!depths);
    depths = dc;
    if (depths) depths->created.inc(std::min(createdDepth, depths->MAX_DEPTH));
}

void Task::start(ThreadID _runningTid) {
    assert(idle());
    assert(undoLog.empty());
    assert(irrevocable || runCond != RunCondition::CANTSPEC);
    assert(irrevocable || !runOnAbort);
    assert(!madeSpeculativeSyscall());

    state = RUNNING;
    runningTid = _runningTid;
    visitObservers([_runningTid](TaskObserver* o) { o->start(_runningTid); });
    if (!irrevocable) undoLog.init(GetCoreIdx(runningTid));

    startDepth = currentDepth();
    startQuartile = container()->cqQuartile(ts);
    assert(startDepth <= createdDepth);
    DEBUG("Starting task %s at depth: %u", toString().c_str(), startDepth);
    if (depths) {
        depths->start.inc(std::min(startDepth, depths->MAX_DEPTH));
        depths->startQuartile.inc(std::min(startQuartile, depths->MAX_DEPTH));
    }
    trace::taskStart(this, runningTid);
}

void Task::finish() {
    assert(running());
    // A speculative system call precludes finishing successfully
    assert(!madeSpeculativeSyscall() || hasPendingAbort());
    state = COMPLETED;
    runningTid = INVALID_TID;
    specSyscall = false;
    visitObservers([](TaskObserver* o) { o->finish(); });
    const uint32_t finishDepth = currentDepth();
    DEBUG("Finishing task %s at depth: %u", toString().c_str(), finishDepth);
    if (depths)
        depths->finish.inc(std::min(finishDepth, depths->MAX_DEPTH));
    trace::taskFinish(this);
}

void Task::commit() {
    assert(!hasPendingAbort());
    assert(!abortedInTransit);
    assert(completed());
    assert(!exceptioned());
    assert(runningTid == INVALID_TID);
    assert(!isTied());
    assert(children.empty());
    assert(!madeSpeculativeSyscall());
    DEBUG("Committing task %s, current depth: %u",
          toString().c_str(), currentDepth());

    if (irrevocable) {
        assert(undoLog.empty());
        assert(allocCtxt.empty());
    } else {
        undoLog.clear();
        allocCtxt.handleCommit();
    }

    // FIXME(dsm): The recordAsAborted magic op shouldn't exist, and this
    // flag/logic should be removed.
    if (recordAsAborted) {
        visitObservers([](TaskObserver* o) { o->abort(false); });
    } else {
        visitObservers([](TaskObserver* o) { o->commit(); });
    }

    if (depths) {
        depths->successfulCreated.inc(
            std::min(createdDepth, depths->MAX_DEPTH));
        depths->successfulStart.inc(std::min(startDepth, depths->MAX_DEPTH));
        depths->successfulStartQuartile.inc(
            std::min(startQuartile, depths->MAX_DEPTH));
    }

    trace::taskCommitted(this);
    state = UNREACHABLE;
}

void Task::makeIrrevocable() {
    assert(!irrevocable);
    // There should be no need to mark a completed task irrevocable. Even if it
    // finished and aborted, makeIrrevocable() should be called after abort().
    assert(matchAny(state, IDLE, RUNNING));
    assert(!hasPendingAbort());
    assert(!abortedInTransit);
    assert(!isTied());
    assert(children.empty());
    assert(!madeSpeculativeSyscall());
    undoLog.clear();
    allocCtxt.makeIrrevocable();
    irrevocable = true;
}

void Task::cutTie() {
    assert(isTied());
    assert(state != UNREACHABLE);
    // FIXME(mcj) I wish children was a set as it used to be. Oh well.
    // assert(parent->children.count(this) > 0);

    // The parent is responsible to remove this task from its children set
    parent = nullptr;
}

uint32_t Task::currentDepth() const {
    // N.B. an optimization: exit the loop when depth >= MAX_DEPTH
    // I didn't bother with that, hoping that we could decouple task structure
    // from depth profiling. Technically this method currentDepth() is agnostic
    // to all the depth profiling variables.
    uint32_t depth = 0;
    Task* ancestor = parent.get();
    while (ancestor) {
        depth++;
        ancestor = ancestor->parent.get();
    }
    return depth;
}

void Task::ffStart(ThreadID _runningTid) {
    assert(idle());
    makeIrrevocable();
    state = RUNNING;
    runningTid = _runningTid;
}

void Task::ffFinish() {
    assert(running());
    assert(!hasPendingAbort());
    state = UNREACHABLE;
    runningTid = INVALID_TID;
}

void Task::abort(AbortType type) {
    assert(!irrevocable);
    // Remove abort event, and check the type is right. The task may have a
    // pendingStartAbort and not a pendingFinishAbort if the finishAbort event
    // would have been at the same cycle as the start. This is an optimization
    uint32_t prio = (uint32_t)type;
    assert((pendingFinishAbort == nullptr) ^ (pendingStartAbort == nullptr));
    Event* ev = pendingFinishAbort ? pendingFinishAbort : pendingStartAbort;
    assert(ev && ev->cycle() == minimumFinishAbortCycles[prio]);
    pendingStartAbort = nullptr;
    pendingFinishAbort = nullptr;
    minimumFinishAbortCycles[prio] = UINT64_MAX;
    for (uint32_t p = 0; p <= prio; p++) {
        assert(minimumFinishAbortCycles[p] == UINT64_MAX);
    }

    // General actions (for all abort types)
    runningTid = INVALID_TID;
    numAborts++;

    // Fractal time (dsm): Undo possible timestamp deepenings
    ts.undeepen();
    enqueuedChildToSuperdomain = false;
    deepenState = DeepenState::NO_SUBDOMAIN;

    // [mcj] the AbortHandler should have already walked the undo log, and
    // ConflictResolution should have already cleared the children set.
    assert(undoLog.empty());
    assert(children.empty());
    assert(!running());
    allocCtxt.handleAbort();
    visitObservers([type](TaskObserver* o) { o->abort(type != AbortType::PARENT); });

    specSyscall = false;

    // We should not update createdDepth here
    // 1. If this task is not requeued, then it will be
    // deleted, in which case createdDepth does not matter.
    // 2. If task is requeued, then we are retaining
    // this task, so it must retain its createdDepth
    // history.
    if (hasContainer() && depths) {
        depths->abortedStart.inc(std::min(startDepth, depths->MAX_DEPTH));  // Tasks that have never begun (ancestral abort) will not reach here.
        depths->abortedStartQuartile.inc(std::min(startQuartile, depths->MAX_DEPTH));
    }

    trace::taskAborted(this, type != AbortType::PARENT);
    startQuartile = MAX32_T;

    // [mcj] While you might think that, because the undoLog is empty (above),
    // this clear is unnecessary, you'd be wrong, because clear() has other side
    // effects. Say no to static variables and their 'init' functions, kids.
    undoLog.clear();

    // Type-dependent actions
    switch (type) {
        case AbortType::EXCEPTION:
            assert(completed());
            state = EXCEPTIONED;
            assert(exceptionReason.size());
            break;
        case AbortType::HAZARD:
            assert(state != IDLE && state != UNREACHABLE);
            state = IDLE;
            break;
        case AbortType::PARENT:
            assert(!isTied()); // Should have cut ties when receiving the msg
            assert(state != UNREACHABLE);
            state = UNREACHABLE;
            if (hasContainer() && depths) {
                assert(createdDepth != 0);
                depths->abortedCreated.inc(std::min(createdDepth, depths->MAX_DEPTH));
            }
            break;
        default:
            panic("Invalid AbortType");
    }
}

bool Task::isAborting(uint64_t cycle) const {
    return pendingFinishAbort
            || (pendingStartAbort && pendingStartAbort->cycle() <= cycle);
}

void Task::recordStartAbortEvent(Event* ev) {
    assert(!isAborting(ev->cycle()));
    if (pendingStartAbort) {
        assert(pendingStartAbort->cycle() > ev->cycle());
        // FIXME(dsm): Formalize ownership rules or use unique_ptr!!
        eraseEvent(pendingStartAbort);
        delete pendingStartAbort;
        pendingStartAbort = nullptr;
    }
    assert(!pendingStartAbort && !pendingFinishAbort);
    pendingStartAbort = ev;
}

void Task::recordFinishAbortCycle(AbortType type, uint64_t cycle) {
    uint32_t prio = (uint32_t) type;
    for (uint32_t p = prio; p < minimumFinishAbortCycles.size(); p++) {
        if (cycle >= minimumFinishAbortCycles[p]) return;
    }

    // We can pull the min finish abort cycle lower
    for (uint32_t p = 0; p < prio; p++) {
        if (cycle < minimumFinishAbortCycles[p]) {
            minimumFinishAbortCycles[p] = UINT64_MAX;
            if (p == (size_t) AbortType::EXCEPTION) exceptionReason = "";
        }
    }
    minimumFinishAbortCycles[prio] = cycle;
}

Task::AbortType Task::abortTypeBefore(uint64_t cycle) {
    auto it = std::find_if(minimumFinishAbortCycles.rbegin(),
                           minimumFinishAbortCycles.rend(),
                           [=] (uint64_t c) { return c <= cycle; });
    assert(it != minimumFinishAbortCycles.rend());
    std::fill(it + 1, minimumFinishAbortCycles.rend(), UINT64_MAX);
    *it = cycle;

    uint32_t prio = std::distance(it + 1, minimumFinishAbortCycles.rend());
    if ((uint32_t)AbortType::EXCEPTION < prio) exceptionReason = "";

    return (Task::AbortType) prio;
}

uint64_t Task::nextAbortCycle() const {
    auto it = std::find_if(minimumFinishAbortCycles.begin(),
                           minimumFinishAbortCycles.end(),
                           [] (uint64_t c) { return c != UINT64_MAX; });
    if (it != minimumFinishAbortCycles.end()) {
        return *it;
    } else {
        return UINT64_MAX;
    }
}


void Task::recordFinishAbortEvent(Event* ev) {
    assert(isAborting(ev->cycle()));
    assert(!pendingFinishAbort);
    assert(!pendingStartAbort || pendingStartAbort->cycle() <= ev->cycle());

    pendingStartAbort = nullptr;
    pendingFinishAbort = ev;
}


void Task::deepen(uint64_t maxTs) {
    ts.deepen(maxTs);
    assert_msg(deepenState == DeepenState::NO_SUBDOMAIN,
               "DEEPEN called after UNDEEPEN!");
    deepenState = DeepenState::DEEPENED;
}

void Task::undeepen() {
  assert_msg(deepenState == DeepenState::DEEPENED,
             "UNDEEPEN called without matching DEEPEN.");
  deepenState = DeepenState::UNDEEPENED;
  ts.undeepen();
}


std::ostream& Task::operator<<(std::ostream& os) const {
    std::string tid = (runningTid != INVALID_TID) ?
                      std::to_string(runningTid) : "inv";
    std::string exStr = "";
    if (exceptionReason.size()) exStr = " ex " + exceptionReason;
    os  << "["
        << this
        << " (ts=" << ts
        << " softTs=" << softTs
        << " fcn=0x" << std::hex << taskFn << std::dec
        << " state=" << state
        << " tid=" << tid
        << (isTied() ? " " : " un") << "tied"
        << (isCustomSpiller() ? " custom-spiller" : "")
        << (isCustomRequeuer() ? " custom-requeuer" : "")
        << (isOrdinarySpiller() ? " ordinary-spiller" : "")
        << (isOrdinaryRequeuer() ? " ordinary-requeuer" : "")
        << (isFrameSpiller() ? " frame-spiller" : "")
        << (isFrameRequeuer() ? " frame-requeuer" : "")
        << (nonSerialHint ? " nonSerialHint" : "")
        << (isProducer() ? " producer" : "")
        << (isIrrevocable() ? " irrevocable" : "")
        << (maySpeculate() ? " maySpec" : "")
        << (cantSpeculate() ? " cantSpec" : "")
        << (runOnAbort ? " runOnAbort" : "")
        << (noTimestamp ? " noTimestamp" : "")
        << (hasPendingAbort() ? " aborting" : "")
        << (!running() ? "" :
            !IsPriv(runningTid) ? "" :
            IsPrivDoomed(runningTid) ? " priv doomed" : " priv")
        << exStr
        << ")]";
    return os;
}

std::string Task::toString() const {
    std::stringstream ss;
    this->operator<<(ss);
    return ss.str();
}

void Task::registerObserver(std::unique_ptr<TaskObserver> to) {
    if (to == nullptr) return;
    TaskObserver* toPtr = to.get();
    bool success = false;
    std::tie(std::ignore, success) = observers.emplace(toPtr, std::move(to));
    assert(success);
}

void Task::unregisterObserver(TaskObserver* to) {
    observers.erase(to);
}


std::ostream& operator<<(std::ostream& os, Task* t) {
    t->operator<<(os);
    return os;
}

void DepthCounters::init(AggregateStat* parentStat) {
    created.init("createdDepth", "depth at which tasks were created", MAX_DEPTH + 1);
    start.init("startDepth", "depth at which tasks were when start execution", MAX_DEPTH + 1);
    finish.init("finishDepth", "depth at which tasks were when finished execution", MAX_DEPTH + 1);
    successfulCreated.init("successfulCreatedDepth", "depth at which successful tasks were created", MAX_DEPTH + 1);
    successfulStart.init("successfulStartDepth", "depth at which successful tasks started", MAX_DEPTH + 1);
    abortedCreated.init("abortedCreatedDepth", "depth at which aborted tasks were created", MAX_DEPTH + 1);
    abortedStart.init("abortedStartDepth", "depth at which aborted tasks started", MAX_DEPTH + 1);

    startQuartile.init("startQuartile", "quartile of commit queue at which tasks started", MAX_DEPTH + 1);
    abortedStartQuartile.init("abortedStartQuartile", "quartile of commit queue at which aborted tasks started", MAX_DEPTH + 1);
    successfulStartQuartile.init("successfulStartQuartile", "quartile of commit queue at which successful tasks started", MAX_DEPTH + 1);

    parentStat->append(&created);
    parentStat->append(&start);
    parentStat->append(&finish);
    parentStat->append(&successfulCreated);
    parentStat->append(&successfulStart);
    parentStat->append(&abortedCreated);
    parentStat->append(&abortedStart);

    parentStat->append(&startQuartile);
    parentStat->append(&abortedStartQuartile);
    parentStat->append(&successfulStartQuartile);
}
