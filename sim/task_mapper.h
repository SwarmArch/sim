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

#include <algorithm>
#include <random>
#include <unordered_map>
#include <utility>
#include <vector>
#include <boost/circular_buffer.hpp>
#include <boost/range/irange.hpp>
#include "sim/str.h"
#include "sim/log.h"
#include "sim/assert.h"
#include "sim/memory/hash.h"
#include "sim/stats/stats.h"
#include "sim/load.h"
#include "sim/bloom/h3array.h"
#include "sim/bloom/custom_hash.h"
#include "sim/memory/memory_partition.h"

using std::vector;
using boost::circular_buffer;
using std::pair;
using std::unordered_map;

// FIXME(mcj & dsm) Stop using ossinfo->robs. Instead, set up a reference or
// pointer to robs in the classes that use it and pass it during construction,
// or add a setter method. If many TaskMapper children use robs, that
// indicates TaskMapper should have a reference/pointer to robs.

// Armadillo linear algebra library (FIXME: With more users, pull this to a common importing header file)
// #define ARMA_DEBUG(...) do { info(__VA_ARGS__); } while(0)
#define ARMA_DEBUG(...) do { ; } while(0)

#define ARMA_DONT_USE_CXX11  // dsm: for whatever reason, Armadillo breaks without this

#include "sim/log.h"
#include <armadillo>

#undef DEBUG
#define DEBUG(args...) //info(args)

class Network;

class TaskMapper {
  public:
    enum PhaseEvent { CYCLE, COMMIT, DEQUEUE };

  private:
    const PhaseEvent event;
    uint64_t eventWindow;
    uint64_t lastEventCount;
    RunningStat<size_t> updateInterval;
    uint64_t lastUpdateCycle;

  public:
    TaskMapper()
        : event(CYCLE),
          eventWindow(10000),
          lastEventCount(0),
          lastUpdateCycle(0) {}

    TaskMapper(PhaseEvent _event, uint64_t _eventWindow)
        : event(_event),
          eventWindow(_eventWindow),
          lastEventCount(0),
          lastUpdateCycle(0) {}

    // Some task mappers periodically sample stats from various ROBs
    // and accordingly reconfigure any internal mappings.
    void sample() {
        DEBUG("[%lu] Sampling...", getCurCycle());
        uint64_t currentEventCount = 0;
        switch (event) {
            case CYCLE:
                currentEventCount = getCurCycle();
                break;
            case COMMIT:
                for (uint32_t i = 0; i < ossinfo->robs.size(); ++i)
                    currentEventCount +=
                        ossinfo->robs[i]->getCommits().first;  // get only real task
                                                     // commits (not
                                                     // spiller, requeuer)
                break;
            case DEQUEUE:
                for (uint32_t i = 0; i < ossinfo->robs.size(); ++i)
                    currentEventCount +=
                        ossinfo->robs[i]->getDequeues().first;  // get only real task
                                                      // commits (not
                                                      // spiller, requeuer)
                break;
        };

        assert(currentEventCount >= lastEventCount);
        if ((currentEventCount - lastEventCount) >= eventWindow) {
            DEBUG("Going to update...");
            update();
            uint64_t currentCycle = getCurCycle();
            uint64_t interval = currentCycle - lastUpdateCycle;
            updateInterval.push(interval);
            lastUpdateCycle = currentCycle;
            lastEventCount = currentEventCount;
        }
    }

    uint32_t getDstROBIdx(uint32_t robIdx, const Task* parent, uint64_t hint,
                          bool noHash);

    // Only used by tmStat
    void initTmStats(AggregateStat* parentStat) {
        AggregateStat* tmStats = new AggregateStat();
        tmStats->init("tm", "Task Mapper stats");

        updateInterval.init("updateInterval", "update interval cycles");
        tmStats->append(&updateInterval);

        initStatsCustom(tmStats);
        parentStat->append(tmStats);
    }

    virtual void initStats(AggregateStat* parentStat) {}

    virtual void initStatsCustom(AggregateStat* tmStats) {}
    virtual void updateStats() {}

    virtual void update() {}

    virtual LoadStatsSummary* getLoadStatsSummary(Task* task) { return nullptr; }
    virtual uint64_t getMeanReconfigInterval() { return 0; }

    virtual void newTask(Task* task) {}
    virtual void spillTask(Task* task) {}
    virtual void stealTask(Task* task) {}

    virtual ~TaskMapper() = default;

  protected:
    virtual uint32_t getDstROBIdx(uint32_t robIdx, const Task* parent, uint64_t hint) = 0;
};

class RandomTaskMapper : public TaskMapper {
  private:
    std::mt19937 gen;
    std::uniform_int_distribution<uint32_t> dist;

  public:
    RandomTaskMapper() : TaskMapper(), gen(42), dist(0, UINT32_MAX) {}

  protected:
    uint32_t getDstROBIdx(uint32_t, const Task* parent, uint64_t) override {
        if (parent && parent->isRequeuer()) {
            // Before Swarm was modded with Hints, a requeuer's child was always
            // re-enqueued to the tile from which it came. When the
            // HintTaskMapper is enabled, we still achieve that behavior. With
            // load-balanced hints, a requeuer's child may be re-mapped
            // elsewhere, but it is sent to the tile that now owns its hint.
            // With Random, we have to manually send the requeuer's child back
            // to the tile from whence it came.
            // FIXME(mcj) there was a time when this wasn't broken: we
            // randomly-generated all hints, but effectively always used the
            // HintTaskMapper, so that we could use a task's hint to send it
            // back to its original tile.
            return parent->container()->getIdx();
        } else {
            return dist(gen) % ossinfo->robs.size();
        }
    }
};

class HintTaskMapper : public TaskMapper {
  private:
    H3HashFamily hashFn;
    const MemoryPartition* memPart;

  public:
    HintTaskMapper(const MemoryPartition* _memPart)
        : TaskMapper(), hashFn(2, 64, 0xBADBAE0FF), memPart(_memPart) {}

  protected:
    uint32_t getDstROBIdx(uint32_t, const Task* parent, uint64_t hint) override {
        if (memPart) {
            int32_t partId = memPart->getPartId(hint, ossinfo->robs.size());
            if (partId >= 0) return partId;
        }
        const uint32_t idx = hashFn.hash(1, hashFn.hash(0, hint))
                                % ossinfo->robs.size();
        // With a simple hashed-hint mapping, if the parent task is a
        // requeuer, then we expect all its children to be mapped to the
        // same tile again. The requeuer's children were originally mapped
        // to that tile via this hash function, so the hash function should
        // send them there again.
        // NOTE(dsm): This doesn't work well with stealing, which rehints
        // tasks, because the spillers save the old hint, which results in
        // the task going back to where it was..
#if 0
        assert_msg(!parent || !parent->isRequeuer() ||
                       idx == parent->container()->getIdx(),
                   "idx %d pcontainer %d hint %ld parent %s", idx,
                   parent->container()->getIdx(), hint,
                   parent->toString().c_str());
#else
        if (parent && parent->isRequeuer() && idx != parent->container()->getIdx()) {
            warn("Enqueuing a requeuer child for a different tile; requeuer was stolen?");
        }
#endif
        return idx;
    }
};

/**
 * Similar to the HintTaskMapper, but periodically changes the task hash
 * function. Since Random is good at load balancing, but poor for locality, and
 * Hint is good at locality, but poor for load balance, tuning the frequency of
 * reconfigurations here might strike a balance.
 */
class BlindRedistributionTaskMapper : public TaskMapper {
    H3HashFamily* hashFn;

    // For deterministically seeding the hash functions:
    std::mt19937 gen;
    std::uniform_int_distribution<uint64_t> dist;

  public:
    BlindRedistributionTaskMapper()
        : TaskMapper(), hashFn(nullptr), gen(42), dist(0, UINT64_MAX) {
        update();
    }

    ~BlindRedistributionTaskMapper() override {
        if (hashFn) delete hashFn;
    }

  protected:
    uint32_t getDstROBIdx(uint32_t, const Task*, uint64_t hint) override {
        return hashFn->hash(0, hint) % ossinfo->robs.size();
    }

    void update() override {
        if (hashFn) delete hashFn;
        hashFn = new H3HashFamily(1, 32, dist(gen));
    }
};

class LeastLoadedTaskMapper : public TaskMapper {
  public:
    LeastLoadedTaskMapper(LoadArray::LoadMetric _metric) : load() {
        load.init(ossinfo->numROBs /*r*/, 1 /*c*/, _metric);
        lastPhaseLoads.resize(ossinfo->numROBs, 0L);
    }

    LoadStatsSummary* getLoadStatsSummary(Task* task) override {
        uint32_t r = task->container()->getIdx();
        return load.getLoadStatsSummary(r, 0);
    }

  protected:
    uint32_t getDstROBIdx(uint32_t, const Task*, uint64_t) override {
        uint32_t minIdx = -1u;
        size_t minSz = -1ul;
        for (uint32_t i = 0; i < ossinfo->robs.size(); i++) {
            size_t sz = lastPhaseLoads[i];
            if (sz < minSz) {
                minSz = sz;
                minIdx = i;
            }
        }
        assert(minIdx < ossinfo->robs.size());
        return minIdx;
    }

    void spillTask(Task* task) override { load.spillTask(task->container()->getIdx(), 0); }
    void stealTask(Task* task) override { load.stealTask(task->container()->getIdx(), 0); }

    void update() override {
        for (uint32_t r = 0; r < ossinfo->numROBs; ++r)
            lastPhaseLoads[r] = load.getLoad(r, 0);
    }

  private:
    LoadArray load;
    vector<uint64_t> lastPhaseLoads;
};

// Uses "power of 2 choices" to load balance while taking hints into account
class HintPow2TaskMapper : public TaskMapper {
  private:
    H3HashFamily hashFn;

  public:
    HintPow2TaskMapper(LoadArray::LoadMetric _metric)
        : TaskMapper(), hashFn(2, 32,0xBADBAE0FF), load() {
        load.init(ossinfo->numROBs /*r*/, 1 /*c*/, _metric);
        lastPhaseLoads.resize(ossinfo->numROBs, 0L);
    }

    LoadStatsSummary* getLoadStatsSummary(Task* task) override {
        uint32_t r = task->container()->getIdx();
        return load.getLoadStatsSummary(r, 0);
    }

  protected:
    uint32_t getDstROBIdx(uint32_t, const Task*, uint64_t hint) override {
        auto robHash = [this, hint](uint32_t k) {
            return (hashFn.hash(k, hint)) % ossinfo->robs.size();
        };

        uint32_t i0 = robHash(0);
        uint32_t i1 = robHash(1);
        // Add a 20% hysteresis, so i0 and i1 don't ping-pong tasks to each
        // other
        return (lastPhaseLoads[i0] <= lastPhaseLoads[i1] * 120 / 100) ? i0 : i1;
    }

    void update() override {
        for (uint32_t r = 0; r < ossinfo->numROBs; ++r)
            lastPhaseLoads[r] = load.getLoad(r, 0);
    }

    void spillTask(Task* task) override { load.spillTask(task->container()->getIdx(), 0); }
    void stealTask(Task* task) override { load.stealTask(task->container()->getIdx(), 0); }

  private:
    LoadArray load;
    vector<uint64_t> lastPhaseLoads;
};

class BucketTaskMapper : public TaskMapper {
  protected:
    typedef vector<vector<uint64_t>> LoadStats;
    H3HashFamily hashFn;

    // Configuration
    // FIXME(mcj) many children of TaskMapper use a global reference to the
    // ROBArray rather than an object instance reference.
    // I now realize my adding numROBs here is moot, since this class could
    // just use robs.size() for the same value. Or neighborRobs.size(). There
    // are too many ways to do the same thing in this file (e.g. the value of
    // the number of ROBs can be gleaned in too many ways. That means state is
    // duplicated, which means they might divert and cause bugs). The root
    // problem seems to be global variables, or use of ossinfo. Should
    // TaskMapper itself have a reference or pointer to ROBArray with which it
    // is constructed?
    const uint32_t numROBs;
    const uint32_t bucketsPerRob;
    const uint32_t guard;
    const uint32_t leash;
    const uint32_t window;
    const bool     donateAscending;
    const bool     donateAll;
    const bool     includeDonated;
    const bool     shouldReconfigure;
    vector<uint32_t> homeRob;
    vector<vector<uint32_t>> neighborRobs;

    // State
    vector<uint32_t> bucketToRob;   // Holds the mapping of buckets to ROB
    circular_buffer<LoadStats> windowStats;
                        // Holds all load related stats across the window
    LoadArray load;     // Move these out, and just hold a pointer?
    UniformityPolicy *uPolicy;
    LoadStats prevPhaseStats;       // Only to compute the diff load
    LoadStats phaseSpilled;       // Number of tasks spilled in this phase
    unordered_map<const Task*, uint32_t> taskToBucket;

    Network& network;

    // Stats
    VectorCounter   bucketLoads;
    VectorCounter   robLoads;
    VectorCounter   spillLoads;     // for each ROB
    RunningStat<size_t> reconfigIntervalCycles;
    RunningStat<size_t> averageActiveBuckets;
    RunningStat<size_t> maximumActiveBuckets;
    RunningStat<size_t> maximumMappedBuckets;
    uint64_t        lastReconfigCycle;

    // Functions
    virtual void reconfigure(const arma::Mat<uint64_t>& diffRobLoads,
                     const LoadStats& cStats) = 0;
    uint32_t getBucket(uint64_t hint) {
        return hashFn.hash(1, hashFn.hash(0, hint))
                % (ossinfo->robs.size() * bucketsPerRob);
    }
    void adjustPhaseStatsForSpills(vector<uint64_t>& cyclesPerBucket,
                                   const vector<uint64_t>& spillCount);
    uint64_t getRobLoad(uint32_t r);
    uint64_t getSpillLoad(uint32_t r);
    uint64_t getBucketLoad(uint32_t b);

  public:
    BucketTaskMapper(PhaseEvent _event, uint64_t _eventWindow,
                     uint32_t _numROBs, uint32_t _bucketsPerRob,
                     uint32_t _guard, uint32_t _leash, uint32_t _window,
                     bool _donateAscending, bool _donateAll,
                     bool _includeDonated, bool _shouldReconfigure,
                     LoadArray::LoadMetric _metric, UniformityPolicy* _uPolicy,
                     Network& _network);
    void update() override;

    void initStats(AggregateStat* parentStat) override {
        AggregateStat* tmStats = new AggregateStat();
        tmStats->init("btm", "Bucket Task Mapper Stats");

        averageActiveBuckets.init("avgActiveBuckets", "average active buckets");
        tmStats->append(&averageActiveBuckets);
        maximumActiveBuckets.init("maxActiveBuckets", "maximum active buckets");
        tmStats->append(&maximumActiveBuckets);
        maximumMappedBuckets.init("maxMappedBuckets", "maximum mapped buckets");
        tmStats->append(&maximumMappedBuckets);

        parentStat->append(tmStats);
    }

    uint64_t getMeanReconfigInterval() override {
        auto runningTuple = reconfigIntervalCycles.getMoments();
        if (std::get<0>(runningTuple) != 0)
            return std::ceil(std::get<1>(runningTuple) /
                             std::get<0>(runningTuple));
        else
            return 0;
    }

    void initStatsCustom(AggregateStat* tmStats) override {
        bucketLoads.init("bucketLoads", "load per bucket",
                         ossinfo->robs.size() * bucketsPerRob);
        tmStats->append(&bucketLoads);
        robLoads.init("robLoads", "load per rob (without spills)",
                      ossinfo->robs.size());
        tmStats->append(&robLoads);
        spillLoads.init("spillLoads", "spill load per rob", ossinfo->robs.size());
        tmStats->append(&spillLoads);
        reconfigIntervalCycles.init("reconfigInterval",
                                    "reconfiguration interval cycles");
        tmStats->append(&reconfigIntervalCycles);
    }

    void updateStats() override {
        for (uint32_t r = 0; r < ossinfo->robs.size(); ++r) {
            robLoads.set(r, getRobLoad(r));
            spillLoads.set(r, getSpillLoad(r));
        }
        for (uint32_t b = 0; b < ossinfo->robs.size() * bucketsPerRob; ++b)
            bucketLoads.set(b, getBucketLoad(b));
    }

    LoadStatsSummary* getLoadStatsSummary(Task* task) override {
        uint32_t r = task->container()->getIdx();
        uint32_t b = taskToBucket[task];

        assert_msg(getBucket(task->hint) == b || task->noHashHint,
                   "task: %s, hint: %lu, isSpiller/Requeuer: %s, bucket: %u",
                   task->toString().c_str(), task->hint,
                   (task->isSpiller() || task->isRequeuer()) ? "yes" : "no",
                   b);
        assert(b < bucketToRob.size());
        assert(r < numROBs);

        return load.getLoadStatsSummary(r, b);
    }

    uint32_t getDstROBIdx(uint32_t, const Task*, uint64_t hint) override {
        uint32_t bucket = getBucket(hint);
        assert(bucket != numROBs * bucketsPerRob);
        return bucketToRob[bucket];
    }

    void newTask(Task* task) override {
        if (task->isSpiller() || task->isRequeuer()) {
            taskToBucket[task] = bucketsPerRob * numROBs;
        } else if (task->noHashHint) {
            uint32_t r = task->hint % numROBs;
            uint32_t b = std::find(bucketToRob.begin(), bucketToRob.end(), r) -
                         bucketToRob.begin();
            assert(b < bucketToRob.size());
            taskToBucket[task] = b;
        } else {
            taskToBucket[task] = getBucket(task->hint);
        }
    }

    void spillTask(Task* task) override {
        assert(taskToBucket.count(task));
        uint32_t b = taskToBucket[task];
        uint32_t r = task->container()->getIdx();
        assert((b < bucketToRob.size() - 1) ||
               (task->isRequeuer() ||
                task->isSpiller()));  // only spillers/requeuers can be in
                                        // the last bucket
        phaseSpilled[r][b] += 1;
        load.spillTask(r, b);
    }

    void stealTask(Task* task) override { load.stealTask(task->container()->getIdx(), taskToBucket[task]); }
};

class DeadBeatBucketTaskMapper : public BucketTaskMapper {
  public:
    DeadBeatBucketTaskMapper(PhaseEvent _event, uint64_t _eventWindow,
                             uint32_t _numROBs, uint32_t _bucketsPerRob,
                             uint32_t _guard, uint32_t _leash, uint32_t _window,
                             bool _donateAscending, bool _donateAll,
                             bool _includeDonated, bool _shouldReconfigure,
                             LoadArray::LoadMetric _metric,
                             UniformityPolicy* _uPolicy, Network& _network)
        : BucketTaskMapper(_event, _eventWindow, _numROBs, _bucketsPerRob,
                           _guard, _leash, _window, _donateAscending,
                           _donateAll, _includeDonated, _shouldReconfigure,
                           _metric, _uPolicy, _network) {}

  protected:
    void reconfigure(const arma::Mat<uint64_t>& diffRobLoads,
                     const LoadStats& cStats) override;
};

class ProportionalBucketTaskMapper : public BucketTaskMapper {
public:
  ProportionalBucketTaskMapper(double _Kp, PhaseEvent _event,
                               uint64_t _eventWindow, uint32_t _numROBs,
                               uint32_t _bucketsPerRob, uint32_t _guard,
                               uint32_t _leash, uint32_t _window,
                               bool _donateAscending, bool _donateAll,
                               bool _includeDonated, bool _shouldReconfigure,
                               LoadArray::LoadMetric _metric,
                               UniformityPolicy* _uPolicy, Network& _network)
      : BucketTaskMapper(_event, _eventWindow, _numROBs, _bucketsPerRob, _guard,
                         _leash, _window, _donateAscending, _donateAll,
                         _includeDonated, _shouldReconfigure, _metric, _uPolicy,
                         _network),
        Kp(_Kp) {}

protected:
    void reconfigure(const arma::Mat<uint64_t>& diffRobLoads,
                     const LoadStats& cStats) override;

private:
    double Kp;  // Proportional factor
};

// dsm: Help-first algorithm (see help-first vs work-first strategies). Similar
// flavor to work-stealing, but at mapping time, without rebalancing.
class HelpFirstTaskMapper : public TaskMapper {
  private:
      const uint32_t underflowThreshold;

  public:
    HelpFirstTaskMapper(uint32_t ut) : TaskMapper(), underflowThreshold(ut) {}

  protected:
    uint32_t getDstROBIdx(uint32_t robIdx, const Task* parent, uint64_t) override {
        if (parent && (parent->isSpiller() || parent->isRequeuer())) return robIdx;
        auto& rv = ossinfo->robs;
        // If we are underflowing, queue to ourselves. Otherwise, donate task
        // to first ROB in need.
        // NOTE: Because enqueues are delayed, we may send a few tasks than
        // needed to the underflowed node...
        for (uint32_t i = 0; i < rv.size(); i++) {
            uint32_t d = (i + robIdx) % rv.size();
            if (rv[d]->taskQueueSize() <= underflowThreshold) return d;
        }
        return robIdx;
    }
};

class SelfTaskMapper : public TaskMapper {
  public:
    SelfTaskMapper() : TaskMapper() {}

  protected:
    uint32_t getDstROBIdx(uint32_t robIdx, const Task* parent, uint64_t) override {
        return robIdx;
    }
};
