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

#ifndef CONSTANTS_H_
#define CONSTANTS_H_

/* Simulator constants/limits go here, defined by macros */

// PIN 2.9 (rev39599) can't do more than 2048 threads...
#define MAX_THREADS (2048)

// How many children caches can each cache track? Note each bank is a separate child. This impacts sharer bit-vector sizes.
#define MAX_CACHE_CHILDREN (256)
//#define MAX_CACHE_CHILDREN (1024)

// Complex multiprocess runs need multiple clocks, and multiple port domains
#define MAX_CLOCK_DOMAINS (64)
#define MAX_PORT_DOMAINS (64)

//Maximum IPC of any implemented core. This is used for adaptive events and will not fail silently if you define new, faster processors.
//If you use it, make sure it does not fail silently if violated.
#define MAX_IPC (4)

#endif  // CONSTANTS_H_
