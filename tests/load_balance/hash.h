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

#ifndef HASH_H_
#define HASH_H_

#include <stdint.h>

class HashFamily  {
    public:
        HashFamily() {}
        virtual ~HashFamily() {}

        virtual uint64_t hash(uint32_t id, uint64_t val) const = 0;
};

class H3HashFamily : public HashFamily {
    private:
        const uint32_t numFuncs;
        uint32_t resShift;
        uint64_t* hMatrix;
    public:
        H3HashFamily(uint32_t numFunctions, uint32_t outputBits, uint64_t randSeed = 123132127);
        ~H3HashFamily() override;
        uint64_t hash(uint32_t id, uint64_t val) const override;
};

/* Used when we don't want hashing, just return the value */
class IdHashFamily : public HashFamily {
    public:
        inline uint64_t hash(uint32_t id, uint64_t val) const override {return val;}
};

#endif  // HASH_H_
