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

#include "sim/memory/memory_hierarchy.h"

class AbortReq;
class AbortAck;
class TaskEnqueueReq;
class TaskEnqueueResp;
class CutTieMsg;
class GVTUpdateMsg;
class LVTUpdateMsg;

// [ssub] Partial template specialization to aid nested templated class
// AtomicPort<T,R> figure out type of message being sent.
template<typename T>
struct Helper {
    static uint32_t typeToIdx(T& req) {
        assert_msg(false, "Need a valid helper!");
        return 0;
    }
};

template<typename T, typename R>
struct HelperResp {
    static uint32_t typeToIdx(T& req, R& resp) {
        assert_msg(false, "Need a valid helper!");
        return 0;
    }
};


/* Atomic Ports */
template<>
struct Helper<const MemReq> {
    static uint32_t typeToIdx(const MemReq& req) {
        return asUInt(req.type);
    }
};

template<>
struct Helper<const InvReq> {
    static uint32_t typeToIdx(const InvReq& req) {
        return uint32_t(AccessType::NUMMEMREQTYPES) + asUInt(req.type);
    }
};

template<>
struct HelperResp<const MemReq, MemResp> {
    static uint32_t typeToIdx(const MemReq& req, MemResp& resp) {
        uint32_t idx = resp.carriesData(req) ? 0 : 1;
        return uint32_t(AccessType::NUMMEMREQTYPES) +
               uint32_t(InvType::NUMINVREQTYPES) + idx;
    }
};

template<>
struct HelperResp<const InvReq, InvResp> {
    static uint32_t typeToIdx(const InvReq& req, InvResp& resp) {
        uint32_t idx = resp.carriesData() ? 0 : 1;
        return uint32_t(AccessType::NUMMEMREQTYPES) +
               uint32_t(InvType::NUMINVREQTYPES)
               + idx;
    }
};

template<>
struct Helper<const AbortReq> {
    static uint32_t typeToIdx(const AbortReq& req) {
        return uint32_t(AccessType::NUMMEMREQTYPES) +
               uint32_t(InvType::NUMINVREQTYPES) +
               2 /*MemResp types*/;
    }
};

template<>
struct HelperResp<const AbortReq, AbortAck> {
    static uint32_t typeToIdx(const AbortReq& req, AbortAck& ack) {
        return uint32_t(AccessType::NUMMEMREQTYPES) +
               uint32_t(InvType::NUMINVREQTYPES) +
               2 /*MemResp types*/ +
               1 /*AbortReq*/;
    }
};

/* Non-atomic Ports */
template<>
struct Helper<TaskEnqueueReq> {
    static uint32_t typeToIdx(const TaskEnqueueReq& req) {
        return uint32_t(AccessType::NUMMEMREQTYPES) +
               uint32_t(InvType::NUMINVREQTYPES) +
               2 /*MemResp types*/ +
               1 /*AbortReq*/ +
               1 /*AbortRes*/;
    }
};

template<>
struct Helper<TaskEnqueueResp> {
    static uint32_t typeToIdx(const TaskEnqueueResp& req) {
        return uint32_t(AccessType::NUMMEMREQTYPES) +
               uint32_t(InvType::NUMINVREQTYPES) +
               2 /*MemResp types*/ +
               1 /*AbortReq*/ +
               1 /*AbortRes*/ +
               1 /*TaskEnqReq*/;
    }
};

template<>
struct Helper<CutTieMsg> {
    static uint32_t typeToIdx(const CutTieMsg& msg) {
        return uint32_t(AccessType::NUMMEMREQTYPES) +
               uint32_t(InvType::NUMINVREQTYPES) +
               2 /*MemResp types*/ +
               1 /*AbortReq*/ +
               1 /*AbortRes*/ +
               1 /*TaskEnqReq*/ +
               1 /*TaskEnqResp*/;
    }
};

template<>
struct Helper<GVTUpdateMsg> {
    static uint32_t typeToIdx(const GVTUpdateMsg& msg) {
        return uint32_t(AccessType::NUMMEMREQTYPES) +
               uint32_t(InvType::NUMINVREQTYPES) +
               2 /*MemResp types*/ +
               1 /*AbortReq*/ +
               1 /*AbortRes*/ +
               1 /*TaskEnqReq*/ +
               1 /*TaskEnqResp*/ +
               1 /*CutTieMsg*/;
    }
};

template<>
struct Helper<LVTUpdateMsg> {
    static uint32_t typeToIdx(const LVTUpdateMsg& msg) {
        return uint32_t(AccessType::NUMMEMREQTYPES) +
               uint32_t(InvType::NUMINVREQTYPES) +
               2 /*MemResp types*/ +
               1 /*AbortReq*/ +
               1 /*AbortRes*/ +
               1 /*TaskEnqReq*/ +
               1 /*TaskEnqResp*/ +
               1 /*CutTieMsg*/ +
               1 /*GVTUpdateMsg*/;
    }
};
