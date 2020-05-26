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

// This module implements an interactive command prompt to help in debugging
// the simulator. It is inspired by the RISC-V ISA simulator:
// https://github.com/riscv/riscv-isa-sim/blob/master/riscv/interactive.cc

#include "interactive.h"

static bool atPrompt = false;
bool AtInteractivePrompt() { return atPrompt; }

#ifdef ENABLE_INTERACTIVE

#include <string>
#include <unordered_map>
#include <stdio.h>
#include <readline/readline.h>
#include <readline/history.h>

#include "sim/sim.h"
#include "sim/driver.h"

static uint64_t IC_Immediate(void) { return 1; }

// return non-zero to prompt
static uint64_t (*interactiveCallback)(void) = IC_Immediate;

static uint64_t eventCount;
static uint64_t IC_EventCount(void) { return (--eventCount) == 0; }

static uint64_t targetCycle;
static uint64_t IC_TargetCycle(void) { return getCurCycle() >= targetCycle; }

static uint64_t targetPC;
static uint64_t IC_TargetPC(void) { return spin::getReg(spin::getContext(GetCurThread()->tid), REG_RIP) == targetPC; }

static void InteractiveHelp() {
    printf("ordspecsim interactive mode:\n");
    printf("    help, h                    this help message\n");
    printf("  state:\n");
    printf("    reg REG                    read out reg REG (default: all)\n");
    printf("  running:\n");
    printf("    step N, s N, or [enter]    step N (default 1) thread events\n");
    printf("    until cycle N              run until getCurCycle >= N\n");
    printf("    until pc PC                run until curThread pc == PC\n");
    printf("    quit, q, or EOF [Ctrl-D]   quit\n");
}

// uses state still from strtok
// returns true if OK; false if bad command
static bool InteractiveUntil(void) {
    char *variant = strtok(NULL, " ");
    if (!variant) {
        return false;
    } else if (strcmp(variant, "cycle") == 0) {
        char *val = strtok(NULL, " ");
        if (!val)
            return false;
        targetCycle = atoi(val);
        interactiveCallback = IC_TargetCycle;
    } else if (strcmp(variant, "pc") == 0) {
        char *val = strtok(NULL, " ");
        if (!val)
            return false;
        targetPC = strtol(val, NULL, 0);
        interactiveCallback = IC_TargetPC;
    } else {
        return false;
    }

    return true;
}

// reg name map
static std::unordered_map<std::string, REG> rnm = {
    { "al",  REG_RAX}, { "ah",  REG_RAX}, { "ax",  REG_RAX}, {"eax",  REG_RAX}, {"rax",  REG_RAX},
    { "bl",  REG_RBX}, { "bh",  REG_RBX}, { "bx",  REG_RBX}, {"ebx",  REG_RBX}, {"rbx",  REG_RBX},
    { "cl",  REG_RCX}, { "ch",  REG_RCX}, { "cx",  REG_RCX}, {"ecx",  REG_RCX}, {"rcx",  REG_RCX},
    { "dl",  REG_RDX}, { "dh",  REG_RDX}, { "dx",  REG_RDX}, {"edx",  REG_RDX}, {"rdx",  REG_RDX},
    { "bp",  REG_RBP}, {"ebp",  REG_RBP}, {"rbp",  REG_RBP},
    { "sp",  REG_RSP}, {"esp",  REG_RSP}, {"rsp",  REG_RSP},
    { "di",  REG_RDI}, {"edi",  REG_RDI}, {"rdi",  REG_RDI},
    { "si",  REG_RSI}, {"esi",  REG_RSI}, {"rsi",  REG_RSI},
    { "r8",  REG_R8 }, {"r8d",  REG_R8 }, {"r8w",  REG_R8 }, {"r8b",  REG_R8 },
    { "r9",  REG_R9 }, {"r9d",  REG_R9 }, {"r9w",  REG_R9 }, {"r9b",  REG_R9 },
    { "r10", REG_R10}, {"r10d", REG_R10}, {"r10w", REG_R10}, {"r10b", REG_R10},
    { "r11", REG_R11}, {"r11d", REG_R11}, {"r11w", REG_R11}, {"r11b", REG_R11},
    { "r12", REG_R12}, {"r12d", REG_R12}, {"r12w", REG_R12}, {"r12b", REG_R12},
    { "r13", REG_R13}, {"r13d", REG_R13}, {"r13w", REG_R13}, {"r13b", REG_R13},
    { "r14", REG_R14}, {"r14d", REG_R14}, {"r14w", REG_R14}, {"r14b", REG_R14},
    { "r15", REG_R15}, {"r15d", REG_R15}, {"r15w", REG_R15}, {"r15b", REG_R15},
    { "pc",  REG_RIP}, {"ip",   REG_RIP}, {"eip",  REG_RIP}, {"rip",  REG_RIP}
};

// print out a register's value, allowing for either PIN's value in the enum or
// the actual name of the register
static bool InteractiveReg() {
    auto PrintReg = [](REG reg) {
        printf("  %s\t0x%016lx\n",
               REG_StringShort(REG_FullRegName(reg)).c_str(),
               spin::getReg(spin::getContext(GetCurThread()->tid), reg));
    };
    if (char *regname = strtok(NULL, " ")) {
        do {
            // try it as a number first
            REG r = (REG)strtol(regname, NULL, 0);

            if (!r) {
            // try it as a string
                auto it = rnm.find(regname);
                if (it == rnm.end())
                    return false;
                r = it->second;
            }

            PrintReg(r);
        } while ((regname = strtok(NULL, " ")));
    } else {
        constexpr REG regs[] = {REG_RIP, REG_RAX, REG_RCX, REG_RDX, REG_RBX,
                                REG_RSP, REG_RBP, REG_RSI, REG_RDI, REG_R8,
                                REG_R9,  REG_R10, REG_R11, REG_R12, REG_R13,
                                REG_R14, REG_R15};
        for (REG r : regs) PrintReg(r);
    }
    return true;
}

void InteractivePrompt() {
    if (!interactiveCallback()) return;

    atPrompt = true;
    interactiveCallback = IC_Immediate;

    while (char *line = readline("[sim > ")) {
        struct Freer { void operator()(char *str) { free(str); } };
        std::unique_ptr<char, Freer> freeAtEndOfScope(line);

        add_history(line);
        std::string orig(line);
        char *cmd = strtok(line, " ");

        // switch based on commands (NULL if empty)
        if (!cmd) {
            // empty string (no more tokens); do nothing
            atPrompt = false;
            return;

        } else if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "q") == 0) {
            break;

        } else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "h") == 0) {
            InteractiveHelp();
            continue;

        } else if (strcmp(cmd, "reg") == 0) {
            if (InteractiveReg())
                continue;

        } else if (strcmp(cmd, "step") == 0 || strcmp(cmd, "s") == 0) {
            if (char *val = strtok(NULL, " ")) {
                eventCount = atoi(val); // it's your fault if you put in a negative number
                interactiveCallback = IC_EventCount;
            }
            atPrompt = false;
            return;

        } else if (strcmp(cmd, "until") == 0) {
            if (InteractiveUntil()) {
                atPrompt = false;
                return;
            }
        }

        printf("Unrecognized command: %s\n", orig.c_str());
    }
    atPrompt = false;

    SimEnd();  // does not return
}

#endif  // ENABLE_INTERACTIVE
