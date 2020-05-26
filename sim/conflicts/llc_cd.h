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

#include "sim/conflicts/conflict_detection.h"
#include "sim/conflicts/sticky_directory.h"

class StickyDirectory;

class LLCCD : public ConflictDetectionTop, public ConflictDetectionBottom {
    StickyDirectory sdReaders_;
    StickyDirectory sdWriters_;

  public:
    LLCCD();

    // Top interface
    uint64_t invalidated(uint32_t, const InvReq&, const InvResp&,
                         uint64_t) override;
    std::bitset<MAX_CACHE_CHILDREN> sharers(Address, bool) const override;
    bool giveExclusive(const MemReq& req) const override;

    // Bottom interface: silences incoming conflict-checked MemReqs
    uint64_t access(const MemReq&, uint64_t cycle) override;
};
