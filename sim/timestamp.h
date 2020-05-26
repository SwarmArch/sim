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

#include <cstdint>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>
#include "sim/assert.h"
#include "sim/types.h"

// Forward declaration of types
class TimeStamp;
class TimeDomain;
typedef std::shared_ptr<TimeDomain> TimeDomainPtr;

// Constant timestamps (defined in cpp file)
// Defined as refs to avoid constantly creating ephemeral timestamp objects for
// comparisons
extern const TimeStamp ZERO_TS;
extern const TimeStamp IRREVOCABLE_TS;
extern const TimeStamp INFINITY_TS;
extern const TimeStamp NO_TS;

// Use this class as the ROB-internal TS to attempt to break ties on application
// TS's with partial order. It will force some unnecessary aborts due to false
// timing dependencies, but this is the simplest way to get our HW to correctly
// execute applications with partial order.
//
// In some execution configurations, the tie-breaking component can't be set at
// TimeStamp construction. Specifically, ties need not be broken until conflict
// resolution. Thus the TimeStamp permits operators that tolerate the
// tie-breaker being unset.
//
// There is a mini state machine here regarding whether the tie-breaker is set.
//  states: {(unset, n/a), (set, tb)}
//  permissible transitions:
//     (unset, n/a) -> (set, tb)
//     (set, tb)    -> (set, tb)  (i.e. no changing the tie-breaker once set)
class TimeStamp {
    /* dsm: Under fractal time, each timestamp may have an associated domain,
     * and tasks can start new domains to gain additional resolution and
     * improve composability.  See interface below.  Implementation is deferred
     * to cpp file to avoid cyclic dependences between TimeDomain and
     * TimeStamp, and to hide implementation details from the rest of the code
     * as much as possible.
     */
    TimeDomainPtr domain_;
    TimeDomainPtr subdomain_;

    uint64_t app_;
    uint64_t tiebreaker_;
public:

    // To prevent task timestamps colliding with special values like ZERO_TS
    static const constexpr uint64_t MIN_TIEBREAKER = 2;

    // Initialize the tie-breaker component to the maximum integer. This is
    // the "unset" value. Since tiebreakers globally montonically increase,
    // then an unset tiebreaker should dominate a set tiebreaker, since when it
    // gets set, it will eventually be larger.
    // FIXME it is hacky that the TimeStamp class must make that assumption.
    // Really we should only be assuming that tiebreakers are unique.
    constexpr TimeStamp(uint64_t app)
        : app_(app), tiebreaker_(__MAX_TS) { }

    // TODO(victory): Make this private, users only need single-arg constructor
    constexpr TimeStamp(uint64_t app, uint64_t tiebreaker)
        : app_(app), tiebreaker_(tiebreaker) { }

    TimeStamp(const TimeStamp& ts) = default;
    TimeStamp(TimeStamp&& ts) = default;

    TimeStamp& operator=(const TimeStamp &) = default;

    bool hasTieBreaker() const ;
    bool hasAssignedTieBreaker() const ;
    void assignTieBreaker(uint64_t tiebreaker) ;

    void clearTieBreaker() ;

    // Weak ordering
    bool operator<(const TimeStamp& that) const;
    inline bool operator>(const TimeStamp& that) const {
        return that.operator<(*this);
    }
    inline bool operator>=(const TimeStamp& that) const {
        return !operator<(that);
    }
    inline bool operator<=(const TimeStamp& that) const {
        return !operator>(that);
    }

    bool operator==(const TimeStamp& that) const;
    inline bool operator!=(const TimeStamp& that) const {
        return !operator==(that);
    }

    uint64_t app() const { return app_; }
    uint64_t tiebreaker() const { return tiebreaker_; }

    uint64_t reportToApp() const;
    uint64_t reportToAppSuper() const;

    std::ostream& operator<<(std::ostream& os) const;

    std::string toString() const {
        std::ostringstream buffer;
        this->operator<<(buffer);
        return buffer.str();
    }

    // Specify number of DVTs to print out within FVT. Useful for debugging.
    std::string toString(const uint32_t levels) const {
        std::ostringstream buffer;
        printLevels(buffer, levels);
        return buffer.str();
    }

    /* Fractal time interface */

    // To start a new level, a task simply calls deepen() on its timestamp.
    // Timestamp ordering is unaffected, so there's no need to update queues.
    // It is an error to call deepen() twice without calling undeepen() in between.
    // All enqueues from that point on happen on the deeper level.
    void deepen(uint64_t maxTS = __MAX_TS);

    // Called when task aborts to discard possible deepenings
    void undeepen();

    // Children task timestamps must be generated with this method, which sets
    // the right timestamp domain automatically
    TimeStamp childTS(uint64_t app, bool useParentDomain = false) const;

    bool sameDomain(const TimeStamp& ts) const { return domain_ == ts.domain_; }

    // Used to profile distribution of domains
    uint64_t domainId() const;
    uint64_t domainDepth() const;
    TimeStamp getSameDomain(uint64_t app, uint64_t tb=__MAX_TS) const;
    TimeStamp domainMin() const;
    TimeStamp domainMax() const;
    TimeStamp getTimeStampAtLevel(uint64_t domainDepth) const;
    TimeStamp getUpLevels(uint64_t levelsUp) const {
        assert(levelsUp <= domainDepth());
        return getTimeStampAtLevel(domainDepth() - levelsUp);
    }

private:
    // Private b/c we don't want to expose TimeDomain externally...
    TimeStamp(TimeDomainPtr domain, uint64_t app, uint64_t tiebreaker) :
        domain_(domain), app_(app) , tiebreaker_(tiebreaker) {}

    // Domain virtual time
    std::tuple<uint64_t, uint64_t> dvt() const {
        return std::make_tuple(app_, tiebreaker_);
    }

    static inline std::tuple<const TimeStamp*, const TimeStamp*, int64_t>
    sameDomainAncestors(const TimeStamp* a, const TimeStamp* b);
    void printLevels(std::ostream& os, uint32_t levels) const;
};

std::ostream& operator<<(std::ostream& os, const TimeStamp & ts);
