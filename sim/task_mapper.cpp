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

#include <queue>
#include <deque>
#include <numeric>
#include <cmath>
#include "sim/task_mapper.h"
#include "sim/network.h"

#undef DEBUG
#define DEBUG(args...) //info(args)

uint32_t TaskMapper::getDstROBIdx(uint32_t robIdx, const Task* parent,
                                  uint64_t hint, bool noHash) {
    uint32_t dstRobIdx;
    if (noHash) {
        // Regardless of the implementation, if the programmer specified NOHASH,
        // then map the task using the hint modulo robs.
        dstRobIdx = hint % ossinfo->robs.size();
    } else {
        dstRobIdx = getDstROBIdx(robIdx, parent, hint);
    }

    DEBUG("enqueue noHash %d, hint %ld robIdx %d", noHash, hint, dstRobIdx);
    return dstRobIdx;
}

BucketTaskMapper::BucketTaskMapper(
    PhaseEvent _event, uint64_t _eventWindow, uint32_t _numROBs,
    uint32_t _bucketsPerRob, uint32_t _guard, uint32_t _leash, uint32_t _window,
    bool _donateAscending, bool _donateAll, bool _includeDonated,
    bool _shouldReconfigure, LoadArray::LoadMetric _metric,
    UniformityPolicy* _uPolicy, Network& _network)
    : TaskMapper(_event, _eventWindow),
      hashFn(2, 64, 0xBADBAE0FF),
      numROBs(_numROBs),
      bucketsPerRob(_bucketsPerRob),
      guard(_guard),
      leash(_leash),
      window(_window),
      donateAscending(_donateAscending),
      donateAll(_donateAll),
      includeDonated(_includeDonated),
      shouldReconfigure(_shouldReconfigure),
      load(),
      uPolicy(_uPolicy),
      network(_network),
      lastReconfigCycle(0) {
    uint32_t totalBuckets =
        bucketsPerRob * numROBs +
        1;  // +1: we use a separate bucket for spiller/requeuer
    bucketToRob.resize(totalBuckets);
    homeRob.resize(totalBuckets);
    neighborRobs.resize(numROBs);

    // 1. Set home rob for bucket
    for (uint32_t b = 0; b < totalBuckets; ++b) {
        bucketToRob[b] = b % numROBs;
        homeRob[b] = b % numROBs;
    }

    // 2. Get neighbors
    for (uint32_t r = 0; r < numROBs; r++) {
        network.getNeighbors(r, leash, neighborRobs[r]);
        DEBUG("ROB %u neighbors :: ", r);
        for (auto& x : neighborRobs[r]) {
            (void)x;
            DEBUG("\t%u", x);
        }
    }

    // 3. Initialize stats state
    windowStats.set_capacity(window);
    load.init(numROBs/*r*/, totalBuckets/*c*/, _metric);
    prevPhaseStats.resize(numROBs);
    phaseSpilled.resize(numROBs);
    for (uint32_t r = 0; r < numROBs; ++r) {
        prevPhaseStats[r].resize(totalBuckets, 0L);
        phaseSpilled[r].resize(totalBuckets, 0L);
    }

}

// The three functions below are only used to update the stats.
// Gets ROB load without spills.
// This includes all buckets at the ROB (not just those mapped to the ROB in
// this phase). It's a little tedious to get the load corresponding to just the
// buckets mapped to the ROB. If we were dumping tmStats by phase instead of
// every N cycles, then we can simply update the stats in update().
uint64_t BucketTaskMapper::getRobLoad(uint32_t r) {
    uint64_t value = 0;
    for (uint32_t b = 0; b < ossinfo->robs.size() * bucketsPerRob; ++b)
        value += load.getLoad(r, b);
    return value;
}

uint64_t BucketTaskMapper::getSpillLoad(uint32_t r) {
    return load.getLoad(r, ossinfo->robs.size() * bucketsPerRob);
}

// This represents the true bucket load (i.e. commits for a bucket
// across the entire system). Getting bucket load only from the ROB
// it was mapped to is tedious (note that we want to maintain monotonocity
// for bucket loads).
uint64_t BucketTaskMapper::getBucketLoad(uint32_t b) {
    uint64_t value = 0;
    for (uint32_t r = 0; r < ossinfo->robs.size(); ++r) value += load.getLoad(r, b);
    return value;
}

void BucketTaskMapper::update() {
    DEBUG("Updating load balancer");

    // 1. Initialize local variables
    uint32_t nr = numROBs;
    uint32_t nb = nr * bucketsPerRob + 1;

    vector<vector<uint32_t>> robBuckets(nr);
    for (uint32_t b = 0; b < nb-1; b++) robBuckets[bucketToRob[b]].push_back(b);

    LoadStats curPhaseStats;
    LoadStats diffPhaseStats;
    LoadStats cumulativeWindowStats;
    curPhaseStats.resize(nr);
    diffPhaseStats.resize(nr);
    cumulativeWindowStats.resize(nr);
    for (uint32_t r = 0; r < nr; ++r) {
        curPhaseStats[r].resize(nb, 0L);
        diffPhaseStats[r].resize(nb, 0L);
        cumulativeWindowStats[r].resize(nb, 0L);
    }

    // 2. Gather load information for this phase
    uint32_t maxActiveBuckets = 0;
    uint32_t maxMappedBuckets = 0;
    uint32_t totalActiveBuckets = 0;
    for (uint32_t r = 0; r < nr; ++r) {
        uint32_t activeBuckets = 0;
        uint32_t mappedBuckets = robBuckets[r].size();
        for (uint32_t b = 0; b < nb; ++b) {
            curPhaseStats[r][b] = load.getLoad(r, b);
            diffPhaseStats[r][b] = curPhaseStats[r][b] - prevPhaseStats[r][b];
            if (diffPhaseStats[r][b]) {
                DEBUG("r: %u, b: %u, cur: %lu, prev: %lu, diff: %lu", r, b,
                      curPhaseStats[r][b], prevPhaseStats[r][b],
                      diffPhaseStats[r][b]);
                // If includeDonated, we have to track cycles for donated
                // buckets too. Else, only if this is a bucket mapped to
                // this RoB, we need to track its stats.
                if (includeDonated || (bucketToRob[b] == r)) activeBuckets++;
            }
        }
        if (activeBuckets > maxActiveBuckets) maxActiveBuckets = activeBuckets;
        if (mappedBuckets > maxMappedBuckets) maxMappedBuckets = mappedBuckets;
        totalActiveBuckets += activeBuckets;

        adjustPhaseStatsForSpills(diffPhaseStats[r], phaseSpilled[r]);
    }
    uint32_t avgActiveBuckets = static_cast<uint32_t>(
        static_cast<float>(totalActiveBuckets) / static_cast<float>(nr));

    // 3. Update state, stats that needs to be updated every phase regardless of
    //    reconfiguration
    prevPhaseStats = curPhaseStats;
    for (uint32_t r = 0; r < nr; ++r)
        for (uint32_t b = 0; b < nb; ++b) phaseSpilled[r][b] = 0L;

    averageActiveBuckets.push(avgActiveBuckets);
    maximumActiveBuckets.push(maxActiveBuckets);
    maximumMappedBuckets.push(maxMappedBuckets);

    // 4. Decide if we must reconfigure -- is there a consistent bias across the
    //    window?
    windowStats.push_back(diffPhaseStats);
    auto robLoad = [&, this](uint32_t r) {
        uint64_t load = 0;
        for (uint32_t w = 0; w < window; ++w) {
            auto phaseDiffStats = windowStats.at(w);
            // Do we count load only for buckets assigned to ROB in this phase
            // or for any bucket that may have committed in this phase. We may
            // have donated the bucket prior, but it still contributes to the
            // load in this phase, due to remnant tasks in the task queue.
            if (includeDonated) {
                for (uint32_t b = 0; b < nb-1; ++b) load += phaseDiffStats[r][b];
            } else {
                for (uint32_t b = 0; b < nb-1; ++b) {
                    if (bucketToRob[b] == r)
                        load += phaseDiffStats[r][b];
                }
            }
        }
        return load;
    };
    arma::Mat<uint64_t> diffRobLoads(nr, 1);

    if (windowStats.size() < window) {
        DEBUG("Not enough phases elapsed since last update.");
        return;
    } else {
        assert(windowStats.size() == window);
        for (uint32_t r = 0; r < nr; r++) {
            diffRobLoads[r] = robLoad(r);
            DEBUG("[-] r:%u, diffRobLoads: %lu", r, diffRobLoads[r]);
        }
        if (uPolicy->isUniform(
                arma::conv_to<vector<uint64_t>>::from(diffRobLoads)))
            return;
        DEBUG("Non-uniform across window, going to reconfigure task mapper.");
    }

    // 5. We must reconfigure
    assert(windowStats.size() == window);
    for (uint32_t w = 0; w < window; ++w) {
        auto phaseDiffStats = windowStats.at(w);
        for (uint32_t r = 0; r < nr; ++r)
            for (uint32_t b = 0; b < nb; ++b)
                cumulativeWindowStats[r][b] += phaseDiffStats[r][b];
    }

    for (uint32_t r = 0; r < nr; ++r)
        for (uint32_t b = 0; b < nb; ++b)
            if (cumulativeWindowStats[r][b])
                DEBUG("r: %u, b: %u, cumulative: %lu", r, b,
                      cumulativeWindowStats[r][b]);

    // 6. We have all the data we need, now reconfigure
    if (shouldReconfigure)
        reconfigure(diffRobLoads, cumulativeWindowStats);
    windowStats.clear();

    // 7. Update some stats
    const uint64_t curCycle = getCurCycle();
    uint64_t interval = curCycle - lastReconfigCycle;
    reconfigIntervalCycles.push(interval);
    lastReconfigCycle = curCycle;
}

void BucketTaskMapper::adjustPhaseStatsForSpills(
    vector<uint64_t>& cyclesPerBucket, const vector<uint64_t>& spillCount) {
    // FIXME Split into spiller and requeuer cycles?
    uint64_t spillCycles = cyclesPerBucket.back();  // Last bucket contains spiller/requeuer cycles
    DEBUG("Total spill cycles: %lu", spillCycles);

    // Distribute spill cycles to various buckets in the ratio of spillCount
    uint64_t totalSpilledTasks = std::accumulate(spillCount.begin(), spillCount.end(), 0);
    for (uint32_t b = 0; b < cyclesPerBucket.size() - 1; ++b) {
        if (spillCount[b]) {
            uint64_t extraCycles = static_cast<uint64_t>(
                static_cast<double>(spillCount[b] * spillCycles) /
                static_cast<double>(totalSpilledTasks));
            DEBUG("Updating bucket %u, #spilled: %lu, adding: %lu cycles", b, spillCount[b], extraCycles);
            cyclesPerBucket[b] += extraCycles;
        }
    }
}

void DeadBeatBucketTaskMapper::reconfigure(const arma::Mat<uint64_t>& diffRobLoads,
                                   const LoadStats& cStats) {
    // 1. Initialize required variables
    uint32_t nr = numROBs;
    uint32_t nb = nr * bucketsPerRob;   // TODO For now do not consider spiller/requeuer cycles

    vector<vector<uint32_t>> updateNeighborRobs = neighborRobs;
    vector<vector<uint32_t>> robBuckets(nr);
    for (uint32_t b = 0; b < nb; b++) robBuckets[bucketToRob[b]].push_back(b);
    arma::Mat<double> rLoads =
        arma::conv_to<arma::Mat<double>>::from(diffRobLoads) /
        arma::accu(diffRobLoads);

    for (uint32_t r = 0; r < nr; r++)
        DEBUG("This window load on ROB %2u: %4lu (%.6f)", r, diffRobLoads[r],
              rLoads[r]);

    // 2. Helper functions
    // This is different from the getBucketLoad() function.
    // It computes the bucket load as a fraction of the total rob load
    // passed into reconfigure(). Importantly, this load only accounts
    // for load associated with the rob that the bucket is mapped to in
    // this phase.
    auto bucketLoad = [&](uint32_t b) {
        uint32_t r = bucketToRob[b];
        return ((float)cStats[r][b] / (float)diffRobLoads[r]) *
               (float)rLoads[r];
    };
    auto cmpBucketsAscending = [&](uint32_t ba, uint32_t bb) {
        return bucketLoad(ba) < bucketLoad(bb);
    };
    auto cmpBucketsDescending = [&](uint32_t ba, uint32_t bb) {
        return bucketLoad(ba) > bucketLoad(bb);
    };

    // 3. For overloaded ROBs, get the buckets to donate
    std::vector<std::vector<uint32_t>> bucketsToRemap(nr);
    std::priority_queue<std::pair<double, uint32_t>>
        toShedRobs;  // top returns max load ROB
    std::vector<std::pair<double, uint32_t>> toLoadRobs;

    const float moe = 0.0;     // FIXME
    for (uint32_t r = 0; r < nr; r++) {
        uint32_t nrb = robBuckets[r].size();
        assert(nrb || donateAll);
        if (nrb == 0) {
            if (rLoads[r] < (1.0 - moe) / nr)
                toLoadRobs.push_back(std::make_pair(rLoads[r], r));
            continue;
        }

        //  TODO Pull into a separate class / function
        uint32_t donated = 0;
        if (donateAscending) {
            // Sort buckets from smallest to largest
            std::sort(robBuckets[r].begin(), robBuckets[r].end(), cmpBucketsAscending);

            // Only donate buckets with > 0 load
            uint32_t first_non_zero =
                std::find_if(
                    robBuckets[r].begin(), robBuckets[r].end(),
                    [&](uint32_t b) -> bool { return bucketLoad(b) > 0; }) -
                robBuckets[r].begin();
            uint32_t g = (nrb > first_non_zero + guard) ? (first_non_zero + guard)
                                                        : first_non_zero;
            DEBUG(
                "ROB %u, nrb: %u, least loaded bucket: %u, most loaded: %u, first "
                "candidate: %u, first_non_zero: %u",
                r, nrb, robBuckets[r][0], robBuckets[r][nrb - 1], robBuckets[r][g], first_non_zero);

            double donatedLoad = 0;
            while (((g + donated + (donateAll ? 0 : 1)) < nrb) &&
                   ((rLoads[r] - donatedLoad) > (1.0 + moe) / nr)) {
                DEBUG("ROB %u, donating bucket %u, %.6f (load: %.6f)", r,
                      robBuckets[r][g + donated],
                      bucketLoad(robBuckets[r][g + donated]),
                      bucketLoad(robBuckets[r][g + donated]));
                bucketsToRemap[r].push_back(robBuckets[r][g + donated]);
                donatedLoad += bucketLoad(robBuckets[r][g + donated]);
                donated++;
            }
        } else {
            // Sort buckets from largest to smallest
            std::sort(robBuckets[r].begin(), robBuckets[r].end(), cmpBucketsDescending);

            // Only donate buckets with > 0 load
            uint32_t first_zero =
                std::find_if(
                    robBuckets[r].begin(), robBuckets[r].end(),
                    [&](uint32_t b) -> bool { return bucketLoad(b) == 0; }) -
                robBuckets[r].begin();

            uint32_t g = (nrb < guard) ? nrb-1 : guard;
            DEBUG(
                "ROB %u, nrb: %u, least loaded bucket: %u, most loaded: %u, "
                "first candidate: %u (g=%u)",
                r, nrb, robBuckets[r][nrb - 1], robBuckets[r][0],
                robBuckets[r][g], g);
            double donatedLoad = 0;

            while (((g + donated + (donateAll ? 0 : 1)) < first_zero) &&
                   ((rLoads[r] - donatedLoad) > (1.0 + moe) / nr)) {
                DEBUG("ROB %u, donating bucket %u, %.6f (load: %.6f)", r,
                      robBuckets[r][g + donated],
                      bucketLoad(robBuckets[r][g + donated]),
                      bucketLoad(robBuckets[r][g + donated]));
                bucketsToRemap[r].push_back(robBuckets[r][g + donated]);
                donatedLoad += bucketLoad(robBuckets[r][g + donated]);
                donated++;
            }
        }

        if (donated > 0) {
            assert(rLoads[r] > (1.0 + moe) / nr);
            toShedRobs.push(std::make_pair(rLoads[r], r));
        } else if (rLoads[r] < (1.0 - moe) / nr) {
            toLoadRobs.push_back(std::make_pair(rLoads[r], r));
        }
    }

    // 4. Remap buckets
    while (
        find_if(bucketsToRemap.begin(), bucketsToRemap.end(),
                std::not1(std::mem_fun_ref(&std::vector<uint32_t>::empty))) !=
        bucketsToRemap.end()) {
        if (toLoadRobs.empty()) break;
        assert(!toLoadRobs.empty());
        assert(!toShedRobs.empty());

        // a. Pick a ROB to shed load
        std::pair<double, uint32_t> shedRob = toShedRobs.top();
        toShedRobs.pop();

        // b. Pick a ROB to load from among the neighbors of shedRob (donate
        // load only to neighbors) updateNeighborRobs originally has the same
        // set of RoBs as neighborRoBs. As the update phase proceeds, we may
        // realize that the shedRob does not have any buckets that can be
        // donated to a particular neighbor (because of distance constraints),
        // in which case that neighbor is removed from the updateNeighborRobs.
        auto it = std::partition(
            toLoadRobs.begin(), toLoadRobs.end(),
            [&shedRob, &updateNeighborRobs](std::pair<double, uint32_t> r) {
                return std::find(updateNeighborRobs[shedRob.second].begin(),
                                 updateNeighborRobs[shedRob.second].end(),
                                 r.second) ==
                       updateNeighborRobs[shedRob.second].end();
            });

        auto elemIt = std::min_element(it, toLoadRobs.end());

        if (elemIt != toLoadRobs.end()) {
            std::pair<double, uint32_t> loadRob = *elemIt;
            toLoadRobs.erase(elemIt);
            assert(shedRob.second != loadRob.second);
            assert(!bucketsToRemap[shedRob.second].empty());

            // c. Pick a bucket to donate
            // toLoadRob is a neighbor of the home node of the bucket to donate,
            // and bucket to donate is the least loaded of such buckets.
            std::vector<uint32_t> availableBuckets =
                bucketsToRemap[shedRob.second];
            auto bit = std::stable_partition(
                availableBuckets.begin(), availableBuckets.end(),
                [this, &loadRob](uint32_t b) {
                    return std::find(neighborRobs[homeRob[b]].begin(),
                                     neighborRobs[homeRob[b]].end(),
                                     loadRob.second) ==
                           neighborRobs[homeRob[b]].end();
                });
            // Note that we have to use neighborRobs in this partition, and not
            // updateNeighborRobs.

            if (bit != availableBuckets.end()) {
                uint32_t b = *bit;  // This will be the least loaded bucket s.t
                                    // toLoadRob is among the neighbors of the
                                    // home node of b
                assert(bucketToRob[b] == shedRob.second);
                auto bit_orig =
                    std::find(bucketsToRemap[shedRob.second].begin(),
                              bucketsToRemap[shedRob.second].end(), b);

                // Will this bucket cause toLoadRob to be overloaded?
                // TODO Add different margin of error to this term?
                if (loadRob.first + bucketLoad(b) > (1.0 + moe)/nr) {
                    // Yes, then don't donate, and remove this rob from
                    // update neighbors list.
                    auto nit = std::find(updateNeighborRobs[shedRob.second].begin(),
                                         updateNeighborRobs[shedRob.second].end(),
                                         loadRob.second);
                    assert(nit != updateNeighborRobs[shedRob.second].end());
                    updateNeighborRobs[shedRob.second].erase(nit);
                } else {
                    // It's safe to donate
                    bucketsToRemap[shedRob.second].erase(bit_orig);

                    // Remap the bucket; also modify the provisional load
                    DEBUG("Remapping %d %d->%d (load %.2f->%.2f)", b,
                          shedRob.second, loadRob.second, loadRob.first,
                          loadRob.first + bucketLoad(b));
                    loadRob.first += bucketLoad(b);
                    shedRob.first -= bucketLoad(b);
                    bucketToRob[b] = loadRob.second;
                }
            } else {
                // d. Cannot find a bucket to donate from shedRob --> loadRob
                // Remove loadRob from neighbors of shedRob for this update
                // phase.
                // We will consider shedRob in the next iteration of this loop
                // again.
                auto nit = std::find(updateNeighborRobs[shedRob.second].begin(),
                                     updateNeighborRobs[shedRob.second].end(),
                                     loadRob.second);
                assert(nit != updateNeighborRobs[shedRob.second].end());
                updateNeighborRobs[shedRob.second].erase(nit);
            }

            if (loadRob.first < (1.0 - moe) / nr) toLoadRobs.push_back(loadRob);
            if (!bucketsToRemap[shedRob.second].empty())
                toShedRobs.push(shedRob);
        } else {
            // e. There are no neighbors of toShedRob, that we can donate to.
            // No longer consider toShedRob in the list of RoBs to be rebalanced
            // (do not add it back
            // to toShedRobs for consideration in the remap algorithm).
            // TODO Maybe we want to check at the end of the while loop if there
            // are any RoBs with much
            // higher load than the "target" (avg = 1/nr) and randomly donate
            // buckets.
            assert(it == toLoadRobs.end());
        }

        if (toShedRobs.empty()) break;
        if (toLoadRobs.empty()) break;
    }

    // 5. Profiling info
    std::vector<double> rLoadsVec(rLoads.begin(), rLoads.end());

    std::vector<uint32_t> oldBucketCounts(nr);
    for (uint32_t r = 0; r < nr; r++) oldBucketCounts[r] = robBuckets[r].size();

    std::vector<uint32_t> newBucketCounts(nr, 0);
    for (uint32_t b = 0; b < nb; b++) newBucketCounts[bucketToRob[b]]++;

    DEBUG("Remapping done, loads %s, buckets %s -> %s", Str(rLoadsVec).c_str(),
          Str(oldBucketCounts).c_str(), Str(newBucketCounts).c_str());
}

void ProportionalBucketTaskMapper::reconfigure(const arma::Mat<uint64_t>& diffRobLoads,
                                   const LoadStats& cStats) {
    // 1. Initialize required variables
    uint32_t nr = numROBs;
    uint32_t nb = nr * bucketsPerRob;   // TODO For now do not consider spiller/requeuer cycles

    vector<vector<uint32_t>> updateNeighborRobs = neighborRobs;
    vector<vector<uint32_t>> robBuckets(nr);
    for (uint32_t b = 0; b < nb; b++) robBuckets[bucketToRob[b]].push_back(b);
    arma::Mat<double> rLoads =
        arma::conv_to<arma::Mat<double>>::from(diffRobLoads) /
        arma::accu(diffRobLoads);

    for (uint32_t r = 0; r < nr; r++)
        DEBUG("This window load on ROB %2u: %4lu (%.6f)", r, diffRobLoads[r],
              rLoads[r]);

    // 2. Helper functions
    // This is different from the getBucketLoad() function.
    // It computes the bucket load as a fraction of the total rob load
    // passed into reconfigure(). Importantly, this load only accounts
    // for load associated with the rob that the bucket is mapped to in
    // this phase.
    auto bucketLoad = [&](uint32_t b) {
        uint32_t r = bucketToRob[b];
        return ((float)cStats[r][b] / (float)diffRobLoads[r]) *
               (float)rLoads[r];
    };
    auto cmpBucketsAscending = [&](uint32_t ba, uint32_t bb) {
        return bucketLoad(ba) < bucketLoad(bb);
    };
    auto cmpBucketsDescending = [&](uint32_t ba, uint32_t bb) {
        return bucketLoad(ba) > bucketLoad(bb);
    };

    // 3. For overloaded ROBs, get the buckets to donate
    std::vector<std::vector<uint32_t>> bucketsToRemap(nr);
    std::vector<double> errors(nr);
    std::vector<double> addedLoads(nr, 0.0);
    std::priority_queue<std::pair<double, uint32_t>>
        toShedRobs;  // top returns max load ROB
    std::vector<std::pair<double, uint32_t>> toLoadRobs;

    const float moe = 0.0;     // FIXME
    for (uint32_t r = 0; r < nr; r++) {
        uint32_t nrb = robBuckets[r].size();
        assert(nrb || donateAll);
        if (nrb == 0) {
            if (rLoads[r] < (1.0 - moe) / nr)
                toLoadRobs.push_back(std::make_pair(rLoads[r], r));
            continue;
        }

        uint32_t donated = 0;
        if (donateAscending) {
            // Sort buckets from smallest to largest
            std::sort(robBuckets[r].begin(), robBuckets[r].end(), cmpBucketsAscending);

            // Only donate buckets with > 0 load
            uint32_t first_non_zero =
                std::find_if(
                    robBuckets[r].begin(), robBuckets[r].end(),
                    [&](uint32_t b) -> bool { return bucketLoad(b) > 0; }) -
                robBuckets[r].begin();
            uint32_t g = (nrb > first_non_zero + guard) ? (first_non_zero + guard)
                                                        : first_non_zero;
            DEBUG(
                "ROB %u, nrb: %u, least loaded bucket: %u, most loaded: %u, first "
                "candidate: %u, first_non_zero: %u",
                r, nrb, robBuckets[r][0], robBuckets[r][nrb - 1], robBuckets[r][g], first_non_zero);

            double donatedLoad = 0.0;
            double error = rLoads[r] - (1.0 + moe)/nr;
            errors[r] = error;  // Store the error for this ROB
            // [A] Only donate upto Kp*error
            // Note that error is positive only for overloaded ROBs
            while (((g + donated + (donateAll ? 0 : 1)) < nrb) &&
                   (donatedLoad  < (Kp * error))
                  ) {
                assert(error > 0.0);
                DEBUG("ROB %u, donating bucket %u, %.6f (load: %.6f)", r,
                      robBuckets[r][g + donated],
                      bucketLoad(robBuckets[r][g + donated]),
                      bucketLoad(robBuckets[r][g + donated]));
                bucketsToRemap[r].push_back(robBuckets[r][g + donated]);
                donatedLoad += bucketLoad(robBuckets[r][g + donated]);
                donated++;
            }
        } else {
            // Sort buckets from largest to smallest
            std::sort(robBuckets[r].begin(), robBuckets[r].end(), cmpBucketsDescending);

            // Only donate buckets with > 0 load
            uint32_t first_zero =
                std::find_if(
                    robBuckets[r].begin(), robBuckets[r].end(),
                    [&](uint32_t b) -> bool { return bucketLoad(b) == 0; }) -
                robBuckets[r].begin();

            uint32_t g = (nrb < guard) ? nrb-1 : guard;
            DEBUG(
                "ROB %u, nrb: %u, least loaded bucket: %u, most loaded: %u, "
                "first candidate: %u (g=%u)",
                r, nrb, robBuckets[r][nrb - 1], robBuckets[r][0],
                robBuckets[r][g], g);

            double donatedLoad = 0.0;
            double error = rLoads[r] - (1.0 + moe)/nr;
            errors[r] = error;  // Store the error for this ROB

            while (((g + donated + (donateAll ? 0 : 1)) < first_zero) &&
                   (donatedLoad < (Kp * error))) {
                DEBUG("ROB %u, donating bucket %u, %.6f (load: %.6f)", r,
                      robBuckets[r][g + donated],
                      bucketLoad(robBuckets[r][g + donated]),
                      bucketLoad(robBuckets[r][g + donated]));
                bucketsToRemap[r].push_back(robBuckets[r][g + donated]);
                donatedLoad += bucketLoad(robBuckets[r][g + donated]);
                donated++;
            }
        }

        if (donated > 0) {
            assert(rLoads[r] > (1.0 + moe) / nr);
            toShedRobs.push(std::make_pair(rLoads[r], r));
        } else if (rLoads[r] < (1.0 - moe) / nr) {
            toLoadRobs.push_back(std::make_pair(rLoads[r], r));
        }
    }

    // 4. Remap buckets
    while (
        find_if(bucketsToRemap.begin(), bucketsToRemap.end(),
                std::not1(std::mem_fun_ref(&std::vector<uint32_t>::empty))) !=
        bucketsToRemap.end()) {
        if (toLoadRobs.empty()) break;
        assert(!toLoadRobs.empty());
        assert(!toShedRobs.empty());

        // a. Pick a ROB to shed load
        std::pair<double, uint32_t> shedRob = toShedRobs.top();
        toShedRobs.pop();

        // b. Pick a ROB to load from among the neighbors of shedRob (donate
        // load only to neighbors) updateNeighborRobs originally has the same
        // set of RoBs as neighborRoBs. As the update phase proceeds, we may
        // realize that the shedRob does not have any buckets that can be
        // donated to a particular neighbor (because of distance constraints),
        // in which case that neighbor is removed from the updateNeighborRobs.
        auto it = std::partition(
            toLoadRobs.begin(), toLoadRobs.end(),
            [&shedRob, &updateNeighborRobs](std::pair<double, uint32_t> r) {
                return std::find(updateNeighborRobs[shedRob.second].begin(),
                                 updateNeighborRobs[shedRob.second].end(),
                                 r.second) ==
                       updateNeighborRobs[shedRob.second].end();
            });

        auto elemIt = std::min_element(it, toLoadRobs.end());

        if (elemIt != toLoadRobs.end()) {
            std::pair<double, uint32_t> loadRob = *elemIt;
            toLoadRobs.erase(elemIt);
            assert(shedRob.second != loadRob.second);
            assert(!bucketsToRemap[shedRob.second].empty());

            // c. Pick a bucket to donate
            // toLoadRob is a neighbor of the home node of the bucket to donate,
            // and bucket to donate is the least loaded of such buckets.
            std::vector<uint32_t> availableBuckets =
                bucketsToRemap[shedRob.second];
            auto bit = std::stable_partition(
                availableBuckets.begin(), availableBuckets.end(),
                [this, &loadRob](uint32_t b) {
                    return std::find(neighborRobs[homeRob[b]].begin(),
                                     neighborRobs[homeRob[b]].end(),
                                     loadRob.second) ==
                           neighborRobs[homeRob[b]].end();
                });
            // Note that we have to use neighborRobs in this partition, and not
            // updateNeighborRobs.

            if (bit != availableBuckets.end()) {
                uint32_t b = *bit;  // This will be the least loaded bucket s.t
                                    // toLoadRob is among the neighbors of the
                                    // home node of b
                assert(bucketToRob[b] == shedRob.second);
                auto bit_orig =
                    std::find(bucketsToRemap[shedRob.second].begin(),
                              bucketsToRemap[shedRob.second].end(), b);

                // Will this bucket cause toLoadRob to be overloaded?
                // TODO Add different margin of error to this term?
                if (loadRob.first + bucketLoad(b) > (1.0 + moe)/nr) {
                    // Yes, then don't donate, and remove this rob from
                    // update neighbors list.
                    auto nit = std::find(updateNeighborRobs[shedRob.second].begin(),
                                         updateNeighborRobs[shedRob.second].end(),
                                         loadRob.second);
                    assert(nit != updateNeighborRobs[shedRob.second].end());
                    updateNeighborRobs[shedRob.second].erase(nit);
                } else {
                    // It's safe to donate
                    bucketsToRemap[shedRob.second].erase(bit_orig);

                    // Remap the bucket; also modify the provisional load
                    DEBUG("Remapping %d %d->%d (load %.2f->%.2f)", b,
                          shedRob.second, loadRob.second, loadRob.first,
                          loadRob.first + bucketLoad(b));
                    loadRob.first += bucketLoad(b);
                    shedRob.first -= bucketLoad(b);
                    bucketToRob[b] = loadRob.second;
                    addedLoads[loadRob.second] += bucketLoad(b);
                }
            } else {
                // d. Cannot find a bucket to donate from shedRob --> loadRob
                // Remove loadRob from neighbors of shedRob for this update
                // phase.
                // We will consider shedRob in the next iteration of this loop
                // again.
                auto nit = std::find(updateNeighborRobs[shedRob.second].begin(),
                                     updateNeighborRobs[shedRob.second].end(),
                                     loadRob.second);
                assert(nit != updateNeighborRobs[shedRob.second].end());
                updateNeighborRobs[shedRob.second].erase(nit);
            }

            // [B] If the total added load to this ROB is less than Kp * error,
            // then continue to add load to it.
            if (addedLoads[loadRob.second] < (Kp * errors[loadRob.second]))
                toLoadRobs.push_back(loadRob);
            if (!bucketsToRemap[shedRob.second].empty())
                toShedRobs.push(shedRob);
        } else {
            // e. There are no neighbors of toShedRob, that we can donate to.
            // No longer consider toShedRob in the list of RoBs to be rebalanced
            // (do not add it back
            // to toShedRobs for consideration in the remap algorithm).
            // TODO Maybe we want to check at the end of the while loop if there
            // are any RoBs with much
            // higher load than the "target" (avg = 1/nr) and randomly donate
            // buckets.
            assert(it == toLoadRobs.end());
        }

        if (toShedRobs.empty()) break;
        if (toLoadRobs.empty()) break;
    }

    // 5. Profiling info
    std::vector<double> rLoadsVec(rLoads.begin(), rLoads.end());

    std::vector<uint32_t> oldBucketCounts(nr);
    for (uint32_t r = 0; r < nr; r++) oldBucketCounts[r] = robBuckets[r].size();

    std::vector<uint32_t> newBucketCounts(nr, 0);
    for (uint32_t b = 0; b < nb; b++) newBucketCounts[bucketToRob[b]]++;

    DEBUG("Remapping done, loads %s, buckets %s -> %s", Str(rLoadsVec).c_str(),
          Str(oldBucketCounts).c_str(), Str(newBucketCounts).c_str());
}
