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

#include "swarm/api.h"
#include "swarm/cps.h"
#include "common.h"

uint64_t n;

int main(int argc, const char** argv) {
    swarm::forall(0, 0, 100,
            [] (int i) { return NOHINT; },
            [] (swarm::Timestamp ts, int i) {
                swarm::info("This is forall (async) iter %d", i);
            },
            [] (swarm::Timestamp) {}
    );

    pls_forallcc_begin(0, int i, 0, 10, NOHINT, []) {
        swarm::info("forall (with macros) iter %d", i);
        pls_cbegin(0, SAMEHINT, [cc, i]);
        swarm::info("forall continuation iter %d", i);
        cc->run(ts); // finish off the loop iteration synchronously
        pls_cend();
    } pls_forallcc_fallthru([]);

    pls_cbegin(0, NOHINT, [])
    uint32_t x = 1;
    swarm::info("Hello");

    pls_cbegin(0, SAMEHINT, [x])
    swarm::info("%d", x);
    uint32_t y = 2 + 2*x;

    pls_cbegin(ts, SAMEHINT, [y])
    swarm::info("%d", y);

    pls_forall_begin(0, int j, 1, 50, j /*hint*/, [y]) {
        swarm::info("Non-cc forall %d %d", j, y);
    } pls_forall_fallthru([]);

    swarm::info("Starting loopcc");
    pls_loopcc_begin(0, NOHINT, []) {
        if (n++ < 20) {
            swarm::info("loop iteration, %d", n);
            cc->next(ts, SAMEHINT);
        } else {
            cc->done(ts, SAMEHINT);
        }
    } pls_loopcc_fallthru([]);
    swarm::info("Loopcc fallthru %d", n);
    pls_loopcc_end();

    pls_forall_end();

    pls_cend();
    pls_cend();
    pls_cend();
    pls_forallcc_end();

    n = 7;
    swarm::run();

    // [mcj] Ideally this test would assert something to prevent regressions.
    // For now it will always "pass"
    tests::assert_true(true);
    return 0;
}

