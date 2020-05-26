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

#include "sim/assert.h"

#include "sim/conflicts/llc_cd.h"

LLCCD::LLCCD() : sdReaders_("llc-sd-readers"), sdWriters_("llc-sd-writers") {}

uint64_t LLCCD::access(const MemReq& req, uint64_t cycle) {
    if (IsPut(req.type)) {
        if (req.is(MemReq::TRACKREAD)) {
            sdReaders_.add(req.lineAddr, req.childId);
        } else {
            sdReaders_.remove(req.lineAddr, req.childId);
        }
        if (req.is(MemReq::TRACKWRITE)) {
            sdWriters_.add(req.lineAddr, req.childId);
        } else {
            sdWriters_.remove(req.lineAddr, req.childId);
        }
    }
    return cycle;
}

uint64_t LLCCD::invalidated(uint32_t childId, const InvReq& req,
                            const InvResp& resp, uint64_t cycle) {
    if (resp.is(InvResp::TRACKREAD)) {
        sdReaders_.add(req.lineAddr, childId);
    } else {
        sdReaders_.remove(req.lineAddr, childId);
    }
    if (resp.is(InvResp::TRACKWRITE)) {
        sdWriters_.add(req.lineAddr, childId);
    } else {
        sdWriters_.remove(req.lineAddr, childId);
    }
    return cycle;
}

std::bitset<MAX_CACHE_CHILDREN> LLCCD::sharers(Address lineAddr, bool checkReaders) const {
    auto s = sdWriters_.getSharers(lineAddr);
    if (checkReaders) s |= sdReaders_.getSharers(lineAddr);
    return s;
}

bool LLCCD::giveExclusive(const MemReq& req) const {
    auto stickySharers = sharers(req.lineAddr, true);
    stickySharers[req.childId] = 0;
    return stickySharers.none();
}
