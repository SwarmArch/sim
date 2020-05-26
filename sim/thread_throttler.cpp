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

#include "sim/sim.h"
#include "sim/driver.h"
#include "sim/thread_throttler.h"
#include "sim/log.h"
#include "sim/event.h"
#include "sim/types.h"
#include "sim/match.h"

#undef DEBUG
#define DEBUG(args...) //info(args)

/*
    Generic Thread Throttler functions
*/
template<typename T>
void ThreadThrottler<T>::sample(uint64_t cycle) {
    DEBUG("[%lu] [%s] Sampling...", cycle, this->name());
    uint64_t currentEventCount = 0;
    for (auto stat : statsCollectors) {
        currentEventCount += stat->getRobEventCount(event);
        if (event == throttler::PhaseEvent::CYCLE) break;
    }
    assert(currentEventCount >= lastEventCount);
    if ((currentEventCount - lastEventCount) >= eventWindow) {
        DEBUG("[%s] Going to update... (cur: %lu, last: %lu)", this->name(),
              currentEventCount, lastEventCount);
        bool success = update(cycle); // [ssub] Have to call update() before updating
                                      // the values below. Gradient descent uses the
                                      // lastUpdateCycle to set the measuring period.
        if (success) {
            uint64_t interval = cycle - lastUpdateCycle;
            reconfigIntervalCycles.push(interval);
            lastUpdateCycle = cycle;
            lastEventCount = currentEventCount;
        }
    }
}

template<typename T>
uint32_t ThreadThrottler<T>::changeThreadCount(uint32_t deltaThreads, bool increase) {
    uint32_t count = MAX32_T;
    for (auto tcc : threadCountControllers) {
        uint32_t c;
        c = increase ? tcc->increaseThreads(deltaThreads)
                     : tcc->decreaseThreads(deltaThreads);
        if (count == MAX32_T) count = c;
        else assert(c == count);
    }
    assert(count != MAX32_T);
    return count;
}

template<typename T>
uint32_t ThreadThrottler<T>::increaseThreads(uint32_t deltaThreads) {
    return changeThreadCount(deltaThreads, true/*increase*/);
}

template<typename T>
uint32_t ThreadThrottler<T>::decreaseThreads(uint32_t deltaThreads) {
    return changeThreadCount(deltaThreads, false/*increase*/);
}

template<typename T>
void ThreadThrottler<T>::snapshotPerformance() {
    snapshottedPerformance.clear();
    for (auto leaf : statsCollectors) {
        auto v = perfMetric(leaf);
        snapshottedPerformance.insert(std::end(snapshottedPerformance),
                                      std::begin(v), std::end(v));
    }
    auto curCycle = getCurCycle();
    assert(curCycle >= lastSnapshotCycle);
    lastSnapshotCycle = curCycle;
}

template <>
double ThreadThrottler<uint64_t>::computeMetric(std::vector<uint64_t> count) {
    uint64_t total = std::accumulate(count.begin(), count.end(), 0);
    auto curCycle = getCurCycle();
    assert(curCycle >= lastSnapshotCycle);
    DEBUG("total: %lu, cycles in last phase: %lu", total,
          curCycle - lastSnapshotCycle);
    return static_cast<double>(total) /
           static_cast<double>(curCycle - lastSnapshotCycle);
}

template <>
double ThreadThrottler<perfMetric::StallAndAborts>::computeMetric(
    std::vector<perfMetric::StallAndAborts> count) {
    uint64_t totalAbortedInstrs = 0;
    uint64_t totalStalledCycles = 0;
    for (auto &x : count) {
        totalAbortedInstrs += x.abort;
        totalStalledCycles += x.stall;
    }
    DEBUG("totalStalledCycles: %lu, totalAbortedInstrs: %lu",
          totalStalledCycles, totalAbortedInstrs);
    return (totalStalledCycles == 0)
               ? 0.0
               : static_cast<double>(totalAbortedInstrs) /
                     static_cast<double>(totalStalledCycles);
}

template<typename T>
double ThreadThrottler<T>::getMetric() {
    std::vector<T> currentPerformance;
    for (auto leaf : statsCollectors) {
        auto v = perfMetric(leaf);
        currentPerformance.insert(std::end(currentPerformance),
                                      std::begin(v), std::end(v));
    }
    if (unlikely(snapshottedPerformance.size() == 0))
        snapshottedPerformance.resize(currentPerformance.size());
    assert(currentPerformance.size() == snapshottedPerformance.size());
    double metric =
        computeMetric(metricDiff(currentPerformance, snapshottedPerformance));
    return metric;
}

/*
    Threshold Thread Throttler functions
*/
template<typename T>
bool ThresholdThreadThrottler<T>::update(uint64_t) {
    DEBUG("-------------------------------------------")
    DEBUG("[%s] Going to update at curCycle: %lu", this->name(), getCurCycle());

    if (unlikely(snapshottedPerformance.size() == 0)) { // this should happen only the first time
        uint64_t totalSize = 0;
        for (auto leaf : this->statsCollectors)
            totalSize += perfMetric(leaf).size();
        snapshottedPerformance.resize(totalSize);
    }

    // Get current phase performance
    double currentMetric = getMetric();

    // Compare against threshold, and move accordingly
    uint32_t count = 0; (void) count;
    if (better(currentMetric, threshold, guard))
        count = increaseThreads(deltaThreads/*num*/);
    else
        count = decreaseThreads(deltaThreads/*num*/);

    DEBUG(
        "[%s] Current metric: %2.6f, threshold: %2.6f, %s thread "
        "count by %u",
        this->name(), currentMetric, threshold,
        better(currentMetric, threshold, guard) ? "Increase" : "Decrease", count);

    // Save current performance
    snapshotPerformance();

    return true;
}

/*
    FSM Thread Throttler functions
*/
template<typename T>
void FSMThreadThrottler<T>::goToSteadyState() {
    DEBUG("[%s] transition to STEADY_STATE", this->name());
    state = State::SteadyState;
    numSamples = 0;
    lastDeltaThreadCount = 0;
}

template<typename T>
void FSMThreadThrottler<T>::switchDirection() {
    direction = static_cast<Direction>(static_cast<int>(Direction::Down) -
                                       static_cast<int>(direction));
}

template<typename T>
uint32_t FSMThreadThrottler<T>::move() {
    uint32_t count =
        this->changeThreadCount(deltaThreads, (direction == Direction::Up));
    lastDeltaThreadCount = count;
    return count;
}

template<typename T>
void FSMThreadThrottler<T>::assignDirection() {
    if (directionBias == throttler::DirectionBias::DOWN) {
        direction = Direction::Down;
        return;
    }
    if (directionBias == throttler::DirectionBias::UP) {
        direction = Direction::Up;
        return;
    }
    if (directionBias == throttler::DirectionBias::RANDOM) {
        direction = static_cast<Direction>(std::rand() % 2);
        return;
    }
    panic("Invalid direction bias, or direction bias unset in throttler.");
}


/*
    UpDown FSM Thread Throttler functions
*/
template<typename T>
bool UpDownThreadThrottler<T>::update(uint64_t cycle) {
    DEBUG("[%s] Going to update curCycle: %lu", this->name(), getCurCycle());

    // If we have not passed enough samples in steady state, just wait
    if (++numSamples < samplesToWait) {
        this->snapshotPerformance();
        return true;
    }

    // Take state appropriate action
    switch (state) {
        case State::SteadyState:
            doSteadyStateActions();
            break;
        case State::Move:
            doMoveStateActions();
            break;
        default:
            panic("Invalid state");
            break;
    }

    this->snapshotPerformance();
    return true;
}

template<typename T>
void UpDownThreadThrottler<T>::doSteadyStateActions() {
    assert(state == State::SteadyState);
    // We have enough steady state samples after this sample
    // Get the metric of performance and move UP or DOWN.
    assignDirection();
    DEBUG("[%s] In STEADY_STATE, trying to go %s", this->name(),
          (direction == Direction::Up) ? "UP" : "DOWN");
    uint32_t count = move();
    if (!count) {
        switchDirection();
        DEBUG("[%s] Switching direction to %s", this->name(),
              (direction == Direction::Up) ? "UP" : "DOWN");
        count = move();
    }

    if (count) {    // When numThreads = 1, count can be 0. So just stay in steady state.
        assert(lastDeltaThreadCount == 0);
        DEBUG("[%s] transition to MOVE", this->name());
        state = State::Move;
        lastMetric = this->getMetric();
    }
}

template<typename T>
void UpDownThreadThrottler<T>::doMoveStateActions() {
    assert(state == State::Move);
    uint32_t count;
    double curMetric = this->getMetric();
    DEBUG("[%s] In MOVE, curMetric: %2.6f, lastMetric: %2.6f, better: %s",
          this->name(), curMetric, lastMetric,
          this->better(curMetric, lastMetric, guard) ? "yes" : "no");
    if (this->better(curMetric, lastMetric, guard)) {
        goToSteadyState();
    } else {
        switchDirection();
        DEBUG("[%s] Switching direction to %s", this->name(),
              (this->direction == Direction::Up) ? "UP" : "DOWN");
        assert(lastDeltaThreadCount > 0);
        count = this->changeThreadCount(
            lastDeltaThreadCount,
            (this->direction == Direction::Up));  // Back to SteadyState thread count
        assert(count > 0);
        count = move();    // Move in opposite direction
        if (count) {
            DEBUG("[%s] transition to MOVE", this->name());
        } else goToSteadyState(); // Can't move any further in that direction
    }
}

/*
    UpDown FSM Thread Throttler functions
*/
template<typename T>
bool GradientDescentThreadThrottler<T>::update(uint64_t cycle) {
    DEBUG("[%s] Going to update curCycle: %lu", this->name(), getCurCycle());

    // If we have not passed enough samples in steady state, just wait
    if (++numSamples < samplesToWait) {
        this->snapshotPerformance();
        return true;
    }

    // Take state appropriate action
    switch (state) {
        case State::SteadyState:
            doSteadyStateActions();
            assert(matchAny(state, State::SteadyState, State::Move));
            break;
        case State::Move:
            doMoveStateActions();
            assert(matchAny(state, State::Move, State::Stay, State::MoveOpposite2));
            break;
        case State::MoveOpposite1:
            doMoveOpposite1StateActions();
            assert(matchAny(state, State::SteadyState, State::MoveOpposite2));
            break;
        case State::MoveOpposite2:
            doMoveOpposite2StateActions();
            assert(matchAny(state, State::SteadyState, State::Move, State::Stay));
            break;
        case State::Stay:
            doStayStateActions();
            assert(matchAny(state, State::SteadyState, State::MoveOpposite1));
            break;
        default:
            panic("Invalid state");
            break;
    }

    this->snapshotPerformance();
    return true;
}

template<typename T>
void GradientDescentThreadThrottler<T>::doSteadyStateActions() {
    // We have enough steady state samples after this sample
    // Get the metric of performance and move UP or DOWN.
    assignDirection();
    DEBUG("[%s] In STEADY_STATE, trying to go %s", this->name(),
          (this->direction == Direction::Up) ? "UP" : "DOWN");
    uint32_t count = move();
    if (!count) {
        switchDirection();
        DEBUG("[%s] Switching direction to %s", this->name(),
              (this->direction == Direction::Up) ? "UP" : "DOWN");
        count = move();
    }

    if (count) {    // When numThreads = 1, count can be 0. So just stay in steady state.
        DEBUG("[%s] transition to MOVE", this->name());
        state = State::Move;
        lastMetric = this->getMetric();
    }
}

template<typename T>
void GradientDescentThreadThrottler<T>::doMoveStateActions() {
    assert(state == State::Move);
    uint32_t count;
    double curMetric = this->getMetric();
    DEBUG("[%s] In MOVE, curMetric: %2.6f, lastMetric: %2.6f, better: %s",
          this->name(), curMetric, lastMetric,
          this->better(curMetric, lastMetric, guard) ? "yes" : "no");
    if (this->better(curMetric, lastMetric, guard)) {
        // Try to move further in the same direction
        count = move();

        // If not possible, go back to the previous thread
        // count, re-compare and get back to this thread
        // count if better. That is achieved by the
        // MoveOpposite2 state.
        if (!count) {
            switchDirection();
            DEBUG("[%s] Switching direction to %s, and transition to MOVE_OPP2", this->name(),
                  (this->direction == Direction::Up) ? "UP" : "DOWN");
            count = move();
            assert(count);
            state = State::MoveOpposite2;
        }

        // If successful in moving further in the same direction
        // just continue in the move state. In either case, poll
        // the current performance for comparison purposes.
        lastMetric = curMetric;
    } else {
        // Wait for one more phase before you make a decision
        // You want to compare against the previous performance,
        // so don't update the metric
        DEBUG("[%s] transition to STAY", this->name());
        state = State::Stay;
    }
}

template<typename T>
void GradientDescentThreadThrottler<T>::doStayStateActions() {
    uint32_t count;
    double curMetric = this->getMetric();
    DEBUG("[%s] In STAY, curMetric: %2.6f, lastMetric: %2.6f, better: %s",
          this->name(), curMetric, lastMetric,
          this->better(curMetric, lastMetric, guard) ? "yes" : "no");
    if (this->better(curMetric, lastMetric, guard)) {
        // TODO Should I continue moving in the same direction instead?
        goToSteadyState();
    } else {
        state = State::MoveOpposite1;
        switchDirection();
        DEBUG("[%s] Switching directions to %s, transition to MOVE_OPP1",
              this->name(), (this->direction == Direction::Up) ? "UP" : "DOWN");
        count = move();
        assert(count != 0);
        // TODO Save current performance metric for comparison?
        // I think NO.
    }
}

template<typename T>
void GradientDescentThreadThrottler<T>::doMoveOpposite1StateActions() {
    uint32_t count;
    double curMetric = this->getMetric();
    count = move();
    DEBUG(
        "[%s] In MOVE_OPP1, curMetric: %2.6f, lastMetric: %2.6f, better: %s "
        "(not relevant though)",
        this->name(), curMetric, lastMetric,
        this->better(curMetric, lastMetric, guard) ? "yes" : "no");
    if (count) {
        DEBUG("[%s] transition to MOVE_OPP2", this->name());
        state = State::MoveOpposite2;
        lastMetric = curMetric;
    } else {
        goToSteadyState();
    }
}

template<typename T>
void GradientDescentThreadThrottler<T>::doMoveOpposite2StateActions() {
    uint32_t count;
    double curMetric = this->getMetric();
    DEBUG("[%s] In MOVE_OPP2, curMetric: %2.6f, lastMetric: %2.6f, better: %s",
          this->name(), curMetric, lastMetric,
          this->better(curMetric, lastMetric, guard) ? "yes" : "no");
    if (this->better(curMetric, lastMetric, guard)) {
        count = move();
        if (count) {
            DEBUG("[%s] transition to MOVE", this->name());
            state = State::Move;
            lastMetric = curMetric;
        } else {
            DEBUG("[%s] transition to STAY", this->name());
            state = State::Stay;
            // Don't save curMetric yet.
        }
    } else {
        // Go back to the previous thread count
        switchDirection();
        DEBUG("[%s] Switching direction to %s", this->name(),
                  (this->direction == Direction::Up) ? "UP" : "DOWN");
        uint32_t count = move();
        assert(count);
        goToSteadyState();
    }
}

template class ThresholdThreadThrottler<uint64_t>;
template class ThresholdThreadThrottler<perfMetric::StallAndAborts>;
template class UpDownThreadThrottler<uint64_t>;
template class UpDownThreadThrottler<perfMetric::StallAndAborts>;
template class GradientDescentThreadThrottler<uint64_t>;
template class GradientDescentThreadThrottler<perfMetric::StallAndAborts>;
