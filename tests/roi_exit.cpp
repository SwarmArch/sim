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

#include <stdlib.h>
#include <syscall.h>
#include <unistd.h>

#include "swarm/api.h"
#include "common.h"

inline void exitTask(swarm::Timestamp ts) {
#if defined(TRY_SYS_EXIT)
    syscall(SYS_exit, 1);
#elif defined(TRY_SYS_EXIT_GROUP)
    syscall(SYS_exit_group, 1);
#else
    exit(1);
#endif
}

int main(int argc, const char** argv) {
    swarm::enqueue(exitTask, 42, EnqFlags::NOHINT);
    swarm::run();
    tests::assert_true(true);
    return 0;
}

