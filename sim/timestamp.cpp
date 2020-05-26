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

#include "sim/timestamp.h"

#include <functional>
#include <stack>

// The timestamp constants below cannot be constexpr because the TimeDomainPtr
// (std::shared_ptr) member is non-literal (it has a non-trivial destructor).
// Nonetheless, note that the TimeStamp constructor is constexpr,
// and these non-constexpr variables should still be constant-initialized.
// So, it is safe to use these variables to initialize other static global
// variables.  See:
//     https://stackoverflow.com/a/24493590/12178985
//     https://en.cppreference.com/w/cpp/language/constant_initialization
// In C++20, these constants should be constinit. See:
//     https://en.cppreference.com/w/cpp/language/constinit
//     https://wg21.link/p1143r2

// For canaries, ZERO_TS represents "committed" data, but writes by an
// irrevocable are not *yet* committed.
const TimeStamp ZERO_TS(0,0);
const TimeStamp IRREVOCABLE_TS(0,1);

// N.B. Infinity's tiebreaker can't be assigned __MAX_TS as that value is
// reserved for unset tie-breakers
const TimeStamp INFINITY_TS(__MAX_TS, __MAX_TS - 1);

// For NOTIMESTAMP tasks to hold the GVT from reaching infinity,
// without holding the GVT from passing over any timestamped tasks.
const TimeStamp NO_TS(__MAX_TS, __MAX_TS - 2);

static constexpr uint64_t MAX_ASSIGNED_TIEBREAKER = __MAX_TS - 4;

class TimeDomain {
  public:
    const TimeStamp parent;
    const uint64_t depth;
    const uint64_t maxTS;
    const uint64_t id;

    TimeDomain(const TimeStamp& _parent, uint64_t _depth, uint64_t _maxTS)
        : parent(_parent), depth(_depth), maxTS(_maxTS), id(numDomains++) {}

  private:
    static uint64_t numDomains;
};

uint64_t TimeDomain::numDomains = 1; // root always exists

std::ostream& TimeStamp::operator<<(std::ostream& os) const {
    if (operator==(ZERO_TS)) {
        os << "zero";
    } else if (operator==(INFINITY_TS)) {
        os << "infinity";
    } else if (operator==(IRREVOCABLE_TS)) {
        os << "zero_irrevocable";
    } else if (operator==(NO_TS)) {
        os << "inf_no_ts";
    } else {
        if (domain_) {
            if (domain_->depth < 4) {
                os << domain_->parent << ".";
            } else if (domain_->depth < 32) {
                //victory: it's useful to see many application-level timestamps,
                // as it can help identify where an application is stuck.
                std::function<void (const TimeStamp*)> print_app_ts =
                        [&print_app_ts, &os] (const TimeStamp* ts) {
                    if (ts->domain_) {
                        print_app_ts(&ts->domain_->parent);
                        os << ".";
                    }
                    os << ts->app() << ":_";
                };
                print_app_ts(&domain_->parent);
                os << ".";
            } else {
                // Don't print long timestamp chains (and avoid stack overflows
                // due to too much recursion)
                const TimeStamp* root = this;
                while (root->domain_) root = &root->domain_->parent;
                os << *root << "...[" << domain_->depth - 1 << "]...";
            }
        }
        os << app() << ":";
        if (tiebreaker() == __MAX_TS)
          os << "UNSET";
        else
          os << tiebreaker();
    }
    return os;
}

void TimeStamp::printLevels(std::ostream& os, uint32_t levels) const {
    if (!domain_) {
        operator<<(os);
    } else {
        if (levels < domain_->depth) {
            const TimeStamp* root = this;
            while (root->domain_) root = &root->domain_->parent;
            os << *root << " ...[" << domain_->depth - levels << "]...";
            levels--;
        }
        const TimeStamp* leaf = this;
        std::stack<const TimeStamp*> tsStack;
        for (uint32_t i = 0; i < levels; i++) {
            tsStack.push(leaf);
            if (!leaf->domain_) break;
            leaf = &leaf->domain_->parent;
        }
        while (true) {
            const TimeStamp* top = tsStack.top();
            os << top->app() << ":" << top->tiebreaker();
            tsStack.pop();
            if (tsStack.empty()) break;
            else os << ".";
        }
    }
}

std::ostream& operator<<(std::ostream& os, const TimeStamp & ts) {
    (ts).operator<<(os);
    return os;
}

std::tuple<const TimeStamp*, const TimeStamp*, int64_t>
TimeStamp::sameDomainAncestors(const TimeStamp* a, const TimeStamp* b) {
    if (a == b) return std::make_tuple(a, a, 0);


    // 2. Equalize depths
    uint64_t aDepth = a->domainDepth();
    uint64_t bDepth = b->domainDepth();
    int64_t depthDelta = bDepth - aDepth;
    while (aDepth > bDepth) {
        a = &a->domain_->parent;
        aDepth--;
    }
    while (bDepth > aDepth) {
        b = &b->domain_->parent;
        bDepth--;
    }

    // 3. Find first ancestors with the same domain
    while (a->domain_ && a->domain_->parent != b->domain_->parent) {
        a = &a->domain_->parent;
        b = &b->domain_->parent;
    }

    return std::make_tuple(a, b, depthDelta);
}

/* victory: Fractal time is lexicographically ordered:
 * 1:100.0:150 > 1:100
 * 1:100.1:250 < 1:101
 * 1:100.0:0 > 1:100
 */
bool TimeStamp::operator<(const TimeStamp& that) const {
    if (domain_ == that.domain_) {
        // Fastpath
        return dvt() < that.dvt();
    } else {
        const TimeStamp* a;
        const TimeStamp* b;
        int64_t depthDelta;
        std::tie(a, b, depthDelta) = sameDomainAncestors(this, &that);

        // Compare first common ancestor, break ties with depth
        return a->dvt() < b->dvt() ||
               (a->dvt() == b->dvt() && depthDelta > 0);
    }
}

bool TimeStamp::operator==(const TimeStamp& that) const {
    if (domain_ == that.domain_) {
        return dvt() == that.dvt();
    } else {
        const TimeStamp* a = this;
        const TimeStamp* b = &that;

        if (a->domainDepth() != b->domainDepth()) return false;

        while (true) {
            if (a == b) return true;  // same object
            if (a->dvt() != b->dvt()) return false;
            a = &a->domain_->parent;
            b = &b->domain_->parent;
        }
    }
}

bool TimeStamp::hasTieBreaker() const {
    return tiebreaker() != __MAX_TS;
}

bool TimeStamp::hasAssignedTieBreaker() const {
    assert(tiebreaker() != 0);
    return tiebreaker() <= MAX_ASSIGNED_TIEBREAKER;
}

void TimeStamp::assignTieBreaker(uint64_t tiebreaker) {
    assert(tiebreaker >= MIN_TIEBREAKER);
    assert(tiebreaker <= MAX_ASSIGNED_TIEBREAKER);
    // Try to assign the tie-breaker component
    if (!hasTieBreaker()) {
        tiebreaker_ = tiebreaker;
    }
}

void TimeStamp::clearTieBreaker() {
    assert(*this != ZERO_TS);
    assert(*this != NO_TS);
    assert(*this != INFINITY_TS);
    assert(hasAssignedTieBreaker());
    tiebreaker_ = __MAX_TS;
}

void TimeStamp::deepen(uint64_t maxTS) {
    assert(maxTS >= (app_-1) || ((app_ == 0) && maxTS > app_));
    assert_msg(!subdomain_, "deepen() called twice!");
    // Deepening depends on tiebreakers.  Consider task A with unset
    // tiebreaker.  Suppose it depeens and enqueues subdomain child B.
    //     A: (2,MAX)
    //        |
    //    ---------
    //   |  B: 42  |
    // B then enqueues C (2,MAX) to superdomain.  For domain atomicity,
    // B'ts  := (2,MAX) . (42,xxx) has to be less than
    // C'ts  := (2,MAX)
    // But this is violated since A's tb is not set.
    // We prevent this by requiring that A's tb is set when it deepens.
    // This is true regardless of whether tasks are speculative.
    assert_msg(hasTieBreaker(), "unique tiebreakers needed for domain atomicity");
    subdomain_ = std::make_shared<TimeDomain>(*this, domainDepth() + 1, maxTS);
}

void TimeStamp::undeepen() {
    subdomain_ = nullptr;
}

TimeStamp TimeStamp::childTS(uint64_t app, bool useParentDomain) const {
    assert(app < __MAX_TS);

    TimeDomainPtr domain = subdomain_? subdomain_ : domain_;

    if (useParentDomain) {
        assert(domain);
        domain = domain->parent.domain_;
    }

    while (domain && domain->maxTS < app) domain = domain->parent.domain_;
    return TimeStamp(domain, app, __MAX_TS);
}

uint64_t TimeStamp::domainId() const {
    return domain_? domain_->id : 0;
}

uint64_t TimeStamp::domainDepth() const {
    return domain_? domain_->depth : 0;
}

TimeStamp TimeStamp::domainMin() const {
    // The below would be equivalent in ordering to domain_->parent if you just
    // did a comparison of FVT bit strings implemented simply by concatenating LVTs.
    // However, it is one domain level deeper, so it is ordered immediately
    // after domain_->parent, but before any real TimeStamp in domain_.
    // N.B. if domain_ is null, this is just ZERO_TS.
    return TimeStamp(domain_, 0, 0);
}

TimeStamp TimeStamp::domainMax() const {
    // Since no ordinary task whose TimeStamp was created via childTS() may
    // have app component __MAX_TS, this is guaranteed to be ordered after
    // all ordinary tasks in the same domain.
    // The tiebreaker is set to ensure it is ordered before NO_TS and INFINITY_TS.
    // It is safe to use this for frame spillers & frame requeuers since
    // they will not need their tiebreaker to be dynamically adjusted later
    // (See ROB::notifyDependence().)
    return TimeStamp(domain_, __MAX_TS, __MAX_TS - 3);
}

uint64_t TimeStamp::reportToApp() const {
    return subdomain_? 0 : app();
}

uint64_t TimeStamp::reportToAppSuper() const {
    // See https://github.mit.edu/swarm/sim/issues/2
    // When a task deepens, its frame of reference shifts down a level.
    const uint64_t appDepth = domainDepth() + !!subdomain_;
    if (appDepth > 0)
        return getTimeStampAtLevel(appDepth - 1).app();
    else
        // This is a way of telling the caller it is in the root domain.
        return __MAX_TS;
}

// Should this be absorbed into the childTS() function instead?
TimeStamp TimeStamp::getSameDomain(uint64_t app, uint64_t tb) const {
    return TimeStamp(domain_, app, tb);
}

TimeStamp TimeStamp::getTimeStampAtLevel(const uint64_t domainDepth) const {
    uint64_t thisDepth = domain_ ? domain_->depth : 0;
    assert((thisDepth >= domainDepth) ||
           ((thisDepth == domainDepth - 1) && subdomain_));
    // Can request a timestamp in a subdomain, hence this check.
    if (thisDepth == domainDepth - 1) {
        // Recall that a noTimestamp task should really be implemented as a
        // timestamp interval with upper bound infinity. We would treat this
        // large interval like we do lazy tiebreakers: upon a conflict, child
        // creation, or domain creation, the interval must be collapsed into a
        // single virtual time. Since we aren't yet implementing non-timestamped
        // tasks like that, forbid them from playing with Fractal.
        assert(*this != NO_TS);
        // Based on the use case, it seems like assigning 0
        // as the app-level timestamp in the subdomain is
        // a reasonable thing to do. This is used for assigning
        // a timestamp for a spiller in the subdomain.
        return TimeStamp(subdomain_, 0, __MAX_TS);
    } else if (*this == NO_TS) {
        assert(thisDepth == 0 && domainDepth == 0);
        return NO_TS;
    } else {
        int64_t goUp = (thisDepth - domainDepth);
        auto domain = domain_;
        uint64_t levelApp = app();
        while (goUp > 0) {
            levelApp = domain->parent.app();
            domain = domain->parent.domain_;
            goUp--;
        }
        return TimeStamp(domain, levelApp, __MAX_TS);
    }
}
