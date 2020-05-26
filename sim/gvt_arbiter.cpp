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

#include "gvt_arbiter.h"

#include "sim/ff_queue.h"
#include "sim/log.h"
#include "sim/sim.h"
#include "sim/watchdog.h"

#undef DEBUG_GVT
#define DEBUG_GVT(args...)  //info(args)

// For performing atomic SET_GVT updates:
static uint64_t setGvtCount = 0;
static std::vector<GVTArbiter*> allArbiters;

GVTArbiter::GVTArbiter(const std::string& name)
    : SimObject(name),
      parentPort(nullptr),
      childIdx(-1U),
      gvt(ZERO_TS),
      state(State::STEADY_STATE),
      curFrameBase(ZERO_TS),
      nextFrameBase(ZERO_TS) {
    allArbiters.push_back(this);
}

GVTArbiter::~GVTArbiter() {
    // We could search allArbiters and remove only this one arbiter,
    // but that is unnecessary for now as arbiters are destructed
    // only at the end of the entire simulation.
    allArbiters.clear();
}

void GVTArbiter::receive(const LVTUpdateMsg& msg, uint64_t cycle) {
    DEBUG_GVT("[%s] cycle %ld epoch %ld [%ld-%ld] lvt %s", name(), cycle,
              msg.epoch, latchedEpoch.num, curEpoch.num,
              msg.toString().c_str());
    assert_msg((msg.epoch == 0) == msg.isSetGvt,
               "LVT epoch 0 is reserved for use by SET_GVT");

    // The LVT must have a tiebreaker set to avoid the GVT skipping over
    // all tasks within a single user-level timestamp.
    assert(msg.lvt.hasTieBreaker());

    // With multiple subnets, we can easily get out-of-order delivery of LVT
    // messages.  If we receive a delayed LVT message that has already been
    // passed over by the GVT, it must be from an even earlier epoch than
    // the one that determined the GVT, so we can just drop it.
    if (msg.lvt < gvt && !msg.isSetGvt) {
        DEBUG_GVT("[%s] Dropping LVT message as it preceeds GVT %s", name(),
                  gvt.toString().c_str());
        return;
    }

    if (msg.epoch > curEpoch.num) {
        // Must open a new epoch, can we latch this one?
        if (!latchedEpoch.left || curEpoch.prio() > latchedEpoch.prio()) {
            if (latchedEpoch.left)
                DEBUG_GVT("[%s] discarding latched epoch %ld", name(),
                          latchedEpoch.num);
            latchedEpoch = curEpoch;
        } else {
            DEBUG_GVT("[%s] discarding current epoch %ld", name(),
                      curEpoch.num);
        }

        DEBUG_GVT("[%s] Opening new epoch %ld", name(), msg.epoch);
        uint32_t numLvtMessages = childPorts.size();
        if (!parentPort)  // root arbiter
            numLvtMessages++;  // Extra LVT message from FfTaskQueue
        curEpoch.reset();
        curEpoch.num = msg.epoch;
        curEpoch.left = numLvtMessages;
    }

    auto updateLambda = [&](Epoch& epoch) -> bool {
        const bool last = epoch.recordUpdate(msg);
        assert(epoch.minLvt >= gvt);
        if (last) {
            if (parentPort) {  //  non-root arbiter
                // If LVT == GVT, then there's no point forwarding the LVT:
                // we must have already sent this LVT value before in a
                // previous epoch and it's already been broadcast as the GVT,
                // and the current epoch will not result in any GVT update.
                // Dropping these unneeded LVT updates is a minor optimization,
                // but it might enable stronger assertions elsewhere.
                if ((epoch.minLvt == gvt) && (epoch.zoom == ZoomType::NONE) &&
                    (state == State::STEADY_STATE)) return last;
                DEBUG_GVT("[%s] epoch %ld forwarding minLvt %s", name(),
                          epoch.num, epoch.minLvt.toString().c_str());
                // TODO Use .all() after upgrading boost
                AckType ackType = (~epoch.zoomAcks).none() ? AckType::ZOOM
                                : (~epoch.ssAcks).none() ? AckType::SS
                                : AckType::NONE;
                LVTUpdateMsg msg = {
                    epoch.minLvt, epoch.zoomRequesterTs, epoch.num,
                    false, childIdx, epoch.zoom, ackType};
                parentPort->send(msg, cycle + 1);
            } else {  // root arbiter
                DEBUG_GVT(
                    "[%s] epoch %ld updating gvt %s -> %s "
                    "(zoomType: %s, zoomRequesterTs: %s)",
                    name(), epoch.num, gvt.toString().c_str(),
                    epoch.minLvt.toString().c_str(),
                    ((epoch.zoom == ZoomType::NONE)
                         ? "NONE"
                         : (epoch.zoom == ZoomType::IN) ? "IN" : "OUT"),
                    epoch.zoomRequesterTs.toString().c_str());

                bool gvtWasUpdated = false;
                if (epoch.minLvt > gvt) {
                    advanceGvt(epoch.minLvt);
                    UpdateWatchdogGvt(gvt);
                    gvtWasUpdated = true;
                }

                bool cancelPrevZoom = false;
                bool mustSendZoomMessage = transition(epoch, cancelPrevZoom);

                if (gvtWasUpdated || mustSendZoomMessage) {
                    assert(gvt == epoch.minLvt);

                    if (msg.isSetGvt) {
                        // Notify all arbiters about SET_GVT atomically,
                        // making it easier to resolve races.
                        setGvtCount++;
                        assert(!allArbiters.empty());
                        for (GVTArbiter *arbiter : allArbiters)
                            if (arbiter != this) arbiter->setGvt(gvt, cycle);
                    }

                    GVTUpdateMsg msg = {gvt, nextFrameBase, epoch.zoom,
                                        cancelPrevZoom, msg.isSetGvt, setGvtCount};
#ifdef ATOMIC_GVT_UPDATES
                    updateAllRobGvts(msg, cycle);
#endif
                    for (auto port : childPorts) port->send(msg, cycle + 1);
                }
            }
            epoch.reset();
        }
        return last;
    };

    if (msg.isSetGvt) {
        DEBUG_GVT("[%s] cycle %ld setGvt %s", name(), cycle,
                  msg.lvt.toString().c_str());

        // Nullify our epochs to avoid spurious updates
        latchedEpoch.reset();
        curEpoch.reset();

        if (parentPort) {
            DEBUG_GVT("[%s] Forwarding SET_GVT request");
            parentPort->send(msg, cycle + 1);
        } else {
            // Cannibalize latchedEpoch to send GVT updates
            gvt = ZERO_TS;  // ensure we update it
            assert(latchedEpoch.minLvt == INFINITY_TS);
            latchedEpoch.left = 1;
            bool last = updateLambda(latchedEpoch);
            assert(last);
        }
    } else if (msg.epoch == curEpoch.num) {
        if (updateLambda(curEpoch)) {
            // Nullify latched epoch, we have now replied to a later one.
            // Unless this is a non-root arbiter and latchedEpoch is still
            // valid and has higher prio; in that case promote it to curEpoch.
            // This promotion is unnecessary for the root arbiter,
            // which can simply drop old epochs.
            if (parentPort && latchedEpoch.left &&
                latchedEpoch.prio() > curEpoch.prio()) {
                curEpoch = latchedEpoch;
            }
            latchedEpoch.reset();
        }
    } else if (msg.epoch == latchedEpoch.num) {
        updateLambda(latchedEpoch);
    } else {
        DEBUG_GVT("[%s] Dropping LVT message from discarded epoch", name());
    }
    assert(latchedEpoch.num <= curEpoch.num);
    assert_msg(
        gvt <= latchedEpoch.minLvt && gvt <= curEpoch.minLvt,
        "After %s received LVT %s epoch %ld,\n"
        "        GVT is %s and we have\n"
        "        current epoch %s, and\n"
        "        latched epoch %s.",
        name(), msg.lvt.toString().c_str(), msg.epoch, gvt.toString().c_str(),
        curEpoch.toString().c_str(), latchedEpoch.toString().c_str());
}


void GVTArbiter::receive(const GVTUpdateMsg& msg, uint64_t cycle) {
    assert(parentPort);  // root can't receive these
    DEBUG_GVT("[%s] Cycle %lu: received GVT message %s (%lu setGvts).", name(),
              cycle, msg.gvt.toString().c_str(), msg.setGvtCount);

    // With SET_GVT, gvt no longer monotonically increases, so we must
    // use some other information to detect out-of-order GVT message arrival
    // and avoid incorrectly updating our copy of the GVT.
    if (msg.setGvtCount < setGvtCount) {
        DEBUG_GVT("[%s] Dropping old GVT %s"
                  " from before the most recent SET_GVT (%lu)", name(),
                  msg.gvt.toString().c_str(), setGvtCount);
        return;
    }

    // With multiple subnets, GVT messages can be delivered out of order.
    // No need to forward lower GVT if we've already forwarded a higher one.
    if (gvt > msg.gvt) {
        DEBUG_GVT("[%s] Dropping old GVT %s.  (current gvt is %s)", name(),
                  msg.gvt.toString().c_str(), gvt.toString().c_str());
        return;
    }

    if (gvt < msg.gvt) {
        advanceGvt(msg.gvt);
    } else {
        // [ssub] Redundant messages carrying the same GVT value are possible
        // with frame. Specifically, after a zoom operation is complete (i.e.
        // all ROBs have responded with ACKs on completing the zoom to the root
        // arbiter) or canceled (i.e., the requesting task aborted), the root
        // arbiter de-asserts the zoom signal, while the gvt can remain the
        // same. Similarly, while triggering zoom operations, the gvt can remain
        // the same, while the message asks ROBs to start a zoom operation.
        // TODO(victory): The above seems to indicate that we should not be
        // using GVTUpdateMsgs to announce a zoom completing/canceling: there
        // should be new message types that the arbiters can use to broadcast
        // this information which has nothing to do with GVT forward progress.
        // On the other hand, there might be a good reason to couple the triggering
        // of zooms with the GVT protocol, since it avoids race conditions
        // involving tasks that need to commit before the zoom can proceed.
        assert_msg(msg.isSetGvt || ossinfo->maxFrameDepth != UINT32_MAX,
                   "%s received non-SetGvt GVT %s from parent,\n"
                   "        prev GVT was %s",
                   name(), msg.gvt.toString().c_str(), gvt.toString().c_str());
        assert(gvt == msg.gvt);
    }
    DEBUG_GVT("[%s] cycle %ld forwarding gvt %s", name(), cycle,
              gvt.toString().c_str());
    for (auto port : childPorts) port->send(msg, cycle + 1);
}

bool GVTArbiter::transition(Epoch& epoch, bool& cancelPrevZoom) {
    TimeStamp requestedFrameBase = INFINITY_TS;
    switch (epoch.zoom) {
    case ZoomType::IN:
        requestedFrameBase =
            epoch.zoomRequesterTs.getUpLevels(ossinfo->maxFrameDepth - 2)
                .domainMin();
        break;
    case ZoomType::OUT:
        requestedFrameBase = epoch.zoomRequesterTs.getUpLevels(1).domainMin();
        break;
    case ZoomType::NONE:
        break;
    }
    // [ssub] The transition from WAIT_SS to STEADY_STATE
    // does not lead to updating the GVT. Hence, mustSendZoomMessage
    // does not map to newState != oldState.
    // The WAIT_SS state is required because of the epoch-method
    // of sending LVT updates. The GVT arbiter needs to know that
    // all ROBs have queiesced to the steady state after a zoom
    // operation. Otherwise, we might get confused with ACKs for
    // future zoom operations.
    // Is a simpler state machine possible?
    bool mustSendZoomMessage = false;
    switch (state) {
        case State::STEADY_STATE: {
            if (epoch.zoom != ZoomType::NONE) {
                // If GVT has reached triggerTs, it's safe to
                // initiate frame zoom IN/OUT as appropriate.
                TimeStamp triggerTs = epoch.zoom == ZoomType::IN
                                          ? requestedFrameBase
                                          : epoch.zoomRequesterTs;
                if (gvt >= triggerTs) {
                    if (epoch.zoom == ZoomType::IN) {
                        state = State::IN;
                        assert(requestedFrameBase.getUpLevels(1).domainMin() ==
                               curFrameBase);
                    } else {
                        state = State::OUT;
                        assert(curFrameBase.getUpLevels(1).domainMin() ==
                               requestedFrameBase);
                    }
                    nextFrameBase = requestedFrameBase;
                    mustSendZoomMessage = true;
                    DEBUG_GVT(
                        "[%s] Transitioning from SS to %s as gvt > trigger %s,"
                        " nextFrameBase: %s",
                        name(), state == State::IN ? "IN" : "OUT",
                        triggerTs.toString().c_str(),
                        nextFrameBase.toString().c_str());
                } else {
                    DEBUG_GVT(
                        "[%s] Not yet acting on zoom request, "
                        "as gvt has not yet reached trigger %s",
                        name(), triggerTs.toString().c_str());
                    epoch.zoom = ZoomType::NONE;
                }
            }
        } break;

        case State::OUT:
        case State::IN: {
            if (requestedFrameBase == nextFrameBase) {
                // Some tile(s) continue requesting the current zoom.
                // Note that some previous requests for this zoom may have
                // been de-asserted, but we continue with this zoom since
                // somebody is still requesting it.
                assert((epoch.zoom == ZoomType::IN && state == State::IN) ||
                       (epoch.zoom == ZoomType::OUT && state == State::OUT));
                if ((~epoch.zoomAcks).none()) {
                    assert(epoch.zoomAcks.count() == childPorts.size());
                    mustSendZoomMessage = true;
                    state = State::WAIT_SS;
                    epoch.zoom = ZoomType::NONE;
                    curFrameBase = nextFrameBase;
                    DEBUG_GVT(
                        "[%s] epoch: %ld Received all acks for ZOOM, "
                        "transition to WAIT_SS (%lu, %lu)",
                        name(), epoch.num, childPorts.size(),
                        epoch.zoomAcks.count());
                    FfTaskQueueNotifyZoomIn(curFrameBase.domainDepth());
                }
            } else {
                // This is possible only if the original zoom request
                // was de-asserted (say because the task that initiated
                // the zoom was aborted). We can have: a. No ZOOM
                // request in the system (i.e. epoch.zoomRequesterTs ==
                // INFINITY_TS) b. A new ZOOM request.
                // We will hold off on initiating any new zooms for now,
                // first making sure all ROBs have transitioned to SS.
                cancelPrevZoom = true;
                mustSendZoomMessage = true;
                state = State::WAIT_SS;
                epoch.zoom = ZoomType::NONE;
                nextFrameBase = curFrameBase;
                DEBUG_GVT(
                    "[%s] epoch: %ld ZOOM request de-asserted, "
                    "transition to WAIT_SS, frame base: %s",
                    name(), epoch.num, curFrameBase.toString().c_str());
            }
        } break;

        case State::WAIT_SS: {
            // We don't set transitionOccurred to true for this
            // transition alone. Because this state is merely a
            // notification for the GVT arbiter that all ROBs
            // have transitioned to steady state.
            // TODO We don't have to wait another cycle technically
            // to move to IN / OUT if required / possible.
            // TODO Use .all() after upgrading boost
            if ((~epoch.ssAcks).none()) {
                state = State::STEADY_STATE;
                DEBUG_GVT("[%s] Received all acks for SS, transition to SS",
                          name());
            }
            epoch.zoom = ZoomType::NONE;
        } break;

        default:
            panic("Invalid state!");
    }
    return mustSendZoomMessage;
}

void GVTArbiter::advanceGvt(const TimeStamp& newGvt) {
    DEBUG_GVT("[%s] GVT changed from %s to %s", name(),
              gvt.toString().c_str(), newGvt.toString().c_str());
    assert(newGvt > gvt);
    gvt = newGvt;

    // If any LVT has been passed over by the GVT, then the GVT must have
    // come from a later epoch, so there's no use in keeping the earlier
    // LVT epoch around to potentially forward a straggling old LVT message
    // from the old epoch.
    if (latchedEpoch.minLvt < gvt) latchedEpoch.reset();
    if (curEpoch.minLvt < gvt) curEpoch.reset();
}

void GVTArbiter::setGvt(const TimeStamp& newGvt, uint64_t cycle) {
    DEBUG_GVT("[%s] Set GVT from %s to %s", name(),
              gvt.toString().c_str(), newGvt.toString().c_str());
    gvt = newGvt;

    latchedEpoch.reset();
    curEpoch.reset();
}

std::ostream& GVTArbiter::Epoch::operator<<(std::ostream& os) const {
    os  << "<epoch"
        << " num=" << num
        << " minLvt=" << minLvt
        << " left=" << left
        << ">";
    return os;
}
