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

#include "sim/memory/memory_hierarchy.h"

#include "sim/sim.h"
#include "sim/timestamp.h"
#include "sim/assert.h"

static const char* accessTypeNames[] = {"GETS", "GETX", "PUTS", "PUTX"};
static const char* invTypeNames[] = {"INV", "INVX"};
static const char* mesiStateNames[] = {"I", "S", "E", "M"};

const char* AccessTypeName(AccessType t) {
    assert_msg(t >= 0 && (size_t)t < sizeof(accessTypeNames)/sizeof(const char*), "AccessTypeName got an out-of-range input, %d", t);
    return accessTypeNames[t];
}

const char* InvTypeName(InvType t) {
    assert_msg(t >= 0 && (size_t)t < sizeof(invTypeNames)/sizeof(const char*), "InvTypeName got an out-of-range input, %d", t);
    return invTypeNames[t];
}

const char* MESIStateName(MESIState s) {
    assert_msg(s >= 0 && (size_t)s < sizeof(mesiStateNames)/sizeof(const char*), "MESIStateName got an out-of-range input, %d", s);
    return mesiStateNames[s];
}

std::ostream& MemReq::operator<<(std::ostream& os) const {
    os  << "["
        << AccessTypeName(type)
        << " addr=0x" << std::hex << lineAddr << std::dec
        << " state=" << MESIStateName(state)
        << " childId=" << childId
        << " srcId=" << srcId
        << " flags=" << flags
        << " ts=" << (ts ? ts->toString() : "none")
        << "]";
    return os;
}

std::string MemReq::toString() const {
    std::ostringstream buffer;
    this->operator<<(buffer);
    return buffer.str();
}

std::ostream& MemResp::operator<<(std::ostream& os) const {
    os  << "["
        << "newState=" << MESIStateName(newState)
        << " lastAccTS=" << (timestamped() ? lastAccTS->toString() : "none")
        << "]";
    return os;
}

std::string MemResp::toString() const {
    std::ostringstream buffer;
    this->operator<<(buffer);
    return buffer.str();
}

std::ostream& InvReq::operator<<(std::ostream& os) const {
    os  << "["
        << InvTypeName(type)
        << " addr=0x" << std::hex << lineAddr << std::dec
        << " src=" << srcId
        << " ts=" << (ts ? ts->toString() : "none")
        << " skipChildId=" << skipChildId
        << "]";
    return os;
}

std::string InvReq::toString() const {
    std::ostringstream buffer;
    this->operator<<(buffer);
    return buffer.str();
}

std::ostream& InvResp::operator<<(std::ostream& os) const {
    os  << "["
        << "flags=" << flags
        << " lastAccTS=" << (timestamped() ? lastAccTS->toString() : "none")
        << "]";
    return os;
}

std::string InvResp::toString() const {
    std::ostringstream buffer;
    this->operator<<(buffer);
    return buffer.str();
}

/* Message size methods, used by the network */

static uint32_t tsbytes(const TimeStamp* ts) {
    // 16 bytes for a full VT,
    // 8 bytes for a VT with no tiebreaker, or
    // 0 bytes for an irrevocable VT.
    // Although one bit would be sent to indicate "this is a nonspeculative
    // request", we assume that 8 bytes for an address is overkill anyway.
    return (!ts || *ts == IRREVOCABLE_TS) ? 0 : (ts->hasTieBreaker() ? 16 : 8);
}

uint32_t MemReq::bytes() const {
    bool dirty = (type == PUTX) || ((flags | GETS_WB) && state == M);
    return 8 // address
            + (dirty? ossinfo->lineSize : 0)
            + tsbytes(ts);
}

uint32_t MemResp::bytes(const MemReq& req) const {
    bool data = carriesData(req);
    // [mha21] since PREFETCH memreqs don't touch tcc, we can't expect that the MemResp.newstate
    // is correctly updated
    assert(!data || newState != I || req.is(MemReq::Flag::PREFETCH));
    return  // In the absense of data, it's a simple ACK, no address needed
            // (modeling as a 2-byte payload; no addr needed, this is a response)
            (data ? ossinfo->lineSize : 2)
            // TODO(mcj) one bit would be sent to indicate "this is a
            // nonspeculative request", but can it be packed in elsewhere?
            + tsbytes(lastAccTS);
}

bool MemResp::carriesData(const MemReq& req) const {
    return IsGet(req.type) && (req.state == I);
}

uint32_t InvReq::bytes() const {
    return 8 /*address*/ + tsbytes(ts);
}

uint32_t InvResp::bytes(const InvReq&) const {
    return  // In the absense of data, it's a simple ACK, no address needed.
            (carriesData() ? ossinfo->lineSize : 2)
            + tsbytes(lastAccTS);
}

bool InvResp::carriesData() const {
    // FIXME(mcj) I assume when this was first written, it intended to
    // categorize the bytes for the entire message as either "data" or "other".
    // Unfortunately these responses now mix (data, timestamp, address, etc.)
    // A more robust solution would query the message:
    // "how much of your payload was type X and type Y"?
    // The following code will barely be correct anymore.
    // [ssub] Yes, we would ideally want to categorize bytes transferred
    // by the type (data, timestamp, or other). The network, however, accounts
    // at the granularity of flits. For now, this method does what the name
    // suggest viz. whether this message carries data or not.
    // Messages that carry data may or may not carry timestamps. While the
    // bandwidth for the timestamps is correctly accounted for, we do not
    // differentiate (ie. categorize) it in the network.
    return is(WRITEBACK);
}
/* Static asserts */

#include <type_traits>

static inline void CompileTimeAsserts() {
    // [mcj] {Mem,Inv}Reqs with timestamps are not POD. Is this okay?
    //static_assert(std::is_pod<MemReq>::value, "MemReq not POD!");
    //static_assert(std::is_pod<MemResp>::value, "MemResp not POD!");
    //static_assert(std::is_pod<InvReq>::value, "InvReq not POD!");
    //static_assert(std::is_pod<InvResp>::value, "InvResp not POD!");
    // dsm: Not anynmore, as fractal time adds fields the implementation wouldn't have
    //static_assert(sizeof(TimeStamp) == (128 / 8), "Expect 128-bit Timestamps");
}
