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
#include <limits>

typedef uint32_t ThreadID;
typedef uint64_t Address;
typedef uint32_t TileId;

static const uint32_t MAX32_T     = ((uint32_t)-1);
static const uint64_t MAX64_T     = ((uint64_t)-1);
static const double   MAX_DOUBLE  = (std::numeric_limits<double>::max()-1);
static const ThreadID INVALID_TID = ((ThreadID)-1);
static const uint32_t INVALID_CID = ((uint32_t)-1);
static const uint32_t INVALID_DEPTH = ((uint32_t)-1);

static const uint64_t INDEFINITE  = ((uint64_t)-1);
static const uint64_t INVALID_INT = ((uint64_t)-1);

#define __MAX_TS (~((uint64_t)0))
