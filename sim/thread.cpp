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

#include "thread.h"

#include "spin.h"

std::ostream& ThreadState::operator<<(std::ostream& os) const {
    std::string taskString = task ? task->toString() : "null task";

    os << "Thread<"
       << "id " << tid << ", "
       // At this point we lack a Pin CONTEXT* with the up-to-date thread state,
       // so just print the context most recently saved by libspin,
       // from the switchcall most recently encountered by this thread.
       // If you are printing this within the execution of a switchcall routine
       // (e.g., within HandleMagicOp), then this should be fully up to date.
       << "Most recently saved context: " << std::hex
       << "%rip=0x" << spin::getReg(spin::getContext(tid), REG::REG_RIP);
#define PRINT_THREAD_CONTEXT 1
#if PRINT_THREAD_CONTEXT
    char buf[4 * ((8 + 16) * 4) + (11 + 16) + 1];
    snprintf(buf, sizeof(buf),
             "%%rax=0x%016lx %%rcx=0x%016lx %%rdx=0x%016lx %%rbx=0x%016lx\n"
             "%%rsp=0x%016lx %%rbp=0x%016lx %%rsi=0x%016lx %%rdi=0x%016lx\n"
             " %%r8=0x%016lx  %%r9=0x%016lx %%r10=0x%016lx %%r11=0x%016lx\n"
             "%%r12=0x%016lx %%r13=0x%016lx %%r14=0x%016lx %%r15=0x%016lx\n"
             "%%rflags=0x%016lx",
             spin::getReg(spin::getContext(tid), REG::REG_RAX),
             spin::getReg(spin::getContext(tid), REG::REG_RCX),
             spin::getReg(spin::getContext(tid), REG::REG_RDX),
             spin::getReg(spin::getContext(tid), REG::REG_RBX),
             spin::getReg(spin::getContext(tid), REG::REG_RSP),
             spin::getReg(spin::getContext(tid), REG::REG_RBP),
             spin::getReg(spin::getContext(tid), REG::REG_RSI),
             spin::getReg(spin::getContext(tid), REG::REG_RDI),
             spin::getReg(spin::getContext(tid), REG::REG_R8),
             spin::getReg(spin::getContext(tid), REG::REG_R9),
             spin::getReg(spin::getContext(tid), REG::REG_R10),
             spin::getReg(spin::getContext(tid), REG::REG_R11),
             spin::getReg(spin::getContext(tid), REG::REG_R12),
             spin::getReg(spin::getContext(tid), REG::REG_R13),
             spin::getReg(spin::getContext(tid), REG::REG_R14),
             spin::getReg(spin::getContext(tid), REG::REG_R15),
             spin::getReg(spin::getContext(tid), REG::REG_RFLAGS));
    os << std::endl << buf;
#endif
    os << std::dec << ", "
       << taskString << ", "
       << "cycle " << cycle_ << ", "
       << "state " << state;
    if (isPriv()) os << " priv(" << privDepth << ")";
    if (isPrivDoomed()) os << " doomed";
    os << ">";
    return os;
}
