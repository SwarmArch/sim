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

#include <sstream>
#include <string>
#include <vector>

#include "sim/assert.h"
#include "sim/canary.h"

#define DEBUG_TSARRAY(args...) //info(args)

class TSArray {
  private:
    const std::string name;
    const uint32_t groupSize;

    // Stores the last-writer timestamp for each group of consecutive lineIds
    std::vector<CanaryTS> groupTS;

    // Stores a single bit per lineId denoting whether its timestamp is zero
    // If the timestamp is zero, the corresponding groupTS is not used.
    std::vector<bool> zeroTS;

  public:
    // Arrays are rounded to next groupSize multiple to keep logic simple
    TSArray(const std::string& _name, size_t _size, uint64_t _groupSize)
        : name(_name),
          groupSize(_groupSize),
          groupTS((_size / _groupSize) + 1, INFINITY_TS),
          zeroTS(groupTS.size() * groupSize, true) {}

    void clearTS(uint32_t lineId) {
        assert(lineId < zeroTS.size());
        zeroTS[lineId] = true;
    }

    void adjustOnZoomIn(const TimeStamp& frameMin, const TimeStamp& frameMax) {
        for (uint32_t groupId = 0; groupId < groupTS.size(); ++groupId) {
            groupTS[groupId].adjustOnZoomIn(frameMin, frameMax);
            if (groupTS[groupId] == ZERO_TS) {
                // Set the whole zeroTS bitvector, invalidating groupTS
                uint32_t groupLead = groupId * groupSize;
                for (auto l = groupLead; l < groupLead + groupSize; l++)
                    zeroTS[l] = true;
            }
        }
    }

    void adjustOnZoomOut(uint64_t maxDepth) {
        for (uint32_t groupId = 0; groupId < groupTS.size(); ++groupId)
            groupTS[groupId].adjustOnZoomOut(maxDepth);
    }

    void setTS(uint32_t lineId, const TimeStamp& ts, const TimeStamp& gvt) {
        uint32_t groupId = lineId / groupSize;
        DEBUG_TSARRAY("[%s] setTS lineId %u ts %s gvt %s | PRE group %u = %s",
                      name.c_str(), lineId, ts.toString().c_str(),
                      gvt.toString().c_str(), groupId,
                      groupStr(groupId).c_str());
        uint32_t groupLead = groupId * groupSize;
        if (groupTS[groupId] < gvt) {
            // Set the whole zeroTS bitvector, invalidating groupTS
            for (auto l = groupLead; l < groupLead + groupSize; l++)
                zeroTS[l] = true;
            // Since old groupTS is < gvt, set the new one
            assert(gvt <= ts);
            groupTS[groupId] = ts;
        } else {
            clearTS(lineId);  // take our line out of allZeros computation
            bool allZeros = true;
            for (auto l = groupLead; l < groupLead + groupSize; l++)
                allZeros &= zeroTS[l];
            groupTS[groupId] = allZeros ? ts
                             : std::max(ts, static_cast<const TimeStamp&>(groupTS[groupId]));
        }
        assert(ts != ZERO_TS);
        zeroTS[lineId] = false;
        DEBUG_TSARRAY("[%s] setTS POST group %u = %s", name.c_str(), groupId,
                      groupStr(groupId).c_str());
    }

    inline bool isZeroTS(uint32_t lineId) const {
        assert(lineId < zeroTS.size());
        return zeroTS[lineId];
    }

    const TimeStamp& getTS(uint32_t lineId) const {
        if (isZeroTS(lineId)) return ZERO_TS;
        else return groupTS[lineId / groupSize];
    }

    inline uint32_t getGroupSize() const { return groupSize; }

  private:
    // For debug purposes only
    inline std::string groupStr(uint32_t groupId) const {
        std::stringstream ss;
        ss << "[";
        for (auto l = groupId * groupSize; l < (groupId + 1) * groupSize; l++)
            ss << (uint32_t) zeroTS[l];
        ss << "]  " << groupTS[groupId].toString();
        return ss.str();
    }
};
