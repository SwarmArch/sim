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

#pragma once

/* Padding macros to remove false sharing */

// Line size, in chars (bytes). We could make it configurable through a define
#define CACHE_LINE_BYTES (64)

#define _PAD_CONCAT(x, y) x ## y
#define PAD_CONCAT(x, y) _PAD_CONCAT(x, y)

// One-line pad
// Assuming classes are defined over one file, this should generate unique names
#define PAD() unsigned char PAD_CONCAT(pad_line, __LINE__)[CACHE_LINE_BYTES]

// Pad remainder to line size, use as e.g. PAD(sizeof(uint32)) will produce 60B of padding
#define PAD_SZ(sz) unsigned char PAD_CONCAT(pad_sz_line, __LINE__)[CACHE_LINE_BYTES - ((sz) % CACHE_LINE_BYTES)]

#define ATTR_LINE_ALIGNED __attribute__((aligned CACHE_LINE_BYTES ))
