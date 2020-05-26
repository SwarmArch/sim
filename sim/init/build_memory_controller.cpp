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

#include <iostream>
#include <list>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include "sim/init/builders.h"
#include "sim/log.h"
#include "sim/memory/mem_ctrls.h"

using std::string;

MemCtrl* BuildMemoryController(const Config& config, uint32_t lineSize, uint32_t frequency, string& name) {
    string type = config.get<const char*>("sys.mem.type", "Simple");
    uint32_t latency = (type == "DDR")? -1 : config.get<uint32_t>("sys.mem.latency", 100);

    MemCtrl* mem = nullptr;
    if (type == "Simple") {
        mem = new SimpleMemory(latency, name);
    } else if (type == "LimitedSimple") {
        // The following params are for LimitedSimple only
        // NOTE: Frequency (in MHz) -- note this is a sys parameter (not sys.mem). There is an implicit assumption of having
        // a single CCT across the system, and we are dealing with latencies in *core* clock cycles

        // Peak bandwidth (in MB/s)
        uint32_t bandwidth = config.get<uint32_t>("sys.mem.bandwidth", 6400);

        mem = new LimitedSimpleMemory(latency, frequency, bandwidth, name);

    } else if (type == "MD1") {
        // The following params are for MD1 only
        // NOTE: Frequency (in MHz) -- note this is a sys parameter (not sys.mem). There is an implicit assumption of having
        // a single CCT across the system, and we are dealing with latencies in *core* clock cycles

        // Peak bandwidth (in MB/s)
        uint32_t bandwidth = config.get<uint32_t>("sys.mem.bandwidth", 6400);

        mem = new MD1Memory(lineSize, frequency, bandwidth, latency, name);
    } else {
        panic("Invalid memory controller type %s", type.c_str());
    }
    return mem;
}
