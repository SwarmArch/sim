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

#include "decoder.h"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string.h>
#include <string>
#include <vector>
#include "sim/locks.h"
#include "sim/log.h"

extern "C" {
#include "xed-interface.h"
}

//XED expansion macros (enable us to type opcodes at a reasonable speed)
#define XC(cat) (XED_CATEGORY_##cat)
#define XO(opcode) (XED_ICLASS_##opcode)

//PORT defines. You might want to change these to affect scheduling

#define PORT_0 (0x1)
#define PORT_1 (0x2)
#define PORT_2 (0x4)
#define PORT_3 (0x8)
#define PORT_4 (0x10)
#define PORT_5 (0x20)

#define PORTS_015 (PORT_0 | PORT_1 | PORT_5)

void DynUop::clear() {
    memset(this, 0, sizeof(DynUop));  // NOTE: This may break if DynUop becomes non-POD
}

Decoder::Instr::Instr(INS _ins) : ins(_ins), numLoads(0), numInRegs(0), numOutRegs(0), numStores(0) {
    uint32_t numOperands = INS_OperandCount(ins);
    for (uint32_t op = 0; op < numOperands; op++) {
        bool read = INS_OperandRead(ins, op);
        bool write = INS_OperandWritten(ins, op);
        assert(read || write);
        if (INS_OperandIsMemory(ins, op)) {
            if (read) loadOps[numLoads++] = op;
            if (write) storeOps[numStores++] = op;
        } else if (INS_OperandIsReg(ins, op) && INS_OperandReg(ins, op)) { //it's apparently possible to get INS_OperandIsReg to be true and an invalid reg ... WTF Pin?
            REG reg = INS_OperandReg(ins, op);
            assert(reg);  // can't be invalid
            reg = REG_FullRegName(reg);  // eax -> rax, etc; o/w we'd miss a bunch of deps!
            if (read) inRegs[numInRegs++] = reg;
            if (write) outRegs[numOutRegs++] = reg;
        } else if (INS_OperandIsAddressGenerator(ins, op)) {
            // address generators occur in LEAs & satisfy neither IsMemory nor,
            // more infuriatingly, IsReg. despite being one operand, address
            // generators may introduce dependences on up to two registers: the
            // base and index registers
            assert(INS_OperandReadOnly(ins, op));

            // base register; optional
            REG reg = INS_OperandMemoryBaseReg(ins, op);
            if (REG_valid(reg)) inRegs[numInRegs++] = REG_FullRegName(reg);

            // index register; optional
            reg = INS_OperandMemoryIndexReg(ins, op);
            if (REG_valid(reg)) inRegs[numInRegs++] = REG_FullRegName(reg);
        } else if (INS_OperandIsImmediate(ins, op)
                   || INS_OperandIsBranchDisplacement(ins, op)) {
            // No need to do anything for immediate operands
        } else if (INS_OperandReg(ins, op) == REG_X87) {
            // We don't model x87 accurately in general
            //reportUnhandledCase(*this, "Instr");
        } else {
            assert(INS_OperandIsImplicit(ins, op));
            // Pin classifies the use and update of RSP in various stack
            // operations as "implicit" operands. Although they contribute to
            // OperandCount, OperandIsReg surprisingly returns false.
            // Let's not bother to add RSP to inRegs or outRegs here,
            // since we won't want to consider it as an ordinary register operand.
            // (See handling of stack operations in Decoder::decodeInstr.)
            //
            // Even more weirdly, the use and update of RSI and RDI in MOVSB
            // and similar string-handling instructions are considered
            // implicit operands for which OperandIsReg returns false.
            // Oh well, with ERMSB in Ivy Bridge and later,
            // who knows what's the right way to model these things anyway?

            // [victory] I wish these assertion weren't true, so we could
            //           cleanly check what the implicit register operand is.
            assert(!REG_valid(INS_OperandReg(ins, op)));
            assert(!REG_valid(INS_OperandMemoryBaseReg(ins, op)));
        }
    }

    //By convention, we move flags regs to the end
    reorderRegs(inRegs, numInRegs);
    reorderRegs(outRegs, numOutRegs);
}

static inline bool isFlagsReg(uint32_t reg) {
    return (reg == REG_EFLAGS || reg == REG_FLAGS || reg == REG_MXCSR);
}

void Decoder::Instr::reorderRegs(uint32_t* array, uint32_t regs) {
    if (regs == 0) return;
    //Unoptimized bubblesort -- when arrays are this short, regularity wins over O(n^2).
    uint32_t swaps;
    do {
        swaps = 0;
        for (uint32_t i = 0; i < regs-1; i++) {
            if (isFlagsReg(array[i]) && !isFlagsReg(array[i+1])) {
                std::swap(array[i], array[i+1]);
                swaps++;
            }
        }
    } while (swaps > 0);
}

//Helper function
static std::string regsToString(const uint32_t* regs, uint32_t numRegs) {
    std::string str = ""; //if efficiency was a concern, we'd use a stringstream
    if (numRegs) {
        str += "(";
        for (uint32_t i = 0; i < numRegs - 1; i++) {
            str += REG_StringShort((REG)regs[i]) + ", ";
        }
        str += REG_StringShort((REG)regs[numRegs - 1]) + ")";
    }
    return str;
}

void Decoder::reportUnhandledCase(const Instr& instr, const char* desc) {
    warn("Unhandled case: %s | %s | "
            "loads=%d stores=%d inRegs=%d %s outRegs=%d %s",
            desc, INS_Disassemble(instr.ins).c_str(),
            instr.numLoads, instr.numStores, instr.numInRegs,
            regsToString(instr.inRegs, instr.numInRegs).c_str(),
            instr.numOutRegs,
            regsToString(instr.outRegs, instr.numOutRegs).c_str());
}

void Decoder::emitLoad(const Instr& instr, uint32_t idx,
                       DynUopVec& uops, uint32_t destReg) {
    assert(idx < instr.numLoads);
    uint32_t op = instr.loadOps[idx];
    uint32_t baseReg = INS_OperandMemoryBaseReg(instr.ins, op);
    uint32_t indexReg = INS_OperandMemoryIndexReg(instr.ins, op);

    if (destReg == 0) destReg = REG_LOAD_TEMP + idx;

    DynUop uop;
    uop.clear();
    uop.rs[0] = baseReg;
    uop.rs[1] = indexReg;
    uop.rd[0] = destReg;
    uop.type = UOP_LOAD;
    uop.portMask = PORT_2;
    uops.push_back(uop); //FIXME: The interface should support in-place grow...
}

void Decoder::emitStore(const Instr& instr, uint32_t idx,
                        DynUopVec& uops, uint32_t srcReg) {
    assert(idx < instr.numStores);
    uint32_t op = instr.storeOps[idx];
    uint32_t baseReg = INS_OperandMemoryBaseReg(instr.ins, op);
    uint32_t indexReg = INS_OperandMemoryIndexReg(instr.ins, op);

    if (srcReg == 0) srcReg = REG_STORE_TEMP + idx;

    uint32_t addrReg;

    //Emit store address uop
    //NOTE: Although technically one uop would suffice with <=1 address register,
    //stores always generate 2 uops. The store address uop is especially important,
    //as in Nehalem loads don't issue after all prior store addresses have been resolved.
    addrReg = REG_STORE_ADDR_TEMP + idx;

    DynUop addrUop;
    addrUop.clear();
    addrUop.rs[0] = baseReg;
    addrUop.rs[1] = indexReg;
    addrUop.rd[0] = addrReg;
    addrUop.lat = 1;
    addrUop.portMask = PORT_3;
    addrUop.type = UOP_STORE_ADDR;
    uops.push_back(addrUop);

    //Emit store uop
    DynUop uop;
    uop.clear();
    uop.rs[0] = addrReg;
    uop.rs[1] = srcReg;
    uop.portMask = PORT_4;
    uop.type = UOP_STORE;
    uops.push_back(uop);
}


void Decoder::emitLoads(const Instr& instr, DynUopVec& uops) {
    for (uint32_t i = 0; i < instr.numLoads; i++) {
        emitLoad(instr, i, uops);
    }
}

void Decoder::emitStores(const Instr& instr, DynUopVec& uops) {
    for (uint32_t i = 0; i < instr.numStores; i++) {
        emitStore(instr, i, uops);
    }
}

void Decoder::emitFence(DynUopVec& uops, uint32_t lat) {
    DynUop uop;
    uop.clear();
    uop.lat = lat;
    uop.portMask = PORT_4; //to the store queue
    uop.type = UOP_FENCE;
    uops.push_back(uop);
}

void Decoder::emitExecUop(uint32_t rs0, uint32_t rs1, uint32_t rd0, uint32_t rd1, DynUopVec& uops, uint32_t lat, uint8_t ports, uint8_t extraSlots) {
    DynUop uop;
    uop.clear();
    uop.rs[0] = rs0;
    uop.rs[1] = rs1;
    uop.rd[0] = rd0;
    uop.rd[1] = rd1;
    uop.lat = lat;
    uop.type = UOP_GENERAL;
    uop.portMask = ports;
    uop.extraSlots = extraSlots;
    uops.push_back(uop);
}

void Decoder::emitBasicMove(const Instr& instr, DynUopVec& uops,
                            uint32_t lat, uint8_t ports) {
    if (instr.numLoads + instr.numInRegs > 1 || instr.numStores + instr.numOutRegs != 1) {
        reportUnhandledCase(instr, "emitBasicMove");
    }
    //Note that we can have 0 loads and 0 input registers. In this case, we are loading from an immediate, and we set the input register to 0 so there is no dependence
    uint32_t inReg = (instr.numInRegs == 1)? instr.inRegs[0] : 0;
    if (!instr.numLoads && !instr.numStores) { //reg->reg
        emitExecUop(inReg, 0, instr.outRegs[0], 0, uops, lat, ports);
    } else if (instr.numLoads && !instr.numStores) { //mem->reg
        emitLoad(instr, 0, uops, instr.outRegs[0]);
    } else if (!instr.numLoads && instr.numStores) { //reg->mem
        emitStore(instr, 0, uops, inReg);
    } else { //mem->mem
        emitLoad(instr, 0, uops);
        emitStore(instr, 0, uops, REG_LOAD_TEMP /*chain with load*/);
    }
}

void Decoder::emitXchg(const Instr& instr, DynUopVec& uops) {
    if (instr.numLoads) { // mem <-> reg
        assert(instr.numLoads == 1 && instr.numStores == 1);
        assert(instr.numInRegs == 1 && instr.numOutRegs == 1);
        assert(instr.inRegs[0] == instr.outRegs[0]);

        emitLoad(instr, 0, uops);
        emitExecUop(instr.inRegs[0], 0, REG_EXEC_TEMP, 0, uops, 1, PORTS_015); //r -> temp
        emitExecUop(REG_LOAD_TEMP, 0, instr.outRegs[0], 0, uops, 1, PORTS_015); // load -> r
        emitStore(instr, 0, uops, REG_EXEC_TEMP); //temp -> out
        if (!INS_LockPrefix(instr.ins)) emitFence(uops, 14); //xchg has an implicit lock prefix (TODO: Check we don't introduce two fences...)
    } else { // reg <-> reg
        assert(instr.numInRegs == 2 && instr.numOutRegs == 2);
        assert(instr.inRegs[0] == instr.outRegs[0]);
        assert(instr.inRegs[1] == instr.outRegs[1]);

        emitExecUop(instr.inRegs[0], 0, REG_EXEC_TEMP, 0, uops, 1, PORTS_015);
        emitExecUop(instr.inRegs[1], 0, instr.outRegs[0], 0, uops, 1, PORTS_015);
        emitExecUop(REG_EXEC_TEMP, 0, instr.outRegs[1], 0, uops, 1, PORTS_015);
    }
}


void Decoder::emitConditionalMove(const Instr& instr, DynUopVec& uops,
                                  uint32_t lat, uint8_t ports) {
    uint32_t initialUops = uops.size();
    assert(instr.numOutRegs == 1); //always move to reg
    assert(instr.numStores == 0);

    if (instr.numLoads) {
        assert(instr.numLoads == 1);
        assert(instr.numInRegs == 1);
        uint32_t flagsReg = instr.inRegs[0];
        emitExecUop(flagsReg, 0, REG_EXEC_TEMP, 0, uops, lat, ports);
        emitLoad(instr, 0, uops);
        uint32_t numUops = uops.size();
        assert(numUops - initialUops == 2);
        //We need to make the load depend on the result. This is quite crude, but works:
        uops[numUops - 2].rs[1] = uops[numUops - 1].rs[1]; //comparison uop gets source of load (possibly 0)
        uops[numUops - 1].rs[1] = REG_EXEC_TEMP; //load uop is made to depend on comparison uop
        //TODO: Make this follow codepath below + load
    } else {
        assert(instr.numInRegs == 2);
        assert(instr.numOutRegs == 1);
        uint32_t flagsReg = instr.inRegs[1];
        //Since this happens in 2 instructions, we'll assume we need to read the output register
        emitExecUop(flagsReg, instr.inRegs[0], REG_EXEC_TEMP, 0, uops, 1, ports);
        emitExecUop(instr.outRegs[0], REG_EXEC_TEMP, instr.outRegs[0], 0, uops, lat, ports);
    }
}

void Decoder::emitCompareAndExchange(const Instr& instr, DynUopVec& uops) {
    emitLoads(instr, uops);

    uint32_t srcs = instr.numLoads + instr.numInRegs;
    uint32_t dsts = instr.numStores + instr.numOutRegs;

    uint32_t srcRegs[srcs + 2];
    uint32_t dstRegs[dsts + 2];
    populateRegArrays(instr, srcRegs, dstRegs);

    assert(srcs == 3);
    assert(dsts == 3);

    //reportUnhandledCase(instr, "XXXX");
    //info("%d %d %d | %d %d %d", srcRegs[0], srcRegs[1], srcRegs[2], dstRegs[0], dstRegs[1], dstRegs[2]);

    uint32_t rflags = dstRegs[2];
    uint32_t rax = dstRegs[1]; //note: can be EAX, etc
    assert(srcRegs[2] == rax); //if this fails, pin has changed the register orderings...

    //Compare destination (first operand) w/ RAX. If equal, copy source (second operand) into destination and set the zero flag; o/w copy destination into RAX
    if (!instr.numLoads) {
        //2 swaps, implemented in 2 stages: first, and all sources with rflags.zf; then or results pairwise. This is pure speculation, but matches uops required.
        emitExecUop(srcRegs[0], rax, REG_EXEC_TEMP, rflags, uops, 1, PORTS_015); //includes compare
        emitExecUop(srcRegs[1], rflags, REG_EXEC_TEMP+1, 0, uops, 2, PORTS_015);
        emitExecUop(srcRegs[2], rflags, REG_EXEC_TEMP+2, 0, uops, 2, PORTS_015);

        emitExecUop(REG_EXEC_TEMP, REG_EXEC_TEMP+1, dstRegs[0], 0, uops, 2, PORTS_015);
        emitExecUop(REG_EXEC_TEMP+1, REG_EXEC_TEMP+2, dstRegs[1] /*rax*/, 0, uops, 2, PORTS_015);
    } else {
        //6 uops (so 3 exec), and critical path is 4 (for rax), GO FIGURE
        emitExecUop(srcRegs[0], rax, REG_EXEC_TEMP, rflags, uops, 2, PORTS_015);
        emitExecUop(srcRegs[1], rflags, dstRegs[0], 0, uops, 2, PORTS_015); //let's assume we can do a fancy conditional store
        emitExecUop(srcRegs[2], REG_EXEC_TEMP, dstRegs[1] /*rax*/, 0, uops, 2, PORTS_015); //likewise
    }

    //NOTE: While conceptually srcRegs[0] == dstRegs[0], when it's a memory location they map to different temporary regs

    emitStores(instr, uops);
}



void Decoder::populateRegArrays(const Instr& instr,
                                uint32_t* srcRegs,
                                uint32_t* dstRegs) {
    uint32_t curSource = 0;
    for (uint32_t i = 0; i < instr.numLoads; i++) {
        srcRegs[curSource++] = REG_LOAD_TEMP + i;
    }
    for (uint32_t i = 0; i < instr.numInRegs; i++) {
        srcRegs[curSource++] = instr.inRegs[i];
    }
    srcRegs[curSource++] = 0;
    srcRegs[curSource++] = 0;

    uint32_t curDest = 0;
    for (uint32_t i = 0; i < instr.numStores; i++) {
        dstRegs[curDest++] = REG_STORE_TEMP + i;
    }
    for (uint32_t i = 0; i < instr.numOutRegs; i++) {
        dstRegs[curDest++] = instr.outRegs[i];
    }
    dstRegs[curDest++] = 0;
    dstRegs[curDest++] = 0;
}

void Decoder::emitBasicOp(const Instr& instr, DynUopVec& uops,
                          uint32_t lat, uint8_t ports,
                          uint8_t extraSlots, bool reportUnhandled) {
    emitLoads(instr, uops);

    uint32_t srcs = instr.numLoads + instr.numInRegs;
    uint32_t dsts = instr.numStores + instr.numOutRegs;

    uint32_t srcRegs[srcs + 2];
    uint32_t dstRegs[dsts + 2];
    populateRegArrays(instr, srcRegs, dstRegs);

    if (reportUnhandled && (srcs > 2 || dsts > 2)) reportUnhandledCase(instr, "emitBasicOp"); //We're going to be ignoring some dependencies

    emitExecUop(srcRegs[0], srcRegs[1], dstRegs[0], dstRegs[1], uops, lat, ports, extraSlots);

    emitStores(instr, uops);
}

void Decoder::emitChainedOp(const Instr& instr, DynUopVec& uops,
                            uint32_t numUops, uint32_t* latArray,
                            uint8_t* portsArray) {
    emitLoads(instr, uops);

    uint32_t srcs = instr.numLoads + instr.numInRegs;
    uint32_t dsts = instr.numStores + instr.numOutRegs;

    uint32_t srcRegs[srcs + 2];
    uint32_t dstRegs[dsts + 2];
    populateRegArrays(instr, srcRegs, dstRegs);

    assert(numUops > 1);
    //if (srcs != numUops + 1) reportUnhandledCase(instr, "emitChainedOps");
    assert(srcs + 2 >= numUops + 1); // note equality is not necessary in case one or more operands are immediates

    emitExecUop(srcRegs[0], srcRegs[1], REG_EXEC_TEMP, 0, uops, latArray[0], portsArray[0]);
    for (uint32_t i = 1; i < numUops-1; i++) {
        emitExecUop(REG_EXEC_TEMP, srcRegs[i+1], REG_EXEC_TEMP, 0, uops, latArray[i], portsArray[i]);
    }
    emitExecUop(REG_EXEC_TEMP, srcRegs[numUops-1], dstRegs[0], dstRegs[1], uops, latArray[numUops-1], portsArray[numUops-1]);

    emitStores(instr, uops);
}

//Some convert ops are implemented in 2 uops, even though they could just use one given src/dst reg constraints
void Decoder::emitConvert2Op(const Instr& instr, DynUopVec& uops,
                             uint32_t lat1, uint32_t lat2,
                             uint8_t ports1, uint8_t ports2) {
    if (instr.numStores > 0 || instr.numLoads > 1 || instr.numOutRegs != 1 || instr.numLoads + instr.numInRegs != 1) {
        reportUnhandledCase(instr, "convert");
    } else {
        //May have single load, has single output
        uint32_t src;
        if (instr.numLoads) {
            emitLoads(instr, uops);
            src = REG_LOAD_TEMP;
        } else {
            src = instr.inRegs[0];
        }
        uint32_t dst = instr.outRegs[0];
        emitExecUop(src, 0, REG_EXEC_TEMP, 0, uops, lat1, ports1);
        emitExecUop(REG_EXEC_TEMP, 0, dst, 0, uops, lat2, ports2);
    }
}


void Decoder::emitMul(const Instr& instr, DynUopVec& uops) {
    uint32_t dsts = instr.numStores + instr.numOutRegs;
    if (dsts == 3) {
        emitLoads(instr, uops);

        uint32_t srcs = instr.numLoads + instr.numInRegs;

        uint32_t srcRegs[srcs + 2];
        uint32_t dstRegs[dsts + 2];
        populateRegArrays(instr, srcRegs, dstRegs);

        assert(srcs <= 2);

        emitExecUop(srcRegs[0], srcRegs[1], dstRegs[0], REG_EXEC_TEMP, uops, 3, PORT_1);
        emitExecUop(srcRegs[0], srcRegs[1], dstRegs[1], REG_EXEC_TEMP+1, uops, 3, PORT_1);
        emitExecUop(REG_EXEC_TEMP, REG_EXEC_TEMP+1, dstRegs[2], 0, uops, 1, PORTS_015);

        emitStores(instr, uops);
    } else {
        emitBasicOp(instr, uops, 3, PORT_1);
    }
}

void Decoder::emitDiv(const Instr& instr, DynUopVec& uops) {
    uint32_t srcs = instr.numLoads + instr.numInRegs;
    uint32_t dsts = instr.numStores + instr.numOutRegs;

    /* div and idiv are microsequenced, with a variable number of uops on all ports, and have fixed
     * input and output regs (rdx:rax is the input, rax is the quotient and rdx is the remainder).
     * Also, the number of uops and latency depends on the data. We approximate this with a 4-uop
     * sequence that sorta kinda emulates the typical latency.
     */

    uint32_t srcRegs[srcs + 2];
    uint32_t dstRegs[dsts + 2];
    populateRegArrays(instr, srcRegs, dstRegs);

    //assert(srcs == 3); //there is a variant of div that uses only 2 regs --> see below
    //assert(dsts == 3);
    assert(instr.numInRegs > 1);

    uint32_t width = INS_OperandWidth(instr.ins, 1);
    uint32_t lat = 0;
    switch (width) {
        case 8:
            lat = 15;
            break;
        case 16:
            lat = 19;
            break;
        case 32:
            lat = 23;
            break;
        case 64:
            lat = 63;
            break;
        default:
            panic("emitDiv: Invalid reg size");
    }
    uint8_t extraSlots = lat-1;
    if (srcs == 3 && dsts == 3) {
        emitLoads(instr, uops);

        emitExecUop(srcRegs[0], srcRegs[1], REG_EXEC_TEMP, 0, uops, lat, PORTS_015, extraSlots);
        emitExecUop(srcRegs[0], srcRegs[2], REG_EXEC_TEMP+1, 0, uops, lat, PORTS_015, extraSlots);
        emitExecUop(REG_EXEC_TEMP, REG_EXEC_TEMP+1, dstRegs[0], dstRegs[1], uops, 1, PORTS_015); //quotient and remainder
        emitExecUop(REG_EXEC_TEMP, REG_EXEC_TEMP+1, dstRegs[2], 0, uops, 1, PORTS_015); //flags

        emitStores(instr, uops);
    } else if (srcs <= 2 && dsts <= 2) {
        emitBasicOp(instr, uops, lat, PORTS_015, extraSlots);
    } else {
        reportUnhandledCase(instr, "emitDiv");
    }
}

//Helper function
static bool dropRegister(uint32_t targetReg, uint32_t* regs, uint32_t& numRegs) {
    for (uint32_t i = 0; i < numRegs; i++) {
        uint32_t reg = regs[i];
        if (reg == targetReg) {
            //Shift rest of regs
            for (uint32_t j = i; j < numRegs - 1; j++) regs[j] = regs[j+1];
            numRegs--;
            return true;
        }
    }
    return false;
}

void Decoder::dropStackRegister(Instr& instr) {
    bool dropIn = dropRegister(REG_RSP, instr.inRegs, instr.numInRegs);
    bool dropOut = dropRegister(REG_RSP, instr.outRegs, instr.numOutRegs);
    if (!dropIn && !dropOut) /*reportUnhandledCase(instr, "dropStackRegister (no RSP found)")*/;
    else reportUnhandledCase(instr, "dropStackRegister (RSP found)");
}


bool Decoder::decodeInstr(INS ins, DynUopVec& uops) {
    uint32_t initialUops = uops.size();
    bool inaccurate = false;
    xed_category_enum_t category = (xed_category_enum_t) INS_Category(ins);
    xed_iclass_enum_t opcode = (xed_iclass_enum_t) INS_Opcode(ins);

    Instr instr(ins);

    bool isLocked = false;
    // NOTE(dsm): IsAtomicUpdate == xchg or LockPrefix (xchg has in implicit lock prefix)
    if (INS_IsAtomicUpdate(instr.ins)) {
        isLocked = true;
        emitFence(uops, 0); //serialize the initial load w.r.t. all prior stores
    }
/*
    if (category == XC(MISC) && opcode == XO(LEA)){
    	int numOperands = INS_OperandCount(ins);
    	printf("decoder code\n");
    	cout << "LEA op count "<<  INS_OperandCount(ins) <<" \n";
    	 for (uint32_t op = 0; op < numOperands; op++) {
    	        bool read = INS_OperandRead(ins, op);
    	        bool write = INS_OperandWritten(ins, op);
    	        assert(read || write);
    	        if (INS_OperandIsMemory(ins, op)) {
    	            printf("Op %d is mem\n",op);
    	        } else if (INS_OperandIsReg(ins, op) && INS_OperandReg(ins, op)) { //it's apparently possible to get INS_OperandIsReg to be true and an invalid reg ... WTF Pin?
    	            REG reg = INS_OperandReg(ins, op);
    	            assert(reg);  // can't be invalid
    	            reg = REG_FullRegName(reg);  // eax -> rax, etc; o/w we'd miss a bunch of deps!
    	            printf("Op %d is reg %d %d\n",op,reg, read?1:0);
    	            //if (read) inRegs[numInRegs++] = reg;
    	            //if (write) outRegs[numOutRegs++] = reg;
    	        } else  {
    	        	printf("Op %d is neither mem or reg. WTF?\n",op);
    	        }
    	    }
    	 printf("libspin code\n");
    	 for (uint32_t i = 0; i < INS_MaxNumRRegs(ins); i++) {
			 REG reg = INS_RegR(ins, i);
			 if (REG_valid(reg)) {
				 reg = REG_FullRegName(reg);

			 }
			 printf("R %i %d\n", i,reg);
		 }
    	 for (uint32_t i = 0; i < INS_MaxNumWRegs(ins); i++) {
			 REG reg = INS_RegW(ins, i);
			 if (REG_valid(reg)) {
				 reg = REG_FullRegName(reg);
			 }
			 printf("W %i %d\n", i,reg);
		 }
    }
*/
    switch (category) {
        //NOPs are optimized out in the execution pipe, but they still grab a ROB entry
        case XC(NOP):
        case XC(WIDENOP):
            emitExecUop(0, 0, 0, 0, uops, 1, PORTS_015);
            break;

         /* Moves */
        case XC(DATAXFER):
            switch (opcode) {
                case XO(BSWAP):
                    emitBasicMove(instr, uops, 1, PORT_1);
                    break;
                case XO(MOV):
                    emitBasicMove(instr, uops, 1, PORTS_015);
                    break;
                case XO(MOVAPS):
                case XO(MOVAPD):
                case XO(MOVUPS):
                case XO(MOVUPD):
                case XO(MOVSS):
                case XO(MOVSD):
                case XO(MOVSD_XMM):
                case XO(MOVHLPS):
                case XO(MOVLHPS):
                case XO(MOVDDUP):
                case XO(MOVSHDUP):
                case XO(MOVSLDUP):
                    emitBasicMove(instr, uops, 1, PORT_5);
                    break;
                case XO(MOVHPS):
                case XO(MOVHPD):
                case XO(MOVLPS):
                case XO(MOVLPD):
                    //A bit unclear... could be 2 or 3 cycles, and current microbenchmarks are not enough to tell
                    emitBasicOp(instr, uops, /*2*/ 1, PORT_5);
                    break;
                case XO(MOVMSKPS):
                case XO(MOVMSKPD):
                    emitBasicMove(instr, uops, 1, PORT_0);
                    break;
                case XO(MOVD):
                case XO(MOVQ):
                case XO(MOVDQA):
                case XO(MOVDQU):
                case XO(MOVDQ2Q):
                case XO(MOVQ2DQ):
                    emitBasicMove(instr, uops, 1, PORTS_015); //like mov
                    break;
                case XO(MOVSX):
                case XO(MOVSXD):
                case XO(MOVZX):
                    emitBasicMove(instr, uops, 1, PORTS_015); //like mov
                    break;
                case XO(XCHG):
                    emitXchg(instr, uops);
                    break;
                default:
                    //TODO: MASKMOVQ, MASKMOVDQ, MOVBE (Atom only), MOVNTxx variants (nontemporal), MOV_CR and MOV_DR (privileged?), VMOVxxxx variants (AVX)
                    inaccurate = true;
                    emitBasicMove(instr, uops, 1, PORTS_015);
            }
            break;

        case XC(CMOV):
            emitConditionalMove(instr, uops, 1, PORTS_015);
            break;
        case XC(FCMOV): //these are part of x87, so be inaccurate
            break;

        /* Barebones arithmetic instructions */
        case XC(BINARY):
            {
                if (opcode == XO(ADC) || opcode == XO(SBB)) {
                    uint32_t lats[] = {1, 1};
                    uint8_t ports[] = {PORTS_015, PORTS_015};
                    emitChainedOp(instr, uops, 2, lats, ports);
                } else if (opcode == XO(MUL) || opcode == XO(IMUL)) {
                    emitMul(instr, uops);
                } else if (opcode == XO(DIV) || opcode == XO(IDIV)) {
                    emitDiv(instr, uops);
                } else {
                    //ADD, SUB, CMP, DEC, INC, NEG are 1 cycle
                    emitBasicOp(instr, uops, 1, PORTS_015);
                }
            }
            break;
        case XC(BITBYTE):
            {
                uint32_t opLat = 1;
                switch (opcode) {
                    case XO(BSF):
                    case XO(BSR):
                        opLat = 3;
                        break;
                        //TODO: EXTRQ, INSERTQ, LZCNT
                    default: {} //BT, BTx, SETcc ops are 1 cycle
                }
                emitBasicOp(instr, uops, opLat, PORTS_015);
            }
            break;
        case XC(LOGICAL):
            //AND, OR, XOR, TEST are 1 cycle
            emitBasicOp(instr, uops, 1, PORTS_015);
            break;
        case XC(ROTATE):
            {
                uint32_t opLat = 1; //ROR, ROL 1 cycle
                if (opcode == XO(RCR) || opcode == XO(RCL)) opLat = 2;
                emitBasicOp(instr, uops, opLat, PORT_0 | PORT_5);
            }
            break;
        case XC(SHIFT):
            {
                if (opcode == XO(SHLD)|| opcode == XO(SHRD)) {
                    uint32_t lats[] = {2, opcode == XO(SHLD)? 1u : 2u}; //SHRD takes 4 cycles total, SHLD takes 3
                    uint8_t ports[] = {PORTS_015, PORTS_015};
                    emitChainedOp(instr, uops, 2, lats, ports);
                } else {
                    uint32_t opLat = 1; //SHR SHL SAR are 1 cycle
                    emitBasicOp(instr, uops, opLat, PORT_0 | PORT_5);
                }
            }
            break;
        case XC(DECIMAL): //pack/unpack BCD, these seem super-deprecated
            {
                uint32_t opLat = 1;
                switch (opcode) {
                    case XO(AAA):
                    case XO(AAS):
                    case XO(DAA):
                    case XO(DAS):
                        opLat = 3;
                        break;
                    case XO(AAD):
                        opLat = 15;
                        break;
                    case XO(AAM):
                        opLat = 20;
                        break;
                    default:
                        panic("Invalid opcode for this class");
                }
                emitBasicOp(instr, uops, opLat, PORTS_015);
            }
            break;
        case XC(FLAGOP):
            switch (opcode) {
                case XO(LAHF):
                case XO(SAHF):
                    emitBasicOp(instr, uops, 1, PORTS_015);
                    break;
                case XO(CLC):
                case XO(STC):
                case XO(CMC):
                    emitBasicOp(instr, uops, 1, PORTS_015);
                    break;
                case XO(CLD):
                    emitExecUop(0, 0, REG_EXEC_TEMP, 0, uops, 2, PORTS_015);
                    emitExecUop(REG_EXEC_TEMP, 0, REG_RFLAGS, 0, uops, 2, PORTS_015);
                    break;
                case XO(STD):
                    emitExecUop(0, 0, REG_EXEC_TEMP, 0, uops, 3, PORTS_015);
                    emitExecUop(REG_EXEC_TEMP, 0, REG_RFLAGS, 0, uops, 2, PORTS_015);
                    break;
                default:
                    inaccurate = true;
            }
            break;

        case XC(SEMAPHORE): //atomic ops, these must involve memory
            //reportUnhandledCase(instr, "SEM");
            //emitBasicOp(instr, uops, 1, PORTS_015);

            switch (opcode) {
                case XO(CMPXCHG):
                case XO(CMPXCHG8B):
                //case XO(CMPXCHG16B): //not tested...
                    emitCompareAndExchange(instr, uops);
                    break;
                case XO(XADD):
                    {
                        uint32_t lats[] = {2, 2};
                        uint8_t ports[] = {PORTS_015, PORTS_015};
                        emitChainedOp(instr, uops, 2, lats, ports);
                    }
                    break;
                default:
                    inaccurate = true;
            }
            break;

        /* FP, SSE and other extensions */
        case /*XC(X)87_ALU*/ XC(X87_ALU):
            //emitBasicOp(instr, uops, 1, PORTS_015);
            break;

        case XED_CATEGORY_3DNOW:
            //emitBasicOp(instr, uops, 1, PORTS_015);
            break;

        case XC(MMX):
            //emitBasicOp(instr, uops, 1, PORTS_015);
            break;

        case XC(SSE):
            {
                //TODO: Multi-uop BLENDVXX, DPXX

                uint32_t lat = 1;
                uint8_t ports = PORTS_015;
                uint8_t extraSlots = 0;
                switch (opcode) {
                    case XO(ADDPD):
                    case XO(ADDPS):
                    case XO(ADDSD):
                    case XO(ADDSS):
                    case XO(SUBPD):
                    case XO(SUBPS):
                    case XO(SUBSD):
                    case XO(SUBSS):
                    case XO(ADDSUBPD):
                    case XO(ADDSUBPS):
                        lat = 3;
                        ports = PORT_1;
                        break;

                    case XO(BLENDPS):
                    case XO(BLENDPD):
                    case XO(SHUFPS):
                    case XO(SHUFPD):
                    case XO(UNPCKHPD):
                    case XO(UNPCKHPS):
                    case XO(UNPCKLPD):
                    case XO(UNPCKLPS):
                        lat = 1;
                        ports = PORT_5;
                        break;

                    case XO(CMPPD):
                    case XO(CMPPS):
                    case XO(CMPSD):
                    case XO(CMPSS):
                        lat = 3;
                        ports = PORT_1;
                        break;

                    case XO(COMISD):
                    case XO(COMISS):
                    case XO(UCOMISD):
                    case XO(UCOMISS):
                        lat = 1+2; //writes rflags, always crossing xmm -> int domains
                        ports = PORT_1;
                        break;

                    case XO(DIVPS):
                    case XO(DIVSS):
                        lat = 7; //from mubench
                        ports = PORT_0;
                        extraSlots = lat - 1; //non-pipelined
                        break;
                    case XO(DIVPD):
                    case XO(DIVSD):
                        lat = 7; //from mubench
                        ports = PORT_0; //non-pipelined
                        extraSlots = lat - 1;
                        break;

                    case XO(MAXPD):
                    case XO(MAXPS):
                    case XO(MAXSD):
                    case XO(MAXSS):
                    case XO(MINPD):
                    case XO(MINPS):
                    case XO(MINSD):
                    case XO(MINSS):
                        lat = 3;
                        ports = PORT_1;
                        break;

                    case XO(MULSS):
                    case XO(MULPS):
                        lat = 4;
                        ports = PORT_0;
                        break;
                    case XO(MULSD):
                    case XO(MULPD):
                        lat = 5;
                        ports = PORT_0;
                        break;

                    case XO(RCPPS):
                    case XO(RCPSS):
                        lat = 3;
                        ports = PORT_1;
                        break;

                    case XO(ROUNDPD):
                    case XO(ROUNDPS):
                    case XO(ROUNDSD):
                    case XO(ROUNDSS):
                        lat = 3;
                        ports = PORT_1;
                        break;

                    case XO(RSQRTPS):
                    case XO(RSQRTSS):
                        lat = 3;
                        ports = PORT_1;
                        extraSlots = 1; //from mubench, has reciprocal thput of 2
                        break;

                    case XO(SQRTSS):
                    case XO(SQRTPS):
                        lat = 7; //from mubench
                        ports = PORT_0;
                        extraSlots = lat-1; //unpiped
                        break;

                    case XO(SQRTSD):
                    case XO(SQRTPD):
                        lat = 7; //from mubench
                        ports = PORT_0;
                        extraSlots = lat-1; //unpiped
                        break;

                    case XO(POPCNT):
                    case XO(CRC32):
                        lat = 3;
                        ports = PORT_1;
                        break;

                    //Packed arith; these are rare, so I'm implementing only what I've seen used (and simple variants)
                    case XO(PADDB):
                    case XO(PADDD):
                    case XO(PADDQ):
                    case XO(PADDSB):
                    case XO(PADDSW):
                    case XO(PADDUSB):
                    case XO(PADDUSW):
                    case XO(PADDW):
                    case XO(PSUBB):
                    case XO(PSUBD):
                    case XO(PSUBQ):
                    case XO(PSUBSB):
                    case XO(PSUBSW):
                    case XO(PSUBUSB):
                    case XO(PSUBUSW):
                    case XO(PSUBW):

                    case XO(PALIGNR):

                    case XO(PCMPEQB):
                    case XO(PCMPEQD):
                    case XO(PCMPEQQ):
                    case XO(PCMPEQW):
                    case XO(PCMPGTB):
                    case XO(PCMPGTD):
                    case XO(PCMPGTW):

                    case XO(PUNPCKHBW):
                    case XO(PUNPCKHDQ):
                    case XO(PUNPCKHQDQ):
                    case XO(PUNPCKHWD):
                    case XO(PUNPCKLBW):
                    case XO(PUNPCKLDQ):
                    case XO(PUNPCKLQDQ):
                    case XO(PUNPCKLWD):

                    case XO(PSHUFB):
                    case XO(PSHUFD):
                    case XO(PSHUFHW):
                    case XO(PSHUFLW):
                        lat = 1;
                        ports = PORT_0 | PORT_5;
                        break;

                    case XO(PCMPGTQ): //weeeird, only packed comparison that's done differently
                        lat = 3;
                        ports = PORT_1;
                        break;

                    case XO(PMOVMSKB):
                        lat = 2+2;
                        ports = PORT_0;
                        break;

                    default:
                        inaccurate = true;
                }
                emitBasicOp(instr, uops, lat, ports, extraSlots);
            }
            break;

        case XC(STTNI): //SSE 4.2
            break;

        case XC(CONVERT): //part of SSE
            switch (opcode) {
                case XO(CVTPD2PS):
                case XO(CVTSD2SS):
                    emitConvert2Op(instr, uops, 2, 2, PORT_1, PORT_5);
                    break;
                case XO(CVTPS2PD):
                    emitConvert2Op(instr, uops, 1, 1, PORT_0, PORT_5);
                    break;
                case XO(CVTSS2SD):
                    emitBasicOp(instr, uops, 1, PORT_0);
                    break;
                case XO(CVTDQ2PS):
                case XO(CVTPS2DQ):
                case XO(CVTTPS2DQ):
                    emitBasicOp(instr, uops, 3+2 /*domain change*/, PORT_1);
                    break;
                case XO(CVTDQ2PD):
                case XO(CVTPD2DQ):
                case XO(CVTTPD2DQ):
                    emitConvert2Op(instr, uops, 2, 2+2 /*domain change*/, PORT_1, PORT_5);
                    break;
                case XO(CVTPI2PS):
                case XO(CVTPS2PI):
                case XO(CVTTPS2PI):
                    emitBasicOp(instr, uops, 3+2 /*domain change*/, PORT_1);
                    break;
                case XO(CVTPI2PD):
                case XO(CVTPD2PI):
                case XO(CVTTPD2PI):
                    emitConvert2Op(instr, uops, 2, 2+2 /*domain change*/, PORT_1, PORT_0 | PORT_5);
                    break;
                case XO(CVTSI2SS):
                case XO(CVTSS2SI):
                case XO(CVTTSS2SI):
                    emitBasicOp(instr, uops, 3+2 /*domain change*/, PORT_1);
                    break;
                case XO(CVTSI2SD):
                    emitConvert2Op(instr, uops, 2, 2+2 /*domain change*/, PORT_1, PORT_0);
                    break;
                case XO(CVTSD2SI):
                case XO(CVTTSD2SI):
                    emitBasicOp(instr, uops, 3+2 /*domain change*/, PORT_1);
                    break;
                case XO(CBW):
                case XO(CWDE):
                case XO(CDQE):
                    emitBasicOp(instr, uops, 1, PORTS_015);
                    break;
                case XO(CWD):
                case XO(CDQ):
                case XO(CQO):
                    emitBasicOp(instr, uops, 1, PORT_0 | PORT_5);
                    break;

                default: // AVX converts
                    inaccurate = true;
            }
            break;

        case XC(AVX):
            //TODO: Whatever, Nehalem has no AVX
            break;

        case XC(BROADCAST): //part of AVX
            //TODO: Same as AVX
            break;

        case XC(AES):
            break;

        case XC(PCLMULQDQ): //CLMUL extension (carryless multiply, generally related to AES-NI)
            break;

        case XC(XSAVE):
        case XC(XSAVEOPT): //hold your horses, it's optimized!! (AVX)
            break;

        /* Control flow ops (branches, jumps) */
        case XC(COND_BR):
        case XC(UNCOND_BR):
            // We model all branches and jumps with a latency of 1. Far jumps are really expensive, but they should be exceedingly rare (from Intel's manual, they are used for call gates, task switches, etc.)
            emitBasicOp(instr, uops, 1, PORT_5);
            if (opcode == XO(JMP_FAR)) inaccurate = true;
            break;

        /* Stack operations */
        case XC(CALL):
        case XC(RET):
            /* Call and ret are both unconditional branches and stack operations; however, Pin does not list RSP as source or destination for them */
            //dropStackRegister(instr); //stack engine kills accesses to RSP
            emitBasicOp(instr, uops, 1, PORT_5);
            if (opcode != XO(CALL_NEAR) && opcode != XO(RET_NEAR)) inaccurate = true; //far call/ret or irets are far more complex
            break;

        case XC(POP):
        case XC(PUSH):
            //Again, RSP is not included here, so no need to remove it.
            switch (opcode) {
                case XO(POP):
                case XO(PUSH):
                    //Basic PUSH/POP are just moves. They are always to/from memory, so PORTS is irrelevant
                    emitBasicMove(instr, uops, 1, PORTS_015);
                    break;
                case XO(POPF):
                case XO(POPFD):
                case XO(POPFQ):
                    //Java uses POPFx/PUSHFx variants. POPF is complicated, 8 uops... microsequenced
                    inaccurate = true;
                    emitBasicOp(instr, uops, 14, PORTS_015);
                    break;
                case XO(PUSHF):
                case XO(PUSHFD):
                case XO(PUSHFQ):
                    //This one we can handle... 2 exec uops + store and reciprocal thput of 1
                    {
                        uint32_t lats[] = {1, 1};
                        uint8_t ports[] = {PORTS_015, PORTS_015};
                        emitChainedOp(instr, uops, 2, lats, ports);
                    }
                    break;

                default:
                    inaccurate = true;
            }
            break;

        /* Prefetches */
        case XC(PREFETCH):
            //A prefetch is just a load that doesn't feed into any register (or REG_TEMP in this case)
            //NOTE: Not exactly, because this will serialize future loads under TSO
            emitLoads(instr, uops);
            break;

        /* Stuff on the system side (some of these are privileged) */
        case XC(INTERRUPT):
        case XC(SYSCALL):
        case XC(SYSRET):
        case XC(IO):
            break;

        case XC(SYSTEM):
            //TODO: Privileged ops are not included
            /*switch(opcode) {
                case XO(RDTSC):
                case XO(RDTSCP):
                    opLat = 24;
                    break;
                case XO(RDPMC):
                    opLat = 40;
                    break;
                default: ;
            }*/
            break;

        case XC(SEGOP):
            //TODO: These are privileged, right? They are expensive but rare anyhow
            break;

        case XC(VTX): //virtualization, hmmm
            //TODO
            break;


        /* String ops (I'm reading the manual and they seem just like others... wtf?) */
        case XC(STRINGOP):
            switch (opcode) {
                case XO(STOSB):
                case XO(STOSW):
                case XO(STOSD):
                case XO(STOSQ):
                    //mov [rdi] <- rax
                    //add rdi, 8
                    //emitBasicOp(instr, uops, 1, PORTS_015); //not really, this emits the store later and there's no dep (the load is direct to reg)
                    emitStore(instr, 0, uops, REG_RAX);
                    emitExecUop(REG_RDI, 0, REG_RDI, 0, uops, 1, PORTS_015);
                    break;
                case XO(LODSB):
                case XO(LODSW):
                case XO(LODSD):
                case XO(LODSQ):
                    //mov rax <- [rsi]
                    //add rsi, 8
                    emitLoad(instr, 0, uops, REG_RAX);
                    emitExecUop(REG_RSI, 0, REG_RSI, 0, uops, 1, PORTS_015);
                    break;
                case XO(MOVSB):
                case XO(MOVSW):
                case XO(MOVSD):
                case XO(MOVSQ):
                    //lodsX + stosX
                    emitLoad(instr, 0, uops, REG_RAX);
                    emitStore(instr, 0, uops, REG_RAX);
                    emitExecUop(REG_RSI, 0, REG_RSI, 0, uops, 1, PORTS_015);
                    emitExecUop(REG_RDI, 0, REG_RDI, 0, uops, 1, PORTS_015);
                    break;
                case XO(CMPSB):
                case XO(CMPSW):
                case XO(CMPSD):
                case XO(CMPSQ):
                    //load [rsi], [rdi], compare them, and add the other 2
                    //Agner's tables say all exec uops can go anywhere, but I'm betting the comp op only goes in port5
                    emitLoad(instr, 0, uops, REG_LOAD_TEMP);
                    emitLoad(instr, 0, uops, REG_LOAD_TEMP+1);
                    emitExecUop(REG_LOAD_TEMP, REG_LOAD_TEMP+1, REG_RFLAGS, 0, uops, 1, PORT_5);
                    emitExecUop(REG_RSI, 0, REG_RSI, 0, uops, 1, PORTS_015);
                    emitExecUop(REG_RDI, 0, REG_RDI, 0, uops, 1, PORTS_015);
                    break;
                default: //SCAS and other dragons I have not seen yet
                    inaccurate = true;
            }
            break;
        case XC(IOSTRINGOP):
            //TODO: These seem to make sense with REP, which Pin unfolds anyway. Are they used al all?
            break;

        /* Stuff not even the Intel guys know how to classify :P */
        case XC(MISC):
            if (opcode == XO(LEA)) {
                emitBasicOp(instr, uops, 1, PORT_1);
            } else if (opcode == XO(PAUSE)) {
                //Pause is weird. It takes 9 cycles, issues 5 uops (to be treated like a complex instruction and put a wrench on the decoder?),
                //and those uops are issued to PORT_015. No idea about how individual uops are sized, but in ubenchs I cannot put even an ADD
                //between pauses for free, so I'm assuming it's 9 solid cycles total.
                emitExecUop(0, 0, 0, 0, uops, 9, PORTS_015, 8); //9, longest first
                emitExecUop(0, 0, 0, 0, uops, 5, PORTS_015, 4); //NOTE: latency does not matter
                emitExecUop(0, 0, 0, 0, uops, 5, PORTS_015, 4);
                emitExecUop(0, 0, 0, 0, uops, 4, PORTS_015, 3);
                emitExecUop(0, 0, 0, 0, uops, 4, PORTS_015, 3);
            }
            /*switch (opcode) {
                case CPUID:
                case ENTER:
                case LEAVE:
                case LEA:
                case LFENCE:
                case MFENCE:
                case SFENCE:
                case MONITOR:
                case MWAIT:
                case UD2:
                case XLAT:
            }*/
            //TODO
            break;

        default: {}
            //panic("Invalid instruction category");
    }

    //Try to produce something approximate...
    if (uops.size() - initialUops == isLocked? 1 : 0) { //if it's locked, we have the initial fence for an empty instr
        emitBasicOp(instr, uops, 1, PORTS_015, 0, false /* don't report unhandled cases */);
        inaccurate = true;
    }

    //NOTE: REP instructions are unrolled by PIN, so they are accurately simulated (they are treated as predicated in Pin)
    //See section "Optimizing Instrumentation of REP Prefixed Instructions" on the Pin manual

    //Add ld/st fence to all locked instructions
    if (isLocked) {
        //inaccurate = true; //this is now fairly accurate
        emitFence(uops, 9); //locked ops introduce an additional uop and cache locking takes 14 cycles/instr per the perf counters; latencies match with 9 cycles of fence latency
    }

    assert(uops.size() - initialUops < MAX_UOPS_PER_INSTR);
    //assert_msg(uops.size() - initialUops < MAX_UOPS_PER_INSTR, "%ld -> %ld uops", initialUops, uops.size());
    return inaccurate;
}

// See Agner Fog's uarch doc, macro-op fusion for Core 2 / Nehalem
bool Decoder::canFuse(INS ins) {
    xed_iclass_enum_t opcode = (xed_iclass_enum_t) INS_Opcode(ins);
    if (!(opcode == XO(CMP) || opcode == XO(TEST))) return false;
    //Discard if immediate
    for (uint32_t op = 0; op < INS_OperandCount(ins); op++) if (INS_OperandIsImmediate(ins, op)) return false;

    //OK so far, let's check the branch
    INS nextIns = INS_Next(ins);
    if (!INS_Valid(nextIns)) return false;
    xed_iclass_enum_t nextOpcode = (xed_iclass_enum_t) INS_Opcode(nextIns);
    xed_category_enum_t nextCategory = (xed_category_enum_t) INS_Category(nextIns);
    if (nextCategory != XC(COND_BR)) return false;
    if (!INS_IsDirectBranch(nextIns)) return false; //according to PIN's API, this s only true for PC-rel near branches

    switch (nextOpcode) {
        case XO(JZ):  //or JZ
        case XO(JNZ): //or JNE
        case XO(JB):
        case XO(JBE):
        case XO(JNBE): //or JA
        case XO(JNB):  //or JAE
        case XO(JL):
        case XO(JLE):
        case XO(JNLE): //or JG
        case XO(JNL):  //or JGE
            return true;
        case XO(JO):
        case XO(JNO):
        case XO(JP):
        case XO(JNP):
        case XO(JS):
        case XO(JNS):
            return opcode == XO(TEST); //CMP cannot fuse with these
        default:
            return false; //other instrs like LOOP don't fuse
    }
}

bool Decoder::decodeFusedInstrs(INS ins, DynUopVec& uops) {
    //assert(canFuse(ins)); //this better be true :)

    Instr instr(ins);
    Instr branch(INS_Next(ins));

    //instr should have 2 inputs (regs/mem), and 1 output (rflags), and branch should have 2 inputs (rip, rflags) and 1 output (rip)

    if (instr.numOutRegs != 1  || instr.outRegs[0] != REG_RFLAGS ||
        branch.numOutRegs != 1 || branch.outRegs[0] != REG_RIP)
    {
        reportUnhandledCase(instr, "decodeFusedInstrs");
        reportUnhandledCase(branch, "decodeFusedInstrs");
    } else {
        instr.outRegs[1] = REG_RIP;
        instr.numOutRegs++;
    }

    emitBasicOp(instr, uops, 1, PORT_5);
    return false; //accurate
}


#ifdef BBL_PROFILING

//All is static for now...
#define MAX_BBLS (1<<24) //16M

static lock_t bblIdxLock = 0;
static uint64_t bblIdx = 0;

static uint64_t bblCount[MAX_BBLS];
static std::vector<uint32_t>* bblApproxOpcodes[MAX_BBLS];

#endif
#if 1
BblInfo* Decoder::decodeBbl(BBL bbl, INS start, INS end) {
    uint32_t instrs = 0;
   // uint32_t bytes = BBL_Size(bbl); // not used
    BblInfo* bblInfo;

    //Decode BBL
    uint32_t approxInstrs = 0;
    uint32_t curIns = 0;
    DynUopVec uopVec;


    //Decode
    for (INS ins = start; INS_Valid(ins) && ins!=end; ins = INS_Next(ins)) {
        instrs++;
        if (INS_IsXchg(ins) && INS_OperandReg(ins, 0) == REG_RCX && INS_OperandReg(ins, 1) == REG_RCX ){
            DynUop uop;
            uop.clear();
            uop.type = UOP_MAGIC_OP;
            uop.portMask = PORT_2; // Same as Memory
            uopVec.push_back(uop);
        } else if (INS_IsXchg(ins) && INS_OperandReg(ins, 0) == REG_RDX && INS_OperandReg(ins, 1) == REG_RDX){
            DynUop uop;
            uop.clear();
            uop.type = UOP_DEQUEUE;
            uop.portMask = PORT_2;
            uopVec.push_back(uop);
        } else {
            bool inaccurate = false;
            inaccurate = Decoder::decodeInstr(ins, uopVec);
            curIns++;
            if (inaccurate) {
                approxInstrs++;
            }
        }
    }
    // Following Not necessaruly true since Xchg is ommitted
    //assert(curIns == instrs);



    //Allocate
    uint32_t objBytes = offsetof(BblInfo, oooBbl) + DynBbl::bytes(uopVec.size());
    bblInfo = static_cast<BblInfo*>(malloc(objBytes));  // can't use type-safe interface

    //Initialize ooo part
    DynBbl& dynBbl = bblInfo->oooBbl[0];
    dynBbl.addr = BBL_Address(bbl);
    dynBbl.uops = uopVec.size();
    dynBbl.approxInstrs = approxInstrs;
    for (uint32_t i = 0; i < dynBbl.uops; i++) dynBbl.uop[i] = uopVec[i];


    //Initialize generic part
    bblInfo->instrs = instrs;
    bblInfo->addr = INS_Address(start);

    return bblInfo;
}
#endif


