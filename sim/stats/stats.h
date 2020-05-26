/** $lic$
 * Copyright (C) 2012-2021 by Massachusetts Institute of Technology
 * Copyright (C) 2010-2012 by The Board of Trustees of Stanford University
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

/* This file was adapted from zsim. */

/* Statistics facilities
 * Author: Daniel Sanchez
 * Date: Aug 2010
 *
 * There are four basic types of stats:
 * - Counter: A plain single counter.
 * - VectorCounter: A fixed-size vector of logically related counters. Each
 *   vector element may be unnamed or named (useful when enum-indexed vectors).
 * - Histogram: A GEMS-style histogram, intended to profile a distribution.
 *   It has a fixed amount of buckets, and buckets are resized as samples
 *   are added, making profiling increasingly coarser but keeping storage
 *   space constant. Unlike GEMS-style stats, though, at some configurable
 *   point part of the array starts growing logarithmically, to capture
 *   outliers without hurting accuracy of most samples.
 * - Lambda/VectorLambda stats allow for arbitrary functions.
 *
 * Groups of stats are contained in aggregates (AggregateStat), representing
 * a collection of stats. At initialization time, all stats are registered
 * with an aggregate, forming a tree of stats. After all stats are
 * initialized, the tree of stats is made immutable; no new stats can be
 * created and output at runtime.
 *
 * These facilities are created with three goals in mind:
 * 1) Allow stats to be independent of stats output: Simulator code is only
 *    concerned with creating, naming, describing and updating a hierarchy of
 *    stats. We can then use a variety of *stats backends* to traverse and
 *    output the stats, either periodically or at specific events.
 * 2) High-performance stats: Updating counters should be as fast as updating raw
 *    integers. Counters are objects though, so they entail some space overhead.
 * 3) Allow fixed-size stats output: The stat types supported are all fixed-size,
 *    and stats cannot be created after initialization. This allows fixed-size records,
 *    making periodic stats much easier to parse and **iterate over** (e.g. we can
 *    parse 1% of the samples for a high-level graph without bringing the whole stats
 *    file from disk, then zoom in on a specific portion, etc.).
 *
 * This design was definitely influenced by the M5 stats facilities, however,
 * it is significantly simpler, doesn't use templates or has formula support,
 * and has an emphasis on fixed-size records for periodic stats.
 */

#ifndef STATS_H_
#define STATS_H_

/* TODO: I want these to be POD types, but polymorphism (needed by dynamic_cast) probably disables it. Dang. */

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <sstream>
#include <stdint.h>
#include <string>
#include <tuple>
#include <vector>
#include "sim/assert.h"

using std::vector;
using std::string;

class Stat  {
    protected:
        const char* _name;
        const char* _desc;

    public:
        Stat() : _name(nullptr), _desc(nullptr) {}

        virtual ~Stat() {}

        const char* name() const {
            assert(_name);
            return _name;
        }

        const char* desc() const {
            assert(_desc);
            return _desc;
        }

        // Called before every dump, for stats that need to be updated to reflect the latest value
        virtual void update() {}

    protected:
        virtual void initStat(const char* name, const char* desc) {
            assert(name);
            assert(desc);
            assert(!_name);
            assert(!_desc);
            _name = name;
            _desc = desc;
        }
};

class AggregateStat : public Stat {
    private:
        vector<Stat*> _children;
        bool _isMutable;
        bool _isRegular;

    public:
        /* An aggregate stat is regular if all its children are 1) aggregate and 2) of the same type (e.g. all the threads).
         * This lets us express all the subtypes of instances of a common datatype, and this collection as an array. It is
         * useful with HDF5, where we would otherwise be forced to have huge compund datatypes, which HDF5 can't do after some
         * point.
         */
        explicit AggregateStat(bool isRegular = false) : Stat(), _isMutable(true), _isRegular(isRegular) {}

        void init(const char* name, const char* desc) {
            assert(_isMutable);
            initStat(name, desc);
        }

        //Returns true if it is a non-empty type, false otherwise. Empty types are culled by the parent.
        bool makeImmutable() {
            if (_isMutable) {
                assert(_name != nullptr); //Should have been initialized
                _isMutable = false;
                vector<Stat*>::iterator it;
                vector<Stat*> newChildren;
                for (it = _children.begin(); it != _children.end(); it++) {
                    Stat* s = *it;
                    AggregateStat* as = dynamic_cast<AggregateStat*>(s);
                    if (as) {
                        bool emptyChild = as->makeImmutable();
                        if (!emptyChild) newChildren.push_back(s);
                    } else {
                        newChildren.push_back(s);
                    }
                }
                _children = newChildren;
            }
            return _children.size() == 0;
        }

        // Collapses 1-element aggregates (TODO: Maintain names...)
        Stat* collapse() {
            assert(_isMutable);
            assert(_name != nullptr);
            vector<Stat*> newChildren;
            for (Stat* s : _children) {
                AggregateStat* as = dynamic_cast<AggregateStat*>(s);
                if (as) {
                    Stat* c = as->collapse();
                    if (c != s) delete s;
                    newChildren.push_back(c);
                } else {
                    newChildren.push_back(s);
                }
            }
            _children = newChildren;
            if (_children.size() == 1) {
                return _children[0];
            } else {
                return this;
            }
        }

        void append(Stat* child) {
            assert(_isMutable);
            _children.push_back(child);
        }

        uint32_t size() const {
            assert(!_isMutable);
            return _children.size();
        }

        bool isRegular() const {
            return _isRegular;
        }

        Stat* get(uint32_t idx) const {
            return _children[idx];
        }

        // Access-while-mutable interface
        uint32_t curSize() const {
            return _children.size();
        }
};

/*  General scalar & vector classes */

class ScalarStat : public Stat {
    public:
        ScalarStat() : Stat() {}

        virtual void init(const char* name, const char* desc) {
            initStat(name, desc);
        }

        virtual uint64_t get() const = 0;
};

class VectorStat : public Stat {
    protected:
        std::vector<std::string> _counterNames;

    public:
        VectorStat() {}

        virtual uint64_t count(uint32_t idx) const = 0;
        virtual uint32_t size() const = 0;

        inline bool hasCounterNames() const {
            return _counterNames.size();
        }

        inline const char* counterName(uint32_t idx) const {
            return _counterNames.size()? _counterNames[idx].c_str() : nullptr;
        }
};


class Counter : public ScalarStat {
    private:
        uint64_t _count;

    public:
        Counter() : ScalarStat(), _count(0) {}

        void init(const char* name, const char* desc) override {
            initStat(name, desc);
            _count = 0;
        }

        inline void inc(uint64_t delta) {
            _count += delta;
        }

        inline void inc() {
            _count++;
        }

    	uint64_t get() const override {
            return _count;
        }

        inline void set(uint64_t data) {
            _count = data;
        }
};

template<typename T=size_t>
class RunningStat : public Stat {
    private:
        // Running moments
        // mn = sum(weight_i)
        // mx = sum(weight_i*x_i)
        // mx2 = sum(weight_i*(x_i^2))
        // Then wmean = mx/mn and wvar = mx2/mn
        uint64_t mn, mx, mx2, max;

    public:
        RunningStat() : Stat(), mn(0), mx(0), mx2(0), max(0) {}

        void init(const char* name, const char* desc) {
            initStat(name, desc);
        }

        string get() const {
            std::ostringstream str;
            double mean = ((double)mx)/mn;
            double stddev = std::sqrt(((double)mx2)/mn - mean*mean);
            str << mean << " (mean), "
                << stddev << " (stddev) "
                << max << " (max) "
                << "[" << mn << " samples]";
            return str.str();
        }

        void push(T x, uint64_t weight=1) {
            mn += weight;
            mx += weight*x;
            mx2 += weight*x*x;
            max = std::max(x, max);
        }

         std::tuple<uint64_t, uint64_t, uint64_t> getMoments() {
             return std::make_tuple(mn, mx, mx2);
         }
};

class VectorCounter : public VectorStat {
    private:
        vector<uint64_t> _counters;

    public:
        VectorCounter() : VectorStat() {}

        /* Without counter names */
        virtual void init(const char* name, const char* desc, uint32_t size) {
            initStat(name, desc);
            assert(size > 0);
            _counters.resize(size);
            std::fill(_counters.begin(), _counters.end(), 0ul);
        }

        /* With counter names */
        virtual void init(const char* name, const char* desc, const vector<std::string>& counterNames) {
            init(name, desc, counterNames.size());
            _counterNames = counterNames;
        }

        inline void inc(uint32_t idx, uint64_t value) {
            _counters[idx] += value;
        }

        inline void inc(uint32_t idx) {
             _counters[idx]++;
        }

        inline void set(uint32_t idx, uint64_t data) {
            _counters[idx] = data;
        }

        inline uint64_t count(uint32_t idx) const override {
            return _counters[idx];
        }

        inline uint64_t sum() const {
            return std::accumulate(_counters.begin(), _counters.end(), 0ul);
        }

        inline uint32_t size() const override {
            return _counters.size();
        }

        inline VectorCounter& operator=(const VectorCounter& that) = default;

        // TODO consider unifying VectorCounter with std::valarray to avoid
        // re-implementing these operators.
        inline VectorCounter& operator+=(const VectorCounter& that) {
            assert(that.size() == size());
            std::transform(_counters.begin(), _counters.end(),
                           that._counters.begin(),
                           _counters.begin(),
                           std::plus<uint64_t>());
            return *this;
        }

        inline VectorCounter& operator-=(const VectorCounter& that) {
            assert(that.size() == size());
            std::transform(_counters.begin(), _counters.end(),
                           that._counters.begin(),
                           _counters.begin(),
                           std::minus<uint64_t>());
            return *this;
        }
};

// FIXME this could probably benefit from the C++ move concept, but I imagine
// std::valarray has that covered.
static inline VectorCounter operator-(const VectorCounter& lhs, const VectorCounter& rhs) {
    VectorCounter result = lhs;
    result -= rhs;
    return result;
}

template<typename T>
struct Allowed; // undefined for bad types!
template<> struct Allowed<uint64_t> { };
template<> struct Allowed<uint32_t> { };
template<> struct Allowed<float> { };
template<> struct Allowed<double> { };

template<typename T=uint64_t>
class Histogram : public VectorStat, private Allowed<T> {
public:
    Histogram(T lowerBound, T binSize, uint64_t numBins) :
        _lowerBound(lowerBound), _binSize(binSize), _numBins(numBins), _count(_numBins+1, 0) {}

    Histogram() : _lowerBound(0), _binSize(T(100)), _numBins(10), _count(_numBins+1,0) {}

    /* Without bin names */
    void init(const char* name, const char* desc, uint64_t numBins, T binSize=10, T lowerBound=0) {
        initStat(name, desc);
        _lowerBound = lowerBound;
        _numBins = numBins;
        _binSize = binSize;
        _count.resize(_numBins+1);
        std::fill(_count.begin(), _count.end(), 0ul);
        _count[_numBins] = static_cast<uint64_t>(_binSize);
    }

    /* With bin names */
    virtual void init(const char* name, const char* desc, const vector<std::string>& binNames) {
        init(name, desc, binNames.size());
        _counterNames = binNames;
    }

    void insert(T value, uint64_t weight) {
        T distance   = value - _lowerBound;
        size_t idx = std::floor(distance/_binSize);
        assert(idx >= 0);
        if (likely(((idx >= 0) && (idx < _numBins)))) _count[idx] += weight;
        else { idx = coarsen(value); _count[idx] += weight; }
    }

    inline uint64_t count(uint32_t idx) const override { assert(idx <= _numBins); return _count[idx]; }
    inline uint32_t size() const override { return _numBins+1; }

private:
    size_t coarsen(T value) {
        T distance = value - _lowerBound;
        while (floor(distance/_binSize) >= _numBins) {
            _binSize *= 2;
            for (uint64_t j = 0; j < _numBins/2; ++j) {
                _count[j] = _count[2*j] + _count[2*j+1];
            }
            std::fill(&_count[_numBins / 2], &_count[_numBins], 0);
        }
        _count[_numBins] = static_cast<uint64_t>(_binSize);
        return std::floor(distance/_binSize);
    }

private:
    T                   _lowerBound;
    T                   _binSize;
    uint64_t            _numBins;
    vector<uint64_t>    _count;
};

/*
 * Generic lambda stats
 * If your stat depends on a formula, this lets you encode it compactly using C++11 lambdas
 *
 * Usage example:
 *  auto x = [this]() { return curCycle - haltedCycles; }; //declare the lambda function that computes the stat; note this is captured because these values are class members
 *  LambdaStat<decltype(x)>* cyclesStat = new LambdaStat<decltype(x)>(x); //instantiate the templated stat. Each lambda has a unique type, which you get with decltype
 *  cyclesStat->init("cycles", "Simulated cycles"); //etc. Use as an usual stat!
 */
template <typename F>
class LambdaStat : public ScalarStat {
    private:
        F f;

    public:
        explicit LambdaStat(F _f) : f(_f) {} //copy the lambda
        uint64_t get() const override {return f();}
};

template<typename F>
class LambdaVectorStat : public VectorStat {
    private:
        F f;
        uint32_t s;

    public:
        LambdaVectorStat(F _f, uint32_t _s) : VectorStat(), f(_f), s(_s) {}
        uint32_t size() const override { return s; }
        uint64_t count(uint32_t idx) const override { //dsm: Interestingly, this compiles even if f() is not const. gcc may catch this eventually...
            assert(idx < s);
            return f(idx);
        }
};

// Convenience creation functions
template <typename F>
LambdaStat<F>* makeLambdaStat(F f) { return new LambdaStat<F>(f); }

template<typename F>
LambdaVectorStat<F>* makeLambdaVectorStat(F f, uint32_t size) { return new LambdaVectorStat<F>(f, size); }

//Stat Backends declarations.

class StatsBackend  {
    public:
        StatsBackend() {}
        virtual ~StatsBackend() {}
        virtual void dump(bool buffered)=0;
};


class TextBackendImpl;

class TextBackend : public StatsBackend {
    private:
        TextBackendImpl* backend;

    public:
        TextBackend(const char* filename, AggregateStat* rootStat);
        void dump(bool buffered) override;
};


class HDF5BackendImpl;

class HDF5Backend : public StatsBackend {
    private:
        HDF5BackendImpl* backend;

    public:
        HDF5Backend(const char* filename, AggregateStat* rootStat, size_t bytesPerWrite, bool skipVectors, bool sumRegularAggregates);
        void dump(bool buffered) override;
};

#endif  // STATS_H_
