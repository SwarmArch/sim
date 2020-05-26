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

#include <fstream>
#include <iostream>
#include "sim/assert.h"
#include "sim/log.h"
#include "sim/stats/stats.h"

using std::endl;

class TextBackendImpl {
    private:
        const char* filename;
        AggregateStat* rootStat;

        void dumpStat(Stat* s, uint32_t level, std::ofstream* out) {
            for (uint32_t i = 0; i < level; i++) *out << " ";
            *out << s->name() << ": ";
            s->update();
            if (AggregateStat* as = dynamic_cast<AggregateStat*>(s)) {
                *out << "# " << as->desc() << endl;
                for (uint32_t i = 0; i < as->size(); i++) {
                    dumpStat(as->get(i), level+1, out);
                }
            } else if (ScalarStat* ss = dynamic_cast<ScalarStat*>(s)) {
                *out << ss->get() << " # " << ss->desc() << endl;
            } else if (VectorStat* vs = dynamic_cast<VectorStat*>(s)) {
                *out << "# " << vs->desc() << endl;
                for (uint32_t i = 0; i < vs->size(); i++) {
                    for (uint32_t j = 0; j < level+1; j++) *out << " ";
                    if (vs->hasCounterNames()) {
                        *out << vs->counterName(i) << ": " << vs->count(i) << endl;
                    } else {
                        *out << i << ": " << vs->count(i) << endl;
                    }
                }
            } else if (RunningStat<>* rs = dynamic_cast<RunningStat<>*>(s)) {
                *out << rs->get() << " # " << rs->desc() << endl;
            } else {
                panic("Unrecognized stat type");
            }
        }

    public:
        TextBackendImpl(const char* _filename, AggregateStat* _rootStat) :
            filename(_filename), rootStat(_rootStat)
        {
            std::ofstream out(filename, std::ios_base::out);
            out << "# sim stats" << endl;
            out << "===" << endl;
        }

        void dump(bool buffered) {
            std::ofstream out(filename, std::ios_base::app);
            dumpStat(rootStat, 0, &out);
            out << "===" << endl;
        }
};

TextBackend::TextBackend(const char* filename, AggregateStat* rootStat) {
    backend = new TextBackendImpl(filename, rootStat);
}

void TextBackend::dump(bool buffered) {
    backend->dump(buffered);
}

