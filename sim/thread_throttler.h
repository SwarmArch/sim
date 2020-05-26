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
#include <vector>
#include "sim/sim.h"
#include "sim/types.h"
#include "sim/spin_cv.h"
#include "sim/stats/stats.h"
#include "sim/str.h"
#include "sim/thread_count_controller.h"
#include "sim/rob_stats_collector.h"
#include "sim/performance_metrics.h"

namespace throttler {
enum class PerfMetric { COMMIT_INSTRS, ABORT_INSTRS, ABORT_TO_STALL };
enum class DirectionBias { UP, DOWN, RANDOM };
using PhaseEvent = RobStatsCollector::RobEventType;
using CoreEvent = RobStatsCollector::CoreEventType;

inline bool higherIsBetter(double current, double last, double guard = 0.0) {
    return current > ((1 + guard) * last);
}
inline bool lowerIsBetter(double current, double last, double guard = 0.0) {
    return current < ((1 - guard) * last);
}
}

class ThreadThrottlerUnit {
public:
    ThreadThrottlerUnit() {}
    virtual void sample(uint64_t) {}
    virtual void initStats(AggregateStat* parentStat)=0;
    virtual void addStatsCollector(RobStatsCollector* rsc)=0;
    virtual void addThreadCountController(ThreadCountController* tcc)=0;
};

template<typename T>
class ThreadThrottler : public ThreadThrottlerUnit {
public:
    ThreadThrottler()
      : ThreadThrottlerUnit(),
        perfMetric([](RobStatsCollector*) { return std::vector<T>(); }),
        better([](double, double, double) { return false; }),
        event(throttler::PhaseEvent::CYCLE),
        eventWindow(10000),
        lastEventCount(0),
        name_("tt"),
        deltaThreads(1),
        lastUpdateCycle(0),
        lastSnapshotCycle(0) {}

    ThreadThrottler(
        uint32_t _idx, throttler::PhaseEvent _event, uint64_t _eventWindow,
        uint32_t _deltaThreads,
        std::function<std::vector<T>(RobStatsCollector*)> _perfMetricFn,
        std::function<bool(double, double, double)> _betterFn)
        : ThreadThrottlerUnit(),
          perfMetric(_perfMetricFn),
          better(_betterFn),
          event(_event),
          eventWindow(_eventWindow),
          lastEventCount(0),
          name_("tt-" + Str(_idx)),
          deltaThreads(_deltaThreads),
          lastUpdateCycle(0),
          lastSnapshotCycle(0) {}

    void sample(uint64_t) override;
    virtual bool update(uint64_t) = 0;

    void addStatsCollector(RobStatsCollector* rsc) override {
        statsCollectors.push_back(rsc);
    }

    void addThreadCountController(ThreadCountController* tcc) override {
        threadCountControllers.push_back(tcc);
    }

    void initStats(AggregateStat* parentStat) override {
        AggregateStat* ttStats = new AggregateStat();
        ttStats->init(name(), "Thread throttler stats");
        reconfigIntervalCycles.init("reconfigInterval",
                                    "reconfiguration interval cycles");
        ttStats->append(&reconfigIntervalCycles);
        parentStat->append(ttStats);
    }

    double getMetric();
    void snapshotPerformance();

    const std::function<std::vector<T>(RobStatsCollector*)> perfMetric;
    const std::function<bool(double, double, double)> better;

private:
    const throttler::PhaseEvent event;
    const uint64_t eventWindow;
    uint64_t lastEventCount;

    // Stats
    RunningStat<size_t> reconfigIntervalCycles;

    double computeMetric(std::vector<T>) {    // default computeMetric()
        warn(
            "Thread throttler does not have a valid metric compute function. "
            "Check your config!");
        return 0.0;
    }

protected:
    const std::string name_;
    const uint32_t deltaThreads;

    uint64_t lastUpdateCycle;
    uint64_t lastSnapshotCycle;
    std::vector<RobStatsCollector*> statsCollectors;
    std::vector<ThreadCountController*> threadCountControllers;

    uint32_t increaseThreads(uint32_t);
    uint32_t decreaseThreads(uint32_t);
    uint32_t changeThreadCount(uint32_t, bool);
    inline const char* name() const { return name_.c_str(); }

    // State
    std::vector<T> snapshottedPerformance;
};

template <>
double ThreadThrottler<uint64_t>::computeMetric(std::vector<uint64_t>);
template <>
double ThreadThrottler<perfMetric::StallAndAborts>::computeMetric(
    std::vector<perfMetric::StallAndAborts>);

template<typename T>
class ThresholdThreadThrottler : public ThreadThrottler<T> {
    using ThreadThrottler<T>::snapshottedPerformance;
    using ThreadThrottler<T>::deltaThreads;

    using ThreadThrottler<T>::snapshotPerformance;
    using ThreadThrottler<T>::increaseThreads;
    using ThreadThrottler<T>::decreaseThreads;
    using ThreadThrottler<T>::changeThreadCount;
    using ThreadThrottler<T>::perfMetric;
    using ThreadThrottler<T>::better;
    using ThreadThrottler<T>::getMetric;

public:
    ThresholdThreadThrottler()
      : ThreadThrottler<T>(), threshold(0.80), guard(0.80) {}
    ThresholdThreadThrottler(
        uint32_t _idx, throttler::PhaseEvent _event, uint64_t _eventWindow,
        uint32_t _deltaThreads, double _threshold, double _guard,
        std::function<std::vector<T>(RobStatsCollector*)> _perfMetricFn,
        std::function<bool(double, double, double)> _betterFn)
        : ThreadThrottler<T>(_idx, _event, _eventWindow, _deltaThreads,
                             _perfMetricFn, _betterFn),
          threshold(_threshold),
          guard(_guard) {}
    bool update(uint64_t) override;

private:
    const double threshold;
    const double guard;
};

template<typename T>
class FSMThreadThrottler : public ThreadThrottler<T> {
    using ThreadThrottler<T>::snapshottedPerformance;
    using ThreadThrottler<T>::deltaThreads;

    using ThreadThrottler<T>::increaseThreads;
    using ThreadThrottler<T>::decreaseThreads;
    using ThreadThrottler<T>::perfMetric;

protected:
    enum class State { SteadyState, Move, MoveOpposite1, MoveOpposite2, Stay };
    enum class Direction { Up, Down };

public:
    FSMThreadThrottler()
      : ThreadThrottler<T>(),
        samplesToWait(5),
        guard(0),
        directionBias(throttler::DirectionBias::DOWN),
        lastMetric(0.0),
        state(State::SteadyState),
        direction(Direction::Down),
        lastDeltaThreadCount(0),
        numSamples(4) {}

    FSMThreadThrottler(
        uint32_t _idx, throttler::PhaseEvent _event, uint64_t _eventWindow,
        uint32_t _deltaThreads, uint32_t _samplesToWait, double _guard,
        throttler::DirectionBias _directionBias,
        std::function<std::vector<T>(RobStatsCollector*)> _perfMetricFn,
        std::function<bool(double, double, double)> _betterFn)
        : ThreadThrottler<T>(_idx, _event, _eventWindow, _deltaThreads,
                             _perfMetricFn, _betterFn),
          samplesToWait(_samplesToWait),
          guard(_guard),
          directionBias(_directionBias),
          lastMetric(0.0),
          state(State::SteadyState),
          direction(Direction::Down),
          lastDeltaThreadCount(0),
          numSamples(_samplesToWait - 1) {}

protected:
    // Parameters
    const uint32_t samplesToWait;
    const double guard;
    const throttler::DirectionBias directionBias;

    // State
    double lastMetric;
    State  state;
    Direction direction;
    uint32_t lastDeltaThreadCount;
    uint32_t numSamples;

    // Internal functions
    virtual void goToSteadyState();

    void assignDirection();
    void switchDirection();
    uint32_t move();
};

template<typename T>
class UpDownThreadThrottler : public FSMThreadThrottler<T> {
    using State = typename FSMThreadThrottler<T>::State;
    using Direction = typename FSMThreadThrottler<T>::Direction;

    using FSMThreadThrottler<T>::samplesToWait;
    using FSMThreadThrottler<T>::guard;
    using FSMThreadThrottler<T>::numSamples;
    using FSMThreadThrottler<T>::state;
    using FSMThreadThrottler<T>::direction;
    using FSMThreadThrottler<T>::lastMetric;
    using FSMThreadThrottler<T>::lastDeltaThreadCount;

    using FSMThreadThrottler<T>::assignDirection;
    using FSMThreadThrottler<T>::switchDirection;
    using FSMThreadThrottler<T>::move;
    using FSMThreadThrottler<T>::goToSteadyState;

public:
    UpDownThreadThrottler() : FSMThreadThrottler<T>() {}
    UpDownThreadThrottler(
        uint32_t _idx, throttler::PhaseEvent _event, uint64_t _eventWindow,
        uint32_t _deltaThreads, uint32_t _samplesToWait, double _guard,
        throttler::DirectionBias _directionBias,
        std::function<std::vector<T>(RobStatsCollector*)> _perfMetricFn,
        std::function<bool(double, double, double)> _betterFn)
        : FSMThreadThrottler<T>(_idx, _event, _eventWindow, _deltaThreads,
                                _samplesToWait, _guard, _directionBias,
                                _perfMetricFn, _betterFn) {}
    bool update(uint64_t) override;

private:
    void doSteadyStateActions();
    void doMoveStateActions();
};

// FIXME Capture previous delta so you move back correctly
// For now, deltaThreads = 1, so this shouldn't be an issue, but in
// general it could be.
template<typename T>
class GradientDescentThreadThrottler : public FSMThreadThrottler<T> {
    using State = typename FSMThreadThrottler<T>::State;
    using Direction = typename FSMThreadThrottler<T>::Direction;

    using FSMThreadThrottler<T>::samplesToWait;
    using FSMThreadThrottler<T>::guard;
    using FSMThreadThrottler<T>::numSamples;
    using FSMThreadThrottler<T>::state;
    using FSMThreadThrottler<T>::direction;
    using FSMThreadThrottler<T>::lastMetric;
    using FSMThreadThrottler<T>::lastDeltaThreadCount;

    using FSMThreadThrottler<T>::assignDirection;
    using FSMThreadThrottler<T>::switchDirection;
    using FSMThreadThrottler<T>::move;
    using FSMThreadThrottler<T>::goToSteadyState;

public:
    GradientDescentThreadThrottler() : FSMThreadThrottler<T>() {}
    GradientDescentThreadThrottler(
        uint32_t _idx, throttler::PhaseEvent _event, uint64_t _eventWindow,
        uint32_t _deltaThreads, uint32_t _samplesToWait, double _guard,
        throttler::DirectionBias _directionBias,
        std::function<std::vector<T>(RobStatsCollector*)> _perfMetricFn,
        std::function<bool(double, double, double)> _betterFn)
        : FSMThreadThrottler<T>(_idx, _event, _eventWindow, _deltaThreads,
                                _samplesToWait, _guard, _directionBias,
                                _perfMetricFn, _betterFn) {}
    bool update(uint64_t) override;

private:
    void doSteadyStateActions();
    void doMoveStateActions();
    void doMoveOpposite1StateActions();
    void doMoveOpposite2StateActions();
    void doStayStateActions();
};
