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

#include <syscall.h>
#include "sim/log.h"
#include "sim/sim.h"
#include "sim/virt/virt.h"
#include "spin.h"

#undef DEBUG
#define DEBUG(args...) //info(args)

namespace virt {
// Ensure at most one synchronous syscall is active at once
static uint32_t syncSyscallTid;

/* External methods */

void init() {
    syncSyscallTid = INVALID_TID;
}

bool syscallEnter(spin::ThreadId tid, spin::ThreadContext* ctxt) {
    assert(syncSyscallTid == INVALID_TID);  // execution must be paused during a synchronous syscall
    // TODO(mcj) why don't we use PIN_GetSyscallNumber? Is it because we aren't
    // given a Pin CONTEXT? Then should spin::getSyscallNumber be added to the
    // API?
    // dsm: Yes it is, and I don't think we should add spin::getSyscallNumber. We're
    // already very heavily tied to the SysV and syscall ABIs (see all the code
    // below). Do we add spin::get{First,Second,...}{Call,Syscall}Arg()
    // methods? I'd be OK with adding REG_SYSCALLNUM, REG_ARG0, ... though.
    uint64_t syscall = spin::getReg(ctxt, REG_RAX);
    DEBUG("[%d] syscall %ld", tid, syscall);

    if (!IsInFastForward()) {
      // Perform reads/writes to syscall input/output data to reflect its memory
      // behavior. This avoids conflicts on syscall data.
      if (syscall == SYS_read) {
          char* buf = (char*) spin::getReg(ctxt, REG_RSI);
          size_t count = (size_t) spin::getReg(ctxt, REG_RDX);
          DEBUG("[%d] sys_read %p / %ld bytes", tid, buf, count);
          // dsm: In theory the writes happen right after the syscall, but doing
          // this is OK because it's synchronous.
          WriteRange(tid, buf, count);  // account for writes to output buffer
      } else if (syscall == SYS_write) {
          char* buf = (char*) spin::getReg(ctxt, REG_RSI);
          size_t count = (size_t) spin::getReg(ctxt, REG_RDX);
          DEBUG("[%d] sys_write %p / %ld bytes", tid, buf, count);
          ReadRange(tid, buf, count);  // validate input buffer
      } else if (syscall == SYS_exit || syscall == SYS_exit_group) {
          info("Exiting simulation because task in thread %d called SYS_%s",
                  tid, (syscall == SYS_exit)? "exit" : "exit_group");
          SimEnd();
      }
    }

    // If a syscall may stop the calling kernel-managed thread indefintely, we
    // must have libspin uncapture the user thread making this syscall and run
    // it asynchronously so we can make forward progress with other threads.
    // Otherwise, if the syscall always returns promptly, we tell libspin not
    // to uncapture, taking the syscall synchronously (i.e., pause *all*
    // threads while the syscall runs) to avoid timing leakage and safely
    // handle syscall side effects.
    // TODO: If we need to blacklist more syscalls, use a bitvector
    bool keepThreadCaptured = true;
    switch (syscall) {
        case SYS_futex:
        case SYS_exit:
        case SYS_exit_group:
        case SYS_sched_yield:
            keepThreadCaptured = false;
            break;
        default: break;
    }
    if (keepThreadCaptured) syncSyscallTid = tid;
    return !keepThreadCaptured;
}

void syscallExit(spin::ThreadId tid, spin::ThreadContext* ctxt) {
    if (syncSyscallTid == tid) syncSyscallTid = INVALID_TID;
}

};
