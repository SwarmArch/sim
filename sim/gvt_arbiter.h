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

#include <vector>
#include <boost/dynamic_bitset.hpp>
#include "sim/assert.h"
#include "sim/object.h"
#include "sim/port.h"
#include "sim/timestamp.h"
#include "sim/match.h"
#ifndef SWARM_SIM_NO_LOG
#include "sim/log.h"
#undef DEBUG_GVT
#define DEBUG_GVT(args...)  //info(args)
#else
#define DEBUG_GVT(args...)
#endif


// Uncomment this to have all the GVT updates happen atomically. Replies are
// also sent, but they are functionally irrelevant
#define ATOMIC_GVT_UPDATES

enum class ZoomType { NONE, IN, OUT };
enum class AckType { NONE, SS, ZOOM };

struct LVTUpdateMsg {
    TimeStamp lvt;
    TimeStamp zoomRequesterTs;

    uint64_t epoch;  // needed for the distributed-arbiter epoch latching scheme
    bool isSetGvt;
    uint32_t nodeIdx;

    ZoomType zoom;
    AckType ack;

    inline uint32_t bytes() const {
        if (zoom != ZoomType::NONE)
            return 40;
        else return 24;
    }

    // Debug
    std::string toString() const {
        std::stringstream ss;
        ss << "["
           << " rob=" << nodeIdx << " lvt=" << lvt << " zoom="
           << ((zoom == ZoomType::NONE) ? "NONE"
                                        : (zoom == ZoomType::IN) ? "IN" : "OUT")
           << " zoomRequesterTs=" << zoomRequesterTs << " ack="
           << ((ack == AckType::SS) ? "SS" : (ack == AckType::ZOOM) ? "ZOOM"
                                                                    : "NONE")
           << "]";
        return ss.str();
    }
};

struct GVTUpdateMsg {
    TimeStamp gvt;
    TimeStamp frameBase;
    ZoomType zoom;
    bool cancelPrevZoom;
    bool isSetGvt;  // For debug purposes
    uint64_t setGvtCount;  // to detect and discard late-arriving messages from
                           // before a SET_GVT being delivered after the SET_GVT

    inline uint32_t bytes() const {
        if (zoom != ZoomType::NONE) return 32;
        else return 16;
    }
};

#ifdef ATOMIC_GVT_UPDATES
extern void updateAllRobGvts(const GVTUpdateMsg& msg,
                             uint64_t cycle);  // see init.cpp
#endif

typedef Port<LVTUpdateMsg> LVTUpdatePort;
typedef Port<GVTUpdateMsg> GVTUpdatePort;

/* Implements the GVT update protocol with one-sided, non-atomic ports. This
 * relies on all ROBs reliably sending messages at the same cycle, starting a
 * new epoch. Messages may arrive in non-deterministic order, and multiple
 * epochs may overlap (eg message i+n from one ROB may arrive before message i
 * from another for arbitrary n).
 *
 * These arbiters can be used flat or connected in a hierarchy. Ideally, the
 * hierarchy should match the network topology to minimize traffic. When used
 * as a hierarchy, non-root arbiters (those with a parent) reduce and forward
 * LVT updates
 *
 * To be resilient to skew in message arrivals, we tag each LVT update with the
 * epoch they represent. Then, each arbiter keeps track of the total number of
 * LVT updates with the highest of all epochs. In case multiple epochs overlap,
 * to avoid never responding to anything because we only keep track of the
 * newest epoch, we latch one additional epoch and keep track of it until we
 * receive all LVT updates with that epoch.
 *
 * Epochs are latched in a prioritized way. An epoch's priority is the number
 * of trailing zeros, i.e., the number of power-of-2 numbers it is a multiple
 * of (e.g., prio(2) = 1, prio(8) = 3, etc). When starting a new epoch, we
 * discard the lowest epoch from current and latched. This way, intermediate
 * arbiters in the hierarchy all eventually feed a high-priority LVT update
 * to the root. This scheme allows arbitrary skew with bounded storage (i.e.,
 * we don't need to store an unbounded number of per-epoch LVTs).
 */
class GVTArbiter : public SimObject,
                   public LVTUpdatePort::Receiver,
                   public GVTUpdatePort::Receiver {
  private:
    enum class State { STEADY_STATE, IN, OUT, WAIT_SS };

    Port<LVTUpdateMsg>* parentPort;
    std::vector<Port<GVTUpdateMsg>*> childPorts;

    //FIXME(victory): This is needed purely to identify this arbiter as the
    // sender of zooming-related ACKs in LVT updates to the parent arbiter.  It
    // should be possible to get rid of this and all the associated mess in
    // commit 174c86ebe431416f2f9bb1371974249515be08fe once we decouple ACKs
    // from LVT updates.  See comment on zoomAcks and ssAcks within Epoch below.
    uint32_t childIdx;

  public: // For unit testing
    struct Epoch {
        uint64_t num;
        TimeStamp minLvt;
        uint32_t left;

        // Frame management
        ZoomType zoom;
        TimeStamp zoomRequesterTs;

        // Because ACKs for zooming are carried on LVT updates, and because the
        // distributed-snapshot GVT protocol drops LVT updates, the ACKs are
        // repeatedly sent by children in the LVT heirarchy, so merely counting
        // ACKs is insufficient: ACKs must report who they are coming from and
        // we need these bitsets to track which acks have been received.
        //FIXME(victory): This is plainly adding complexity both to the
        // hypothetical hardware implementation and adds non-negligible
        // software complexity to the simulator.  We should separate ACKs into
        // their own message types, instead of sending them in LVT updates.
        boost::dynamic_bitset<> zoomAcks;
        boost::dynamic_bitset<> ssAcks;

        Epoch()
            : num(0),
              minLvt(INFINITY_TS),
              left(0),
              zoom(ZoomType::NONE),
              zoomRequesterTs(INFINITY_TS) {}

        Epoch(uint64_t n, uint32_t l)
            : num(n),
              minLvt(INFINITY_TS),
              left(l),
              zoom(ZoomType::NONE),
              zoomRequesterTs(INFINITY_TS) {}

        bool recordUpdate(const LVTUpdateMsg& msg) {
            assert(left);
            if (msg.lvt < minLvt) minLvt = msg.lvt;

            if ((msg.zoom != ZoomType::NONE) &&
                (msg.zoomRequesterTs < zoomRequesterTs)) {
                DEBUG_GVT(
                    "[%s] Received zoom %s, and updating zoomRequesterTs to %s",
                    "gvtArbiter", msg.zoom == ZoomType::IN ? "IN" : "OUT",
                    msg.zoomRequesterTs.toString().c_str());
                zoom = msg.zoom;
                zoomRequesterTs = msg.zoomRequesterTs;
            }
            switch (msg.ack) {
                case AckType::ZOOM: zoomAcks.set(msg.nodeIdx, true); break;
                case AckType::SS: ssAcks.set(msg.nodeIdx, true); break;
                case AckType::NONE: break;
                default: panic("Invalid ackType");
            }

            left--;
            DEBUG_GVT("[%s] Still waiting for %u messages left in epoch %lu", "gvtArbiter", left, num);
            return left == 0;
        }

        // Epoch prio == log2(largest power-of-2 that divides num)
        uint32_t prio() const {
            if (num == 0) return 64;
            return __builtin_ctz(num);
        }

        //FIXME(victory): Keeping the allocations for the bitsets in zoomAcks
        // and ssAcks is the only reason this method exists.  Otherwise, we
        // could much more cleanly revert to using the Epoch() constructor,
        // as in commit 59c4c08a9b4f4a0042fdf70dad59f8276fd10503.  This should
        // become possible if we get rid of zoomAcks and ssAcks (see comment
        // on their declaration above).
        void reset() {
            num = 0;
            left = 0;
            minLvt = INFINITY_TS;
            zoom = ZoomType::NONE;
            zoomRequesterTs = INFINITY_TS;
            zoomAcks.reset();
            ssAcks.reset();
        }

        std::ostream& operator<<(std::ostream& os) const;
        std::string toString() const {
            std::ostringstream buffer;
            this->operator<<(buffer);
            return buffer.str();
        }
    };

  private:
    TimeStamp gvt;

    Epoch latchedEpoch;
    Epoch curEpoch;

    State state;
    TimeStamp curFrameBase;
    TimeStamp nextFrameBase;

    void advanceGvt(const TimeStamp& newGvt);

  public:
    GVTArbiter(const std::string& name);
    ~GVTArbiter();
    GVTArbiter(const GVTArbiter&) = delete;
    GVTArbiter& operator=(const GVTArbiter&) = delete;

    void setChildPorts(const std::vector<GVTUpdatePort*>& ports) {
        assert(childPorts.empty());
        childPorts = ports;
        assert(!childPorts.empty());

        curEpoch.zoomAcks.resize(childPorts.size(), false);
        curEpoch.ssAcks.resize(childPorts.size(), false);
        latchedEpoch.zoomAcks.resize(childPorts.size(), false);
        latchedEpoch.ssAcks.resize(childPorts.size(), false);
    }

    void setParentPort(LVTUpdatePort* port) {
        assert(!parentPort);
        parentPort = port;
        assert(parentPort);
    }

    void printConnections() {
        DEBUG_GVT("[%s] parentPort: %s, childIdx: %u, childPortsSize: %lu",
                  name(), parentPort ? "yes" : "no", childIdx,
                  childPorts.size());
    }

    //FIXME(victory): Remove this interface. See comments on childIdx above.
    void setChildIdx(uint32_t idx) {
        assert(childIdx == -1U);
        childIdx = idx;
        assert(childIdx != -1U);
    }

    void receive(const LVTUpdateMsg& msg, uint64_t cycle) override;
    void receive(const GVTUpdateMsg& msg, uint64_t cycle) override;

  private:
    bool transition(Epoch& epoch, bool& cancelPrevZoom);

    void setGvt(const TimeStamp& newGvt, uint64_t cycle);
};

#undef DEBUG_GVT
