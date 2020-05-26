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

#include "sim/collections/prio_queue.h"
#include "sim/sim.h"

/* Base event, exists to put special "thread events" managed by the event queue
 * code under the same class hierarchy as globally-used Events.
 */
class BaseEvent : public PrioQueueElem<BaseEvent> {
  private:
    // [victory] Event and ThreadState share this member cycle_ in this base
    // class so it can be read (via the cycle() and key() methods) efficiently,
    // without conditional branches, indirections, or virtual function calls.
    // cycle_ is private with a friend declarations to allow ThreadState to
    // access and update it, but prevent Event subclasses from doing so.
    uint64_t cycle_;
    friend class ThreadState;

  protected:
    BaseEvent(uint64_t cycle) : cycle_(cycle) {}

  public:
    inline uint64_t cycle() const { return cycle_; };
    inline uint64_t key() const { return cycle(); };
};

/* All (external) users of the event queue must inherit from Event. Event is
 * single-use, fixed-cycle (can't reschedule or delay), and owned by the event
 * queue code.
 */
class Event : public BaseEvent {
  public:
    Event(uint64_t cycle) : BaseEvent(cycle) {}
    virtual ~Event() {}

    virtual void process() = 0;
};

/* Lambda events */

template <typename F> class LambdaEvent : public Event {
  private:
    const F f_;

  public:
    LambdaEvent(uint64_t cycle, F f) : Event(cycle), f_(f) {}
    void process() override { f_(cycle()); }
};

template <typename F> class PeriodicLambdaEvent : public Event {
  private:
    const uint64_t period_;
    F f_;

  public:
    PeriodicLambdaEvent(uint64_t cycle, uint64_t period, F f)
        : Event(cycle), period_(period), f_(f) {}

    void process() override {
        f_(cycle());
        pushEvent(new PeriodicLambdaEvent<F>(cycle() + period_, period_, f_));
    }
};

// Like a periodic event, but lambda returns the interval till next firing
// (0 stops firing)
template <typename F> class RecurringLambdaEvent : public Event {
  private:
    F f_;

  public:
    RecurringLambdaEvent(uint64_t cycle, F f)
        : Event(cycle), f_(f) {}

    void process() override {
        uint64_t nextInterval = f_(cycle());
        if (nextInterval)
            pushEvent(new RecurringLambdaEvent<F>(cycle() + nextInterval, f_));
    }
};

// Convenience creation functions
template <typename F> LambdaEvent<F>* schedEvent(uint64_t cycle, F f) {
    LambdaEvent<F>* event = new LambdaEvent<F>(cycle, f);
    pushEvent(event);
    return event;
}

template <typename F>
PeriodicLambdaEvent<F>* schedPeriodicEvent(uint64_t cycle, uint64_t period,
                                           F f) {
    assert(period);
    auto event = new PeriodicLambdaEvent<F>(cycle, period, f);
    pushEvent(event);
    return event;
}

template <typename F>
RecurringLambdaEvent<F>* schedRecurringEvent(uint64_t cycle, F f) {
    auto event = new RecurringLambdaEvent<F>(cycle, f);
    pushEvent(event);
    return event;
}

