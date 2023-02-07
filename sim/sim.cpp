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

#include "sim/sim.h"

#include <cstddef>
#include <cstdlib>
#include <exception>
#include <random>
#include <signal.h>
#include <string>
#include <sys/prctl.h>

#include "swarm/hooks.h"
#include "swarm/impl/limits.h"
#include "pin/pin.H"
#include "sim/alloc/alloc.h"
#include "sim/assert.h"
#include "sim/init/init.h"
#include "sim/conflicts/abort_handler.h"
#include "sim/conflicts/unstallers.h"
#include "sim/core/core.h"
#include "sim/core/sbcore.h"
#include "sim/driver.h"
#include "sim/ff_queue.h"
#include "sim/task_mapper.h"
#include "sim/log.h"
#include "sim/memory/memory_hierarchy.h"
#include "sim/memory/memory_partition.h"
#include "sim/rob.h"
#include "sim/spin_cv.h"
#include "sim/stack.h"
#include "sim/stats/stats.h"
#include "sim/task.h"
#include "sim/taskprofiler.h"
#include "sim/thread.h"
#include "sim/timestamp.h"
#include "sim/trace.h"
#include "sim/tsb.h"
#include "sim/types.h"
#include "sim/watchdog.h"
#include "spin.h"  // from libspin
#include "virt/virt.h"

#undef DEBUG
#define DEBUG(args...) //info(args)

#undef DEBUG_EVENTQ
#define DEBUG_EVENTQ(args...) //info(args)

static KNOB<string> KnobConfigFile(KNOB_MODE_WRITEONCE, "pintool", "config", "", "config file name");

/* Data structures */

const GlobSimInfo* ossinfo = nullptr;

enum SwitchCallType { SwitchCall_MAGIC_OP, SwitchCall_DEQUEUE_OP, SwitchCall_MEM_ACCESS, SwitchCall_BBL_BOUNDARY, SwitchCall_COND_BRANCH};
enum MemAccessType {TYPE_L, TYPE_S, TYPE_LL, TYPE_LS, TYPE_LLS}; // combinations of loads and stores

// Used to implement the barrier magic op
static std::vector<uint32_t> barrierWaiters;

static bool Serialize(const std::string& reason);

// FIXME: Disabled for now (pinStart == pinEnd) because the pin memory
// allocator is unstable and runs out of memory.
// NOTE: Keep in sync with harness
static const Address pinStart = 0x0d0000000000;
static const Address pinEnd = pinStart;  //0x0e0000000000;

enum class ROI { BEFORE, INSIDE, AFTER };
static ROI ROIState = ROI::BEFORE;
static bool swarmRun = false;
bool IsInSwarmROI() {
    return swarmRun && ROIState == ROI::INSIDE;
}

typedef void (*endHandler_t)();
static endHandler_t endHandler = nullptr;

static bool IsValidAddress(Address addr) {
    auto curThread = GetCurThread();
    if (!curThread || !curThread->task) return true;
    ThreadID addressOwner = ossinfo->stackTracker->owner(addr);
    if (addressOwner == curThread->tid) return true;  // our own stack
    if (addressOwner != INVALID_TID) return false;  // other stack
    // Currently, only irrevocable and priv tasks can access untracked memory
    // TODO(dsm): May want read-only access in the future, this would require
    // refactoring address-checking code.
    if (isUntrackedAddress((void*)addr) && !curThread->task->isIrrevocable() && !curThread->isPriv())
        return false;
    // victory: If irrevocable NOTIMESTAMP tasks were to interact with
    // speculative tasks through tracked memory, maintianing atomicity would
    // require NOTIMESTAMP tasks to actually abort everything in their path
    // just like any other irrevocable task, and also lower their timestamp
    // to match any timestamped speculative task that they interact with.
    assert_msg(!isTrackedAddress((void*)addr) || !curThread->task->noTimestamp,
               "NOTIMESTAMP task %s accessed tracked address 0x%lx",
               curThread->task->toString().c_str(), addr);
    if (addr >= pinStart && addr < pinEnd) {
        warn("App tried to access pin/simulator memory 0x%lx", addr);
        return false;
    }
    return true;  // other
}

static spin::ThreadId HandleInvalidAddress(spin::ThreadId tid, ADDRINT memAddr) {
    // NOTE: may modify ctxt
    bool exception = Serialize("invalid address");
    if (exception) {
        DEBUG("HandleInvalidAddress: Speculative task tried to "
              "access invalid (stack?) address %p @ PC 0x%lx",
              (void*)memAddr, spin::getReg(spin::getContext(tid), REG::REG_RIP));
        filtered_info("HandleInvalidAddress: Marked exception on thread due to serialize() call");
        return NextThread();
    } else {
        uint32_t owner = ossinfo->stackTracker->owner(memAddr);
        panic("HandleInvalidAddress: Non-speculative task tried to "
              "access invalid address %p which belongs to thread %d's %s"
              "\n  Check whether you passed this address to this task.",
              (void*)memAddr,
              owner,
              memAddr > GetStackStart(owner) ? "thread-local storage" : "stack");
    }
}

static spin::ThreadId SwitchThreadCheckAddr1(spin::ThreadId tid, ADDRINT memAddr1) {
    if (likely(IsValidAddress(memAddr1))) return std::get<0>(SwitchThread(tid));
    else return HandleInvalidAddress(tid, memAddr1);
}

static spin::ThreadId SwitchThreadCheckAddr2(spin::ThreadId tid, ADDRINT memAddr1, ADDRINT memAddr2) {
    if (likely(IsValidAddress(memAddr1) && IsValidAddress(memAddr2))) return std::get<0>(SwitchThread(tid));
    else return HandleInvalidAddress(tid,
                                     !IsValidAddress(memAddr1) ? memAddr1 :
                                     memAddr2);
}

static spin::ThreadId SwitchThreadCheckAddr3(spin::ThreadId tid, ADDRINT memAddr1, ADDRINT memAddr2, ADDRINT memAddr3) {
    if (likely(IsValidAddress(memAddr1) && IsValidAddress(memAddr2) && IsValidAddress(memAddr3))) return std::get<0>(SwitchThread(tid));
    else return HandleInvalidAddress(tid,
                                     !IsValidAddress(memAddr1) ? memAddr1 :
                                     !IsValidAddress(memAddr2) ? memAddr2 :
                                     memAddr3);
}

// FIXME(dsm): Hacky, though temporary while we sort out unstall protocol
SPinConditionVariable stallers;

// Set of tasks that have asserted shouldStall() in the current access
static std::set<Task*> stallSrcs;
void registerShouldStall(Task& t) { stallSrcs.insert(&t); }

// [mcj] The following are wrappers around Core::load and Core::store that soon
// will permit the host thread to block until an abort event is resolved.
template <typename F>
spin::ThreadId __RecordMemoryAccess(spin::ThreadId tid, Address addr,
                                    uint32_t size, F accFcn) {
    auto curThread = GetCurThread();
    assert(curThread && curThread->core);
    assert(curThread->tid == tid);
    assert(stallSrcs.empty());

    assert(size);
    if (addr + (size - 1) < addr) { // overflow.
        std::stringstream ss;
        ss << "access to 0x" << std::hex << addr
           << " of size " << std::dec << size
           << " wraps past end of address space @ PC 0x"
           << std::hex << spin::getReg(spin::getContext(tid), REG::REG_RIP);
        std::string reason = ss.str();
        bool exception = Serialize(reason);
        if (exception) return NextThread();
        panic("%s", reason.c_str());
    }

    bool acked = (curThread->core->*accFcn)(addr, size, tid);
    if (!acked) {
        DEBUG("Nacked");
        curThread->core->leave(Core::State::STALL_NACK, curThread->tid);
        BlockThreadAfterSwitch(BLOCKED);
        // TODO(mcj) this probably doesn't work with SBCore since the
        // current thread may have other instructions it can execute while the
        // memory access resolves.
        Task *curTask = GetCurTask();
        assert(curTask);
        assert(curTask->runningTid == tid);
        assert(!stallSrcs.empty());
        stallers.addWaiter(tid);

        auto stallCount = std::make_shared<uint64_t>(stallSrcs.size());
        for (Task* t : stallSrcs) {
            if (curTask->isIrrevocable())
                t->registerObserver(std::make_unique<IrrevocableUnstaller>(
                    *t, *curTask, stallCount));
            else
                t->registerObserver(
                    // TODO(dsm): UnstallOnFalse = false for Always... this is
                    // correct though it will re-issue some accs, and we don't
                    // use stall = Always anyhow.
                    std::make_unique<Unstaller<true>>(*t, *curTask, stallCount));
        }
        stallSrcs.clear();

        spin::loop();
        return NextThread();
    } else {
        assert(stallSrcs.empty());
        return tid;
    }
}

spin::ThreadId RecordLoad1(spin::ThreadId tid, Address addr, uint32_t size) {
    return __RecordMemoryAccess(tid, addr, size, &Core::load);
}

spin::ThreadId RecordLoad2(spin::ThreadId tid, Address addr, uint32_t size) {
    return __RecordMemoryAccess(tid, addr, size, &Core::load2);
}

spin::ThreadId RecordStore(spin::ThreadId tid, Address addr, uint32_t size) {
    return __RecordMemoryAccess(tid, addr, size, &Core::store);
}

/* Syscall side-effect routines */
void ReadRange(uint32_t tid, char* buf, size_t bytes) {
    if (IsInFastForward()) return;
    uint32_t accSize = 4; // bytes
    uint64_t lineMask = ~((uint64_t)ossinfo->lineSize - 1);
    Address startAddr = ((Address) buf) & lineMask;
    Address endAddr = (((Address) buf) + bytes - accSize) & lineMask;
    for (Address addr = startAddr; addr <= endAddr; addr += ossinfo->lineSize) {
        __RecordMemoryAccess(tid, addr, accSize, &Core::load);
    }
}

void WriteRange(uint32_t tid, char* buf, size_t bytes) {
    if (IsInFastForward()) return;
    uint32_t accSize = 4; // bytes
    uint64_t lineMask = ~((uint64_t)ossinfo->lineSize - 1);
    Address startAddr = ((Address) buf) & lineMask;
    Address endAddr = (((Address) buf) + bytes - accSize) & lineMask;
    for (Address addr = startAddr; addr <= endAddr; addr += ossinfo->lineSize) {
        __RecordMemoryAccess(tid, addr, accSize, &Core::store);
    }
}

/* Heartbeats */

static uint64_t heartbeats = 0;
uint64_t getHeartbeats() { return heartbeats; }
static void incHeartbeats(const Task* task = nullptr) {
    heartbeats++;
    if (heartbeats == ossinfo->maxHeartbeats) {
        info("Max heartbeats reached, terminating simulation");
        if (ossinfo->ffHeartbeats && IsInFastForward() && ROIState == ROI::INSIDE) {
            assert(ossinfo->ffHeartbeats < heartbeats);
            info("Reached both ffHeartbeats and maxHeartbeats within a single task,"
                 " so no simulation recorded");
            assert(task);
            info("The task was: %s", task->toString().c_str());
        }
        SimEnd();
    }
}

// Tasks must increment heartbeats only at commit
class HeartbeatsProfiler : public TaskObserver {
  private:
    Task& task;
  public:
    HeartbeatsProfiler(Task& t) : task(t) {}
    void commit() override { incHeartbeats(&task); }
    void abort(bool requeue) override {
        task.unregisterObserver(this);
    }
};

/* ROB interface routines, including sleeping on an empty ROB */

static ROB& GetCurRob() {
    auto curThread = GetCurThread();
    assert(curThread);
    assert(curThread->core);
    return *ossinfo->robs[getROBIdx(curThread->core->getCid())];
}

static TSB* GetTsb(ThreadState* thread) {
    assert(thread && thread->core);
    return ossinfo->tsbs[getROBIdx(thread->core->getCid())];
}

static TSB* GetCurTsb() { return GetTsb(GetCurThread()); }

/* Callbacks */

// [victory] This should never be called unless the application itself crashes,
// as signals generated by misspeculation should be handled in InterceptSignal.
// NOTE (dsm): This is OK with spin (and we can use normal PIN contexts) b/c the thread is uncaptured at this point
void ContextChange(THREADID tid, CONTEXT_CHANGE_REASON reason, const CONTEXT* from, CONTEXT* to, int32_t sig, void* v) {
    const char* reasonStr = "?";
    switch (reason) {
        case CONTEXT_CHANGE_REASON_FATALSIGNAL:
            reasonStr = "FATAL_SIGNAL";
            break;
        case CONTEXT_CHANGE_REASON_SIGNAL:
            reasonStr = "SIGNAL";
            break;
        case CONTEXT_CHANGE_REASON_SIGRETURN:
            reasonStr = "SIGRETURN";
            break;
        case CONTEXT_CHANGE_REASON_APC:
            reasonStr = "APC";
            break;
        case CONTEXT_CHANGE_REASON_EXCEPTION:
            reasonStr = "EXCEPTION";
            break;
        case CONTEXT_CHANGE_REASON_CALLBACK:
            reasonStr = "CALLBACK";
            break;
    }

    // While panic() will print out a recent %rip value saved by libspin from
    // some previous switchpoint, this PIN_GetContextReg() call might get a
    // more recent %rip value, so maybe it's worth printing both?
    void* pc = (void*) PIN_GetContextReg(from, REG::REG_RIP);
    panic("[%d] ContextChange, reason %s, signal num %d, PC %p", tid, reasonStr, sig, pc);
}

static bool Serialize(const std::string& reason) {
    auto curThread = GetCurThread();
    if (!curThread) return false;
    TaskPtr task = curThread->task;
    if (!task) return false;
    if (task->isIrrevocable()) {
        // The exception-causing irrevocable task can't be pending an abort.
        assert(!task->hasPendingAbort());
        return false;
    }

    DEBUG("Marking exception on speculative task %s",
          task->toString().c_str());
    assert(!task->exceptionReason.size());
    task->exceptionReason = reason;

    if (curThread->isPriv()) {
        warn("Serialize() called in priv mode, reason: %s", reason.c_str());
    }

    bool addedAbort = ossinfo->abortHandler->abort(
        task, GetCurRob().getIdx(), Task::AbortType::EXCEPTION,
        getCurThreadLocalCycle() + 1, -1, nullptr);
    assert(addedAbort);
    assert(curThread->state == BLOCKED_TOABORT || curThread->isPrivDoomed());
    return true;
}

/* Signal/syscall handling */

static std::array<std::string, MAX_THREADS> pendingSerializeReasons;

static uint64_t exceptionHandler;

bool SuppressSignal(THREADID tid, INT32 sig, CONTEXT *ctxt, BOOL hasHandler, const EXCEPTION_INFO *pExceptInfo, VOID *v) {
    return false;  // squash the signal
}

bool InterceptSignal(THREADID tid, INT32 sig, CONTEXT *ctxt, BOOL hasHandler, const EXCEPTION_INFO *pExceptInfo, VOID *v) {
    if (matchAny(sig, SIGHUP, SIGINT, SIGQUIT, SIGPIPE, SIGTERM)) {
        // Asynchronous termination signals should cause immediate termination.
        warn("Terminating simulation due to received signal %d", sig);
        info("Task profile up to cycle %ld:\n", getCurCycle());
        printTaskProfilingInfo();
        StopWatchdogThread();
        TickWatchdog();
        std::_Exit(128 + sig);
    }

    // We assume we are handling a synchronous exception from the application.
    assert_msg(matchAny(sig,
                        SIGILL,
                        SIGTRAP,
                        SIGABRT,
                        SIGBUS,
                        SIGFPE,
                        SIGSEGV,
                        SIGSTKFLT,
                        SIGSYS),
               "Received unexpected signal num %d, hasHandler %d", sig, hasHandler);

    auto curThread = GetCurThread();
    if (curThread && curThread->task && !curThread->task->isIrrevocable()) {
        assert(curThread->tid < pendingSerializeReasons.size());
        assert(sig);
        if (pendingSerializeReasons[curThread->tid] != "") {
            warn("[signal] Clobbering serialize reason for thread %d: %s",
                 curThread->tid,
                 pendingSerializeReasons[curThread->tid].c_str());
        }
        std::stringstream ss;
        ss << "received signal " << sig << " @ PC 0x" << std::hex << PIN_GetContextReg(ctxt, REG::REG_RIP);
        pendingSerializeReasons[curThread->tid] = ss.str();
        // Jump to exception handler, which will call Serialize()
        // (we can't call Serialize from within here)
        // NOTE: Only place where we touch an actual PIN context. spin is transparent here (it's like we're in a syscall)
        PIN_SetContextReg(ctxt, REG::REG_RSP, curThread->rspCheckpoint);
        PIN_SetContextReg(ctxt, REG::REG_RIP, exceptionHandler);
        return false;  // squash
    } else {
        return true;  // forward signal to application
    }
}

bool HandleSyscall(spin::ThreadId tid, spin::ThreadContext* ctxt) {
    DEBUG("[%d] Taking syscall", tid);
    auto curThread = GetCurThread();
    assert(curThread && tid == curThread->tid);
    Task* task = GetCurTask();
    if (!curThread->isPriv() && task && !task->isIrrevocable()) {
        assert(tid < pendingSerializeReasons.size());
        if (pendingSerializeReasons[tid] != "") {
            warn("[syscall] Clobbering serialize reason for thread %d: %s",
                 tid, pendingSerializeReasons[tid].c_str());
        }

        task->setSpeculativeSyscall();
        // Mimicking virt.cpp's syscallEnter
        uint64_t number = spin::getReg(ctxt, REG::REG_RAX);
        pendingSerializeReasons[tid] = "syscall #" + std::to_string(number);
        // Jump to exception handler, which will call Serialize()
        // (spin handles this correctly)
        spin::setReg(ctxt, REG::REG_RSP, curThread->rspCheckpoint);
        assert(spin::getReg(ctxt, REG::REG_RIP) != exceptionHandler);
        spin::setReg(ctxt, REG::REG_RIP, exceptionHandler);
        return false;  // irrelevant whether we allow uncaptures, b/c we're not
                       // taking the syscall anyway
    } else {
        // May do pre-patching of system calls, may disallow uncaptures
        bool allowUncapture = virt::syscallEnter(tid, ctxt);

        // When taking syscalls on multithreaded cores, we may risk libspin thinking
        // we're the only runnable thread (because nonActiveThreads blocks at the
        // libspin level), and therefore will not uncapture. But we want this
        // uncapture, because it'll yield one of these runnable nonActiveThreads
        // and let us take this syscall asynchronously.
        if (SbCore* sbcore = dynamic_cast<SbCore*>(curThread->core))
            sbcore->unblockWaiter();

        return allowUncapture;
    }
}

void BlockAbortingTaskOnThread(uint32_t tid) {
    ThreadState* hostThread = GetThreadState(tid);
    assert(!hostThread->isPriv());
    assert(hostThread->task);

    DEBUG("Block aborting task on thread %d", tid);

    // The thread may be stalled on a full TSB
    GetTsb(hostThread)->wakeup(tid);
    // Or on a stalled memory request
    stallers.notify(tid);
    if (hostThread->state == BLOCKED) {
        // 1. Unblock threads waiting on zoom
        hostThread->task->container()->notifyZoomRequesterOfAbort(hostThread->task, tid);

        // 2. Unblock threads blocked on context switch
        // [maleen] If still blocked, then blocked due to ctx switch
        if (hostThread->state == BLOCKED) {
            assert(ossinfo->isScoreboard);
            hostThread->core->unblockWaiter(tid);
        }
    }
    assert(matchAny(hostThread->state, QUEUED, RUNNING));

    // Block to a fixed state, to simplify AbortTaskOnThread
    hostThread->core->leave(Core::State::EXEC_TOABORT, tid);
    BlockThread(hostThread->tid, BLOCKED_TOABORT);
    assert(hostThread->state == BLOCKED_TOABORT);
}

void AbortTaskOnThread(uint32_t tid) {
    ThreadState* hostThread = GetThreadState(tid);
    DEBUG("[%d] [%ld] AbortTaskOnThread, ts %s, state %d", tid, getCurCycle(),
          hostThread->task->ts.toString().c_str(), hostThread->state);

    assert(hostThread->task);
    //assert(hostThread->state == BLOCKED_TOABORT);

    // Clear serialization reason (this task was waiting for a serialize, but
    // it got aborted first)
    pendingSerializeReasons[tid] = "";

    uint64_t abortCycle = std::max(getCurCycle(), hostThread->core->getCycle(tid))
                          + hostThread->core->pipelineFlushPenalty();
    hostThread->core->abortTask(abortCycle, tid, hostThread->isPriv());
    hostThread->task = nullptr;

    if (hostThread->state == BLOCKED_TOABORT) {
        assert(!hostThread->isPriv());
        UnblockThread(hostThread->tid, abortCycle);

        // Restore checkpoint
        spin::setReg(spin::getContext(tid), REG::REG_RIP, hostThread->abortPc);
        spin::setReg(spin::getContext(tid), REG::REG_RSP, hostThread->rspCheckpoint);
    } else {
        assert(hostThread->isPriv());
        assert(hostThread->isPrivDoomed());
    }
}

/* Data for disk access emulation */
// Latencies are for a 1TB Samsung 960 PRO NVMe SSD, in processor cycles,
// assuming 2GHz clock (TODO: Make configurable?).
// For 4KB IOPS: reads 72 us / 440000 max IOPS; writes 20 us / 360000 max IOPS.
const uint32_t disk_blockSize = 4096;
const uint32_t disk_rdLat = 144000;
const uint32_t disk_rdRepRate = 4545;
const uint32_t disk_wrLat = 40000;
const uint32_t disk_wrRepRate = 5556;

uint64_t disk_firstAvailCycle = 0;

/* Magic ops (TODO: Make composable/move out to another unit) */
spin::ThreadId HandleDeepenMagicOp(spin::ThreadId tid, spin::ThreadContext* ctxt) {
    uint64_t maxTS = spin::getReg(ctxt, REG::REG_RDI);
    assert_msg(maxTS == __MAX_TS || ossinfo->maxFrameDepth == UINT32_MAX,
               "Frame spills may not work correctly with DEEPEN's maxTs.");
    auto curThread = GetCurThread();
    TaskPtr t = curThread->task;
    if (!t) panic("DEEPEN called outside task!");
    assert(t->hasContainer() != IsInFastForward());
    if (t->hasContainer()) {
        bool success = t->container()->deepen(t, tid, maxTS);
        if (!success) {
            curThread->core->leave(Core::State::STALL_RESOURCE, curThread->tid);
            BlockThreadAfterSwitch(BLOCKED);
            spin::loop();
            return NextThread();
        }
    } else {
        // Fast-forwarding, just deepen without consulting ROB
        t->deepen(maxTS);
    }
    return tid;
}

spin::ThreadId HandleSetGvtMagicOp(spin::ThreadId tid, uint64_t cycle, spin::ThreadContext* ctxt) {
    assert(GetCurTask() && !GetCurTask()->noTimestamp);
    bool exception = Serialize("set gvt from non-gvt task");
    auto curThread = GetCurThread();
    if (exception) {
        filtered_info("SetGVT: Marked exception on thread due to serialize() call");
        return NextThread();
    } else { // this is the gvt task, so we can alter the gvt
        Task* task = GetCurTask();
        assert(task);
        uint64_t loweredTs = spin::getReg(ctxt, REG::REG_RDI);
        assert(loweredTs <= task->ts.app());
        TimeStamp gvtToSet = task->ts.getSameDomain(loweredTs);

        if (IsInFastForward()) {
            task->ts = gvtToSet;
            // deepen() requires tiebreakers, so assign them
            GetCurRob().assignTieBreaker(task->ts);
        } else if (!curThread->setGvtInProgress) {
            // Request SET_GVT and block until SET_GVT has completed
            curThread->setGvtInProgress = true;
            GetCurRob().setGvt(gvtToSet, tid, cycle);
            curThread->core->leave(Core::State::STALL_RESOURCE, curThread->tid);
            BlockThreadAfterSwitch(BLOCKED);
            spin::loop();
            return NextThread();
        }

        // At this point, SET_GVT has completed
        assert(task->ts.app() == loweredTs);
        curThread->setGvtInProgress = false;
    }

    return tid;
}

static void DispatchTaskToContext(const Task& task, spin::ThreadContext* ctxt) {
    if (task.softTs) spin::setReg(ctxt, REG::REG_RDI, task.softTs);
    else spin::setReg(ctxt, REG::REG_RDI, task.ts.app());

    const uint32_t numArgs = task.args.size();
    constexpr REG regs[] = {REG::REG_RSI, REG::REG_RDX, REG::REG_RCX,
                            REG::REG_R8, REG::REG_R9};
    constexpr uint32_t MAX_REGS = sizeof(regs)/sizeof(regs[0]);
    assert(numArgs <= MAX_REGS);
    for (uint32_t i = 0; i < numArgs; i++)
        spin::setReg(ctxt, regs[i], task.args[i]);

    /* dsm: Stack layout details
     *
     * The dequeue_task magic op has semantics similar to a "call"
     * instruction (push %rip + jmp) because most runner wrappers are have
     * tail calls (the last thing they do is call another function), and
     * not putting anything after that call (like a finish_task magic op)
     * enables them to avoid mucking with rsp and turn calls into jmps. In
     * other words, if we leave the normal return sequence, we get lower
     * overheads.
     *
     * To complicate things, the dequeue loop is a leaf function, because
     * all gcc sees is an innocent xchg instruction and it has no idea that
     * we're going to make it behave like a call. This means we need to be
     * careful with the red zone and manually adjust alignment.
     *
     * See Sec 3.2.2 of the AMD64 ABI for details on call sequence, rsp
     * alignment, and redzone http://www.x86-64.org/documentation/abi.pdf
     *
     * We have the following constraints:
     *  1. xchg %%rdx, %%rdx needs to behave like a call
     *  2. The function it appears on may have an active 128-byte redzone
     *  3. rsp is most likely not aligned
     *  4. ABI requires that, on a control transfer:
     *    - rsp+8 must be 16 or 32-byte aligned
     *    - rsp points to the return address
     *  5. In our case, all runner functions (where we're xfering control
     *     to) always have zero args passed through the stack
     *  6. It might be a good idea to leave a bit of space beyond the
     *     redzone, in case the task goes bananas and starts smashing the
     *     stack before it's killed.
     */
    auto curThread = GetCurThread();
    uint64_t origRsp = curThread->rspCheckpoint;
    uint64_t rsp = origRsp - 512;  // leave a 512-byte gap
    rsp = (rsp >> ossinfo->lineBits) << ossinfo->lineBits;  // align to a cache line

    // Push the return address
    rsp -= sizeof(uint64_t);
    uint64_t retAddr = curThread->finishPc;
    auto written = PIN_SafeCopy(reinterpret_cast<void*>(rsp), &retAddr, sizeof(uint64_t));
    (void) written;
    assert(written == sizeof(uint64_t));

    // Save rsp
    spin::setReg(ctxt, REG::REG_RSP, rsp);

    DEBUG("[%d] dequeue_task rsp: %lx -> %lx (offset %ld) (retAddr %lx) newrip: %lx",
            curThread->tid, origRsp, rsp, rsp-origRsp, retAddr, task.taskFn);

    spin::setReg(ctxt, REG::REG_RIP, task.taskFn);
}

static uint64_t simMallocCommit = 0;
static uint64_t simMallocAbort = 0;
static uint64_t simFreeCommit = 0;
static uint64_t simFreeAbort = 0;
uint64_t getSimMallocCommits() { return simMallocCommit; }
uint64_t getSimMallocAborts() { return simMallocAbort; }
uint64_t getSimFreeCommits() { return simFreeCommit; }
uint64_t getSimFreeAborts() { return simFreeAbort; }

void AddPenalty(uint64_t cycles) {
    auto curThread = GetCurThread();
    if (!IsInFastForward() && curThread && curThread->core) {
        curThread->core->bbl(cycles);
    }
}
#define ALLOCATION_PENALTY (30)
void AddAllocationPenalty() { AddPenalty(ALLOCATION_PENALTY); }

/* Fast-forwarding thread handling */

// While fast-forwarding, we run all tasks on thread 0 and block all others
static std::unordered_set<spin::ThreadId> ffBlockedThreads;
void UnblockFfBlockedThreads() {
    assert(getCurCycle() == 0);
    for (auto tid : ffBlockedThreads) UnblockThread(tid, 0);
    ffBlockedThreads.clear();
}

// While fast-forwarding, thread 0 dequeues tasks from the ffTaskQueue and
// executes them irrevocably. We avoid running through the usual task FSM and
// leave tasks IDLE all the way. This is because (1) the usual FSM transition
// methods assume invariants that don't hold while fast-forwarding (e.g.,
// having a ROB for RUNNING tasks), and (2) we don't want to pollute stats.
spin::ThreadId HandleFfDequeueMagicOp(spin::ThreadId tid, spin::ThreadContext* ctxt) {
    assert(IsInFastForward());
    auto curThread = GetCurThread();
    assert(curThread);
    assert(tid == curThread->tid);

    if (tid != 0 && !IsFfQueueEmpty()) {
        DEBUG("Blocking tid %d on FF dequeue", tid);
        assert(!ffBlockedThreads.count(tid));
        ffBlockedThreads.insert(tid);
        curThread->core->leave(Core::State::IDLE, curThread->tid);
        BlockThreadAfterSwitch(BLOCKED);
        // In theory NextThread() should not return us. Just in case it does,
        // spin::loop ensures we repeat this call and fail the assertion above.
        spin::loop();
        return NextThread();
    }

    if (curThread->task) {
        assert(!curThread->task->hasPendingAbort());
        curThread->task->ffFinish();
        curThread->task = nullptr;
    } else if (!curThread->rspCheckpoint) {
        // Checkpoint rsp
        curThread->rspCheckpoint = spin::getReg(ctxt, REG::REG_RSP);
    }

    if (IsFfQueueEmpty()) {
        // Restore rsp from checkpoint
        spin::setReg(ctxt, REG::REG_RSP, curThread->rspCheckpoint);
        curThread->rspCheckpoint = 0l;  // next dequeue loop will take a new checkpoint

        spin::setReg(ctxt, REG::REG_RIP, curThread->donePc);

        if (tid == 0) UnblockFfBlockedThreads();
        return tid;
    }

    assert(tid == 0);  // other threads don't get past the empty check

    if (getHeartbeats() >= ossinfo->ffHeartbeats) {
        assert(ROIState == ROI::INSIDE);
        info("Exiting fast-forward (ffHeartbeats reached with tasks to run)");
        DrainFfTaskQueue();
        UnblockFfBlockedThreads();
        ExitFastForward();

        PIN_RemoveInstrumentation();
        // Set PC to ensure correct behavior; see comment on other PIN_RemoveInstrumentation call
        uintptr_t nextPc = spin::getReg(ctxt, REG::REG_RIP);
        spin::setReg(ctxt, REG::REG_RIP, nextPc);

        return tid;
    }

    // Dequeue next ff task
    TaskPtr task = DequeueFfTask();

    // deepen() requires tiebreakers, so assign them
    GetCurRob().assignTieBreaker(task->ts);
    task->ffStart(tid);

    DispatchTaskToContext(*task, ctxt);  // changes PC
    DEBUG("ffDequeue: %s", task->toString().c_str());

    //curThread->task = task;
    std::swap(task, curThread->task);  // Avoids copying shared_ptr

    return tid;
}

spin::ThreadId HandleDequeueMagicOp(spin::ThreadId tid, spin::ThreadContext* ctxt) {
    assert(!IsInFastForward());
    auto curThread = GetCurThread();
    assert(curThread);
    assert(tid == curThread->tid);
    if (ossinfo->numThreadsPerCore > 1 && curThread->task.get()) {
        // Wait until mem accesses from running task has completed conflict
        // detection.  Maybe not the most elegant, but will do for now.
        //TODO(victory): What is the reasoning behind this? It was added
        // to the simulator without any justifying comments or message.
        curThread->core->waitTillMemComplete(curThread->tid);
    }

    // Attempt to switch before handling dequeue
    spin::ThreadId nextTid;
    bool mustReturn;
    std::tie(nextTid, mustReturn) = SwitchThread(tid);
    if (mustReturn) {
        return nextTid;
    }
    assert(curThread->core);
    assert(curThread->core->getCycle(tid) == getCurCycle());

    if (curThread->task) {
        assert(!curThread->task->hasPendingAbort());
        assert(curThread->tid == curThread->task->runningTid);
        assert(curThread->state != BLOCKED);
        curThread->core->finishTask(curThread->tid);
        GetCurRob().finishTask(curThread->task);
        curThread->task = nullptr;
    } else if (!curThread->rspCheckpoint) {
        // Checkpoint rsp
        curThread->rspCheckpoint = spin::getReg(ctxt, REG::REG_RSP);
    }

    if (GetCurRob().terminate()) {
        assert(GetCurRob().taskQueueSize() == 0);
        // Restore rsp from checkpoint
        spin::setReg(ctxt, REG::REG_RSP, curThread->rspCheckpoint);
        curThread->rspCheckpoint = 0l;  // next dequeue loop will take a new checkpoint

        spin::setReg(ctxt, REG::REG_RIP, curThread->donePc);
        return tid;
    }

    // Try to dequeue a task
    TaskPtr task;
    rob::Stall reason;
    std::tie(task, reason) = GetCurRob().dequeueTask(curThread->tid);
    if (!task) {
        assert(!GetCurRob().terminate());
        // ROB has no tasks or no space, block (ROB will unblock us)
        curThread->core->leave(Core::stallToState(reason), curThread->tid);
        BlockThreadAfterSwitch(BLOCKED);
        // NextThread() drives time forward and may unblock us, so
        // call loop() to reexecute switchcall unconditionally
        spin::loop();
        return NextThread();
    }

    assert(curThread->tid == tid);
    assert(task);
    assert(!curThread->task);
    curThread->task = task;
    curThread->core->startTask(task,curThread->tid);
    // Corner case: we dequeued a task that will eventually abort, so block
    if (task->hasPendingAbort()) {
        DEBUG_EVENTQ("[%u] Dequeued task scheduled for abort %s", curThread->tid, task->toString().c_str());
        // FIXME(dsm): Why is this different from the case where we pause
        // a core running a task that is later aborted?
        curThread->core->leave(Core::State::EXEC_TOABORT, curThread->tid);
        BlockThreadAfterSwitch(BLOCKED_TOABORT);
        spin::loop();
        return NextThread();
    }

    // Load task arguments, etc.
    DEBUG_EVENTQ("[HD] [%d] Dequeued task %s", curThread->tid, task->toString().c_str());
    assert(!task->hasPendingAbort());

    DispatchTaskToContext(*task, ctxt);  // changes PC
    return tid;
}

spin::ThreadId HandleEnqueueMagicOp(const uint64_t op,
                                    spin::ThreadId tid,
                                    const uint64_t cycle,
                                    spin::ThreadContext* ctxt) {
    const auto curThread = GetCurThread();
    assert(curThread && curThread->core);
    const TaskPtr parent = curThread->task;  // may be null

    // Flags
    const bool noHint = op & EnqFlags::NOHINT;
    const bool sameHint = op & EnqFlags::SAMEHINT;
    const bool noHash = op & EnqFlags::NOHASH ||
            (sameHint && parent && parent->noHashHint);
    const bool sameTask = op & EnqFlags::SAMETASK;
    const bool yieldIfFull = op & EnqFlags::YIELDIFFULL;
    const bool useParentDomain = op & EnqFlags::PARENTDOMAIN;
    const bool producer = op & EnqFlags::PRODUCER;
    const bool requeuer = op & EnqFlags::REQUEUER;
    const bool maySpec = op & EnqFlags::MAYSPEC;
    const bool cantSpec = op & EnqFlags::CANTSPEC;
    const bool isSoftPrio = op & EnqFlags::ISSOFTPRIO || ossinfo->relaxed;
    const bool runOnAbort = op & EnqFlags::RUNONABORT;
    const bool noTimestamp = op & EnqFlags::NOTIMESTAMP || runOnAbort;
    const bool nonSerialHint = op & EnqFlags::NONSERIALHINT;

    assert_msg(!(op & EnqFlags::SAMETIME), "The SAMETIME flag is deprecated");

    // sameTask and any of the three are allowed together
    // noHash and sameHint should be allowed together, but
    // sameHint implies the same value of noHash as before, for convenience
    // The following combinations are exclusive
    assert(noHint + noHash <= 1);
    assert(noHint + sameHint <= 1);
    assert_msg(noTimestamp || !parent || !parent->noTimestamp,
               "NOTIMESTAMP task enqueuing a timestamped child");
    // Abort handlers are always unordered and nonspeculative.
    assert(!runOnAbort || cantSpec);
    assert(!runOnAbort || noTimestamp);

    const uint32_t numArgs = op & 0x0f;
    assert(numArgs <= SIM_MAX_ENQUEUE_REGS);

    constexpr REG regs[] = {REG::REG_RDI, REG::REG_RSI, REG::REG_RDX,
                            REG::REG_R8,  REG::REG_R9,  REG::REG_R10,
                            REG::REG_R11, REG::REG_R12};
    constexpr uint32_t MAX_TOTAL_REGS = sizeof(regs)/sizeof(regs[0]);
    uint32_t curReg = 0;

    // Get timestamp
    const uint64_t tsApp = noTimestamp ? 0 : spin::getReg(ctxt, regs[curReg++]);
    if (tsApp == __MAX_TS) {
        bool exception = Serialize("UINT64_MAX is an illegal enqueue timestamp");
        assert(exception);
        return NextThread();
    }
    const uint64_t softTs = isSoftPrio ? tsApp : 0;

    // Get args
    std::vector<uint64_t> args;
    for (uint32_t i = 0; i < numArgs; i++) {
        args.push_back(spin::getReg(ctxt, regs[curReg]));
        curReg++;
    }

    // Additional optional args: taskFn and hint
    uint64_t taskFn = 0;
    if (sameTask) {
        if (!parent) panic("Can't use SAMETASK on initial tasks");
        taskFn = parent->taskFn;
    } else {
        taskFn = spin::getReg(ctxt, regs[curReg++]);
    }
    assert(taskFn);

    // TODO [hrlee] Add some checks for overlap of soft priority with frame requeuer.
    const bool frameRequeuer = taskFn == Task::FRAME_REQUEUER_PTR;
    assert(!frameRequeuer || (parent && parent->ts.domainDepth()));
    assert(!frameRequeuer || cantSpec);
    assert(!frameRequeuer || parent->isFrameSpiller() || parent->isRequeuer());
    const TimeStamp ts = frameRequeuer && parent->isFrameRequeuer() ? parent->ts.getUpLevels(1).domainMax()
                       : frameRequeuer ? parent->ts.domainMax()
                       : noTimestamp ? NO_TS
                       : isSoftPrio ? TimeStamp(0)
                       : parent ? parent->ts.childTS(tsApp, useParentDomain)
                       : TimeStamp(tsApp);
    assert(ts < INFINITY_TS);

    // Get optional args: hint
    const bool skipHint = sameHint || noHint;
    uint64_t hint = skipHint ? 0 : spin::getReg(ctxt, regs[curReg++]);

    assert(curReg <= MAX_TOTAL_REGS);

    // victory: Right now we don't have a strong use case for speculative
    // NOTIMESTAMP tasks. In the future, we might implement speculative
    // NOTIMESTAMP tasks that provide TM-like semantics, or something.
    const bool ordinaryRequeuer = taskFn == Task::ORDINARY_REQUEUER_PTR;
    assert(!noTimestamp || cantSpec || ordinaryRequeuer)

    if (runOnAbort && !curThread->isPriv() && parent && !parent->isIrrevocable()) {
        bool exception = Serialize("RUNONABORT only available to priv/irrevocable tasks");
        assert(exception);
        return NextThread();
    }

    if (runOnAbort && !curThread->isPrivDoomed() && (!parent || parent->isIrrevocable())) {
        // This task will never abort, so skip the enqueue
        return tid;
    }

    const bool toSuperdomain = parent && parent->ts.domainDepth() > ts.domainDepth();
    assert(toSuperdomain == (parent && !parent->isDeepened() && useParentDomain));
    if (toSuperdomain) {
        if (parent->hasContainer()
                && parent->container()->requestZoomOutIfNeeded(parent, tid)) {
            const std::string reason = "enqueue to superdomain must wait on zoom-out";
            bool exception = Serialize(reason);
            if (exception) {
                filtered_info("Marked exception on thread %d: %s", tid, reason.c_str());
                return NextThread();
            }
            // We are executing non-speculatively, but we still need to wait
            // for frame expansion to complete. So block thread.
            // Both ordinary tasks and frame requeuers can end up here.
            curThread->core->leave(Core::State::STALL_RESOURCE,
                                   curThread->tid);
            BlockThreadAfterSwitch(BLOCKED);
            spin::loop();
            return NextThread();
        }
    }

    // Doomed priv code should not be enqueueing anything other than abort handlers
    // NOTE(dsm): If this assertion fires, handle it. This doesn't currently
    // happen in our users (commit handlers are always launched from non-priv
    // code), but I want to avoid implementing a particular action (noop?
    // enqueue untied only if some other flag?) until there's a clear use case.
    assert(!(curThread->isPrivDoomed() && !runOnAbort));

    // A requeuer should not yield if it hasn't enqueued anything
    const bool yield = yieldIfFull && parent &&
                 (parent->numEnqueues >= ossinfo->tasksPerRequeuer);

    TSB* tsb = GetCurTsb();
    uint32_t cid = curThread->core->getCid();
    bool isSpillerChild = parent && parent->isSpiller();
    bool isTied = parent && !parent->isIrrevocable();

    // TODO(dsm): This is likely unnecessary given the current implementation of aborts, but it's safe
    if (runOnAbort && curThread->isPrivDoomed()) isTied = false;

    assert(!isSpillerChild || !isTied);
    if (!IsInFastForward() && (yield || !tsb->reserve(tid, cid, isSpillerChild, isTied))) {
        //if (IsInFastForward()) panic("Can't enqueue > tsbEntries tasks at initialization");

        if (yieldIfFull) {
            // Yield the task (which finishes and requeues it) and dequeue another
            assert(parent && parent->isRequeuer());
            assert(curThread->tid == parent->runningTid);
            assert(curThread->state != BLOCKED);
            curThread->core->finishTask(curThread->tid);
	    if (ossinfo->relaxed) parent->softTs = tsApp;
            GetCurRob().yieldTask(parent,
                    // Advance the requeuer's timestamp if its next minimum
                    // child is timestamped, otherwise be conservative and reuse
                    // the requeuer's old timestamp.
                    // N.B. if an ordinary requeuer is yielding while enqueuing
                    // a frame requeuer, we are really using the tsApp value.
                    noTimestamp || isSoftPrio ? parent->ts.app() : tsApp);
            curThread->task = nullptr;

            assert(curThread->rspCheckpoint);
            spin::setReg(ctxt, REG::REG_RSP, curThread->rspCheckpoint);
            spin::setReg(ctxt, REG::REG_RIP, curThread->finishPc);
            return HandleDequeueMagicOp(tid, ctxt);
        } else {
            tsb->block(tid);
            curThread->core->leave(Core::State::STALL_RESOURCE, curThread->tid);
            BlockThreadAfterSwitch(BLOCKED);

            // If this is a parent and we have no tiebreaker yet, become
            // dependent with ourselves. This allows us to become the GVT task,
            // which would unblock us.
            if (parent) GetCurRob().notifyDependence(parent, parent->cts());

            spin::loop();
            return NextThread();
        }
    }

    // This is not OK in our current API...
    assert_msg(IsInFastForward() || parent || curThread->isPriv(),
               "Ordinary non-task code inside the ROI must not spawn tasks.");

    // Adjust hint based on flags & find location
    if (noHint) {
        static std::mt19937 gen(0);
        static std::uniform_int_distribution<uint64_t> randomHintDistribution(0, UINT64_MAX);
        hint = randomHintDistribution(gen);
    } else if (sameHint) {
        if (!parent) {
            if (!runOnAbort) panic("Can't use enqueue_same on initial tasks");
            assert(curThread->isPrivDoomed());
            hint = curThread->getPrivTaskHint();
        } else {
            assert(noHash == parent->noHashHint);
            hint = parent->hint;
        }
    }

    // dsm: Nicer panic than below for programs that violate the timestamp requirements
    if (parent && ts < parent->ts) {
        std::string reason = "Invalid enqueue timestamp by " +
                             parent->toString() + ": parent ts " +
                             parent->ts.toString() + " > child ts " +
                             ts.toString();
        bool exception = Serialize(reason);
        if (exception) return NextThread();
        panic("%s", reason.c_str());
    }

    if (parent && parent->children.size() == swarm::max_children + ossinfo->extraChildren) {
        bool exception = Serialize("max children reached");
        if (exception) return NextThread();
        panic("Max children (%ld) reached by task %s, child funcPtr 0x%lx"
              "\n  Try to create lower-fanout task trees "
              "(e.g., use swarm::enqueue_all or SCC's loop expansion)",
              parent->children.size(),
              parent->toString().c_str(), taskFn);
    }

    if (maySpec && cantSpec) {
        bool exception = Serialize("MAYSPEC and CANTSPEC are both set");
        if (exception) return NextThread();
        panic("Invalid enqueue flags");
    }

    DEBUG("[%d] [%ld] Creating task: ptr %lx hint %ld flags %d%d%d%d%d",
          curThread ? curThread->tid : -1, cycle,
          taskFn, hint, noHint, noHash, sameHint, sameTask, useParentDomain);

    // The enqueuer should be enqueuing future work relative to its local time
    assert(IsInFastForward() || (ts >= GetCurRob().lvt()));

    RunCondition runCond = cantSpec ? RunCondition::CANTSPEC
                         : maySpec ? ossinfo->mayspecMode
                         : RunCondition::MUSTSPEC;

    if (toSuperdomain) parent->markEnqueuedChildToSuperdomain();

    TaskPtr t = std::make_shared<Task>(
        taskFn, ts, hint, noHash,
        nonSerialHint || /*for backward compatibility:*/ (ordinaryRequeuer || frameRequeuer),
        producer,
        requeuer || /*for backward compatibility:*/ (ordinaryRequeuer || frameRequeuer),
        softTs, runCond, runOnAbort, args);
    if (isTied) t->tie(parent);

    if (IsInFastForward()) {
        EnqueueFfTask(std::move(t));
    } else {
        trace::taskCreated(t.get(), parent ? parent.get() : nullptr);
        if (parent) {
            if (!parent->isIrrevocable()) {
                GetCurRob().notifyDependence(parent, t->lts());
            }
            parent->numEnqueues++;
        }

        // Find the ROB to send this task to:
        uint32_t robIdx = ossinfo->taskMapper->getDstROBIdx(
            GetCurRob().getIdx(), parent.get(), hint, noHash);
        DEBUG("[%d] [%ld] Enqueuing task with timestamp %s to ROB %d",
              curThread ? curThread->tid : -1, cycle,
              ts.toString().c_str(), robIdx);

        bool holdTask = (runOnAbort || t->cantSpeculate()) && isTied;
        // Note that we pass the parent TaskPtr here even though the child can
        // be untied, as it remains important to know if the parent is a spiller.
        tsb->enqueue(t, robIdx, cid, parent, holdTask, cycle);
    }

    // NOTE(dsm): After side effects, we MUST return the same thread (or we'll
    // have multiple enqueues)
    return tid;
}

spin::ThreadId HandleMagicOp(spin::ThreadId tid, spin::ThreadContext* ctxt) {
    // Attempt to switch thread before every switch point
    // Magic ops such as enqueues should happen only at curCycle
    DEBUG("MagicOp %ld tid: %d",spin::getReg(ctxt, REG::REG_RCX),tid);
    spin::ThreadId nextTid;
    bool mustReturn;
    std::tie(nextTid, mustReturn) = SwitchThread(tid);
    if (mustReturn) {
        return nextTid;
    }
    auto curThread = GetCurThread();
    assert(curThread && curThread->core);
    assert(curThread->core->getCycle(tid) == getCurCycle());
    const uint64_t cycle = getCurCycle();

    // dsm: Every time we set a live register, we must change the PC to force
    // the trace to end. Otherwise spin-slow fails because it doesn't capture
    // the updated register. Our convention is to jump to the instruction
    // directly following the magic op, using the lambda below.
    auto setPCToNextInstr = [ctxt] () {
        uintptr_t nextPc = spin::getReg(ctxt, REG::REG_RIP) + 3;  // magic op instr is 3 bytes
        spin::setReg(ctxt, REG::REG_RIP, nextPc);
    };

    uint64_t op = spin::getReg(ctxt, REG::REG_RCX);
    if (curThread && curThread->core) curThread->core->handleMagicOp(op,curThread->tid);
    if (op >= MAGIC_OP_TASK_ENQUEUE_BEGIN && op < MAGIC_OP_TASK_ENQUEUE_END) {
        return HandleEnqueueMagicOp(op, tid, cycle, ctxt);
    } else {
        switch (op) {
            case MAGIC_OP_ROI_BEGIN:
            case MAGIC_OP_ROI_END: {
                if (op == MAGIC_OP_ROI_BEGIN) {
                    DEBUG("Begin ROI");
                    assert(ROIState == ROI::BEFORE);
                    ROIState = ROI::INSIDE;
                    if (getHeartbeats() >= ossinfo->ffHeartbeats) {
                        DrainFfTaskQueue();
                        ExitFastForward();
                    } else {
                        info("Starting ROI fast-forwarded (ffHeartbeats not reached)");
                    }
                } else {
                    DEBUG("End ROI");
                    assert(ROIState == ROI::INSIDE);
                    ROIState = ROI::AFTER;
                    if (!IsInFastForward()) {
                        EnterFastForward();
                    } else {
                        assert(getHeartbeats() < ossinfo->ffHeartbeats);
                        info("Leaving ROI fast-forwarded (ffHeartbeats still not reached)");
                    }
                }
                PIN_RemoveInstrumentation();
                // HACK(dsm): Change the PC (to the next instruction) to force
                // the current trace to end, so that RemoveInstrumentation takes
                // effect immediately. Without this code, spin-fast works
                // (because the trace ends anyway), but spin-slow fails, because
                // it keeps going through the trace and invokes a switchcall,
                // which fails a fast-forwarding assertion.
                //
                // A clean fix for this would require non-trivial changes in
                // libspin, and this is the only place where
                // RemoveInstrumentation is used, so for now I'll let it be.
                setPCToNextInstr();
            } break;
            case MAGIC_OP_REGISTER_END_HANDLER: {
                assert_msg(!endHandler,
                           "TODO: support registering multiple sim end handlers");
                endHandler =
                    reinterpret_cast<endHandler_t>(spin::getReg(ctxt, REG::REG_RDI));
                setPCToNextInstr();
            } break;
            case MAGIC_OP_IN_FF: {
                spin::setReg(ctxt, REG::REG_RCX, IsInFastForward());
                setPCToNextInstr();
            } break;
            case MAGIC_OP_HEARTBEAT: {
                assert(curThread);
                Task* t = GetCurTask();
                if (t && !t->isIrrevocable()) {
                    t->registerObserver(
                        std::make_unique<HeartbeatsProfiler>(*t));
                } else {
                    incHeartbeats(t);
                    // If there are tasks queued or running,
                    // we exit fast-forward only on the next dequeue.
                    // However, we can exit fast-forward immediately
                    // if running a non-Swarm competition app.
                    if (getHeartbeats() == ossinfo->ffHeartbeats
                            && ROIState == ROI::INSIDE
                            && !t
                            && IsFfQueueEmpty()) {
                        info("Exiting fast-forward (ffHeartbeats reached with no tasks queued)");
                        UnblockFfBlockedThreads();
                        ExitFastForward();
                        PIN_RemoveInstrumentation();
                        // Set PC to ensure correct behavior; see comment on
                        // other PIN_RemoveInstrumentation call
                        uintptr_t nextPc = spin::getReg(ctxt, REG::REG_RIP);
                        spin::setReg(ctxt, REG::REG_RIP, nextPc);
                    }
                }
            } break;
            case MAGIC_OP_WRITE_STD_OUT:
                {
                    uint64_t strPtr = spin::getReg(ctxt, REG::REG_RDI);
                    info("[0x%lx][%d][%ld] %s",
                            (curThread && curThread->task)? curThread->task->taskFn : 0,
                            curThread ? curThread->tid : 0,
                            cycle,
                            reinterpret_cast<const char*>(strPtr));
                }
                break;
            case MAGIC_OP_BARRIER:
                {
                    const size_t threadsLeft =
                        ossinfo->numCores * ossinfo->numThreadsPerCore -
                        (barrierWaiters.size() + 1);
                    DEBUG("[%d] reached barrier. %d threads left", tid, threadsLeft);
                    if (threadsLeft) {
                        curThread->core->leave(Core::State::IDLE, curThread->tid);
                        BlockThreadAfterSwitch(BLOCKED);
                        barrierWaiters.push_back(tid);
                        return NextThread();
                    } else {
                        for (auto t : barrierWaiters) {
                            spin::ThreadContext* tc = spin::getContext(t);
                            spin::setReg(tc, REG::REG_RIP, spin::getReg(tc, REG::REG_RIP) + 3); // next instruction
                            UnblockThread(t, cycle);
                        }
                        // GetCurThread()->switchCallLooping = true; // prevent issueHead()
                        spin::loop();
                        barrierWaiters.clear();
                        DEBUG("[%d] Barrier ended", tid);
                        setPCToNextInstr();
                        return tid;
                    }
                }
                break;
            case MAGIC_OP_UPDATE_STACK:
                {
                    Address base = spin::getReg(ctxt, REG::REG_RDI);
                    ossinfo->stackTracker->setAddressRange(base, curThread->tid);
                }
                break;
            case MAGIC_OP_THREADS_AND_STACKS:
                {
                    uint32_t* pnthreads = reinterpret_cast<uint32_t*>(spin::getReg(ctxt, REG::REG_RDI));
                    void** pbase = reinterpret_cast<void**>(spin::getReg(ctxt, REG::REG_RSI));
                    uint32_t* plogStackSize = reinterpret_cast<uint32_t*>(spin::getReg(ctxt, REG::REG_RDX));
                    *pnthreads = ossinfo->numThreadsPerROB * ossinfo->numROBs;
                    *plogStackSize = ossinfo->logStackSize;
                    *pbase = ossinfo->stackTracker->base();
                }
                break;
            case MAGIC_OP_YIELD: break;
            case MAGIC_OP_TASK_DEQUEUE_SETUP:
                {
                    assert(curThread != nullptr);
                    assert(curThread->task == nullptr);
                    curThread->finishPc = spin::getReg(ctxt, REG::REG_RDI);
                    curThread->abortPc = spin::getReg(ctxt, REG::REG_RSI);
                    curThread->donePc = spin::getReg(ctxt, REG::REG_RDX);
                    DEBUG("[%d] dequeue_setup PCs: finish %p abort %p done %p", curThread->tid,
                            (void*)curThread->finishPc, (void*)curThread->abortPc, (void*)curThread->donePc);
                }
                break;
            case MAGIC_OP_TASK_REMOVE_UNTIED:
            case MAGIC_OP_TASK_REMOVE_OUT_OF_FRAME:
                {
                    const Task* spiller = GetCurTask();
                    assert(spiller);
                    ROB& rob = GetCurRob();
                    TaskPtr task;
                    if (op == MAGIC_OP_TASK_REMOVE_OUT_OF_FRAME) {
                        assert(spiller->isFrameSpiller());
                        task = rob.removeOutOfFrameTasks();
                    } else {
                        assert(spiller->isOrdinarySpiller());
                        assert(!spiller->noTimestamp ||
                               spiller->ts.domainDepth() == 0);
                        uint64_t curTS = spiller->ts.app();
                        uint64_t maxRemovableTS = spin::getReg(ctxt, REG::REG_RDI);
                        assert(curTS <= maxRemovableTS || maxRemovableTS == 0);

                        const bool canRemoveTimestamped = (
                                curTS <= maxRemovableTS && !spiller->noTimestamp);
                        task = nullptr;
                        if (canRemoveTimestamped) {
                            // Note that if curTS > ZERO_TS, then the ROB won't
                            // return any non-timestamped tasks
                            TimeStamp minTS = spiller->ts.getSameDomain(curTS, 0);
                            TimeStamp maxTS = (maxRemovableTS == UINT64_MAX) ?
                                    spiller->ts.getSameDomain(__MAX_TS, __MAX_TS) :
                                    spiller->ts.getSameDomain(maxRemovableTS + 1, 0);
                            // Request an untied task from the current task's
                            // housing ROB that precedes maxRemovableTS.
                            task = rob.removeUntiedTaskInRange(minTS, maxTS, false);
                            assert(!task || task->lts() < maxTS);
                        }
                        if (!task) {
                            // If we didn't find a task in the given bounds of the
                            // previous branch (i.e. higher or equal to the
                            // spiller's timestamp and lower than maxRemovableTS),
                            // try to remove a non-timestamped task. Note this is
                            // also the path for non-timestamped spillers, but
                            // won't safely mix Fractal with non-timestamped tasks.
                            task = rob.removeUntiedTaskInRange(
                                    ZERO_TS, TimeStamp(1, 0), true);
                            assert(!task || task->noTimestamp);
                        }
                    }
                    assert(!task || &rob == task->container());
                    if (task) {
                        DEBUG("Generic-spilled task %p, ts %lu", task->taskFn, task->ts.app());
                        assert(!task->isTied());

                        // Gather EnqFlags
                        uint64_t flags = 0;
                        // Flags to avoid:
                        //  - NOHINT (preserving it would cause requeuers to enqueue randomly)
                        //  - SAMEHINT SAMETASK (requeuers don't share the task's hint or task)
                        //  - PARENTDOMAIN YIELDIFFULL SUBDOMAIN SUPERDOMAIN (these are not task
                        //    properties, but are specific to the enqueue call)
                        //  - RUNONABORT (this would make the requeuer drop RunOnAbort tasks,
                        //    since the requeuer is irrevocable)
                        // Flags to retain:
                        if (task->noHashHint) flags |= EnqFlags::NOHASH;
                        if (task->isProducer()) flags |= EnqFlags::PRODUCER;
                        if (task->isRequeuer()) flags |= EnqFlags::REQUEUER;
                        if (task->maySpeculate()) flags |= EnqFlags::MAYSPEC;
                        if (task->cantSpeculate()) flags |= EnqFlags::CANTSPEC;
                        if (task->softTs) flags |= EnqFlags::ISSOFTPRIO;
                        if (task->noTimestamp) flags |= EnqFlags::NOTIMESTAMP;
                        if (task->nonSerialHint) flags |= EnqFlags::NONSERIALHINT;

                        // Pack the relevant EnqFlags into the top 16 bits of
                        // the address. The requeuer will queue them as-is.
                        // NOTE: Since x86-64 uses 48-bit VAddrs, this supports
                        // at most 16 flags. If you need more, reorder the
                        // flags to have the ones that requeuers need to retain
                        // in the lower-order bits.
                        assert((flags & 0x0fffful) == flags);
                        uint64_t taskFnAndFlags = (task->taskFn << 16) | flags;
                        // Ensure task can be reconstituted (since C/C++ arith/
                        // bitwise shift behavior is implementation-dependent,
                        // just emit a shift arithmetic right instruction,
                        // which preserves the sign bit)
                        uint64_t taskFn;
                        asm("sar $16,%%rcx;" : "=c"(taskFn) : "c"(taskFnAndFlags));
                        assert(taskFn == task->taskFn);

                        // Frame requeuers may be spilled by either another
                        // frame spiller or an ordinary spiller.  Either way,
                        // we tell the spiller the task has some arbitrary
                        // high-valued timestamp, and will take care to restore
                        // the frame requeuer's proper timestamp when it is
                        // re-enqueued.
                        assert(!task->isFrameRequeuer()
                               || (task->ts == task->ts.domainMax()
                                   && spiller->ts.domainDepth()
                                      == task->ts.domainDepth()
                                         + (spiller->isFrameSpiller())));
                        uint64_t enqTs = task->isFrameRequeuer() ? __MAX_TS - 1
                                       : task->softTs ? task->softTs
                                       : task->ts.app();
                        // UINT64_MAX has special semantic meaning in the
                        // spiller. If this restriction is problematic, we can
                        // rewrite the spiller accordingly.
                        assert(enqTs != UINT64_MAX);
                        spin::setReg(ctxt, REG::REG_RDI, enqTs);
                        spin::setReg(ctxt, REG::REG_RSI, taskFnAndFlags);
                        spin::setReg(ctxt, REG::REG_RDX, task->hint);

                        const uint32_t numArgs = task->args.size();
                        constexpr REG regs[] = {REG::REG_RCX, REG::REG_R8,
                                                REG::REG_R9, REG::REG_R10,
                                                REG::REG_R11};
                        constexpr uint32_t MAX_REGS =
                            sizeof(regs) / sizeof(regs[0]);
                        assert(numArgs <= MAX_REGS);
                        for (uint32_t i = 0; i < numArgs; i++)
                            spin::setReg(ctxt, regs[i], task->args[i]);

                        trace::taskSpilled(task.get(), spiller);
                        task->markUnreachable();  // allow it to be collected
                    } else {
                        spin::setReg(ctxt, REG::REG_RSI, 0ul);
                    }
                    setPCToNextInstr();
                }
                break;
            case MAGIC_OP_REMOVE_UNTIED_TASKFN:
                {
                    const Task* spiller = GetCurTask();
                    assert(spiller);
                    ROB& rob = GetCurRob();
                    assert(spiller->isSpiller());
                    assert(spiller->isCustomSpiller());
                    assert(!spiller->noTimestamp);
                    uint64_t curTS = spiller->ts.app();
                    uint64_t maxRemovableTS = spin::getReg(ctxt, REG::REG_RDI);
                    uint64_t spilleeFn = spin::getReg(ctxt, REG::REG_RSI);
                    assert(curTS <= maxRemovableTS);

                    // Note that if curTS > ZERO_TS, then the ROB won't
                    // return any non-timestamped tasks
                    TimeStamp minTS = spiller->ts.getSameDomain(curTS, 0);
                    TimeStamp maxTS = (maxRemovableTS == UINT64_MAX) ?
                            spiller->ts.getSameDomain(__MAX_TS, __MAX_TS) :
                            spiller->ts.getSameDomain(maxRemovableTS + 1, 0);
                    // Request an untied task from the current task's
                    // housing ROB that precedes maxRemovableTS.
                    TaskPtr task =
                        rob.removeUntiedTaskFnInRange(spilleeFn, minTS, maxTS);
                    assert(!task || task->lts() < maxTS);
                    assert(!task || &rob == task->container());
                    if (task) {
                        assert(!task->isTied());
                        uint64_t enqTs = task->ts.app();
                        DEBUG("Custom-spilled task %p, ts %lu", task->taskFn, enqTs);
                        assert_msg(enqTs != UINT64_MAX, "UINT64_MAX means no task");
                        spin::setReg(ctxt, REG::REG_RCX, enqTs);

                        const uint32_t numArgs = task->args.size();
                        constexpr REG regs[] = {REG::REG_RDI, REG::REG_RSI,
                                                REG::REG_RDX, REG::REG_R8,
                                                REG::REG_R9,  REG::REG_R10};
                        constexpr uint32_t MAX_REGS =
                            sizeof(regs) / sizeof(regs[0]);
                        assert(numArgs <= MAX_REGS);
                        for (uint32_t i = 0; i < numArgs; i++) {
                            DEBUG("  spilled task arg %p", task->args[i]);
                            spin::setReg(ctxt, regs[i], task->args[i]);
                        }

                        trace::taskSpilled(task.get(), spiller);
                        task->markUnreachable();  // allow it to be collected
                    } else {
                        spin::setReg(ctxt, REG::REG_RCX, UINT64_MAX);
                    }
                    setPCToNextInstr();
                }
                break;
            case MAGIC_OP_REGISTER_SPILLER: {
                    uint64_t spiller = spin::getReg(ctxt, REG::REG_RDI);
                    uint64_t spillee = spin::getReg(ctxt, REG::REG_RSI);
                    DEBUG("Registering spiller %p for spillee %p", spiller, spillee);
                    Task::customSpillers.push_back({spiller, spillee});
                }
                break;
            case MAGIC_OP_TASK_FRAMEHANDLER_ADDRS: {
                    assert(!Task::FRAME_SPILLER_PTR);
                    assert(!Task::FRAME_REQUEUER_PTR);
                    Task::FRAME_SPILLER_PTR = spin::getReg(ctxt, REG::REG_RDI);
                    Task::FRAME_REQUEUER_PTR = spin::getReg(ctxt, REG::REG_RSI);
                    DEBUG("swarm::frame_spiller at 0x%lx", Task::FRAME_SPILLER_PTR);
                    DEBUG("swarm::frame_requeuer at 0x%lx", Task::FRAME_REQUEUER_PTR);
                }
                break;
            case MAGIC_OP_TASK_HANDLER_ADDRS:
                {
                    // This magic op should be called once, to inform the
                    // architecture what are the function pointers of the
                    // generic and ordinary spiller and requeuer tasks.
                    assert_msg(
                        !(exceptionHandler || Task::ORDINARY_SPILLER_PTR ||
                          Task::ORDINARY_REQUEUER_PTR),
                        "Does your app call swarm::run() more than once?");
                    Task::ORDINARY_SPILLER_PTR = spin::getReg(ctxt, REG::REG_RDI);
                    Task::ORDINARY_REQUEUER_PTR = spin::getReg(ctxt, REG::REG_RSI);
                    exceptionHandler = spin::getReg(ctxt, REG::REG_RDX);
                    assert(Task::ORDINARY_SPILLER_PTR && Task::ORDINARY_REQUEUER_PTR);
                    assert(exceptionHandler);

                    DEBUG("swarm::spiller at 0x%lx", Task::ORDINARY_SPILLER_PTR);
                    DEBUG("swarm::requeuer at 0x%lx", Task::ORDINARY_SPILLER_PTR);
                    DEBUG("swarm::task_exception_handler at 0x%lx", exceptionHandler);

                    swarmRun = true;
                }
                break;
            case MAGIC_OP_SERIALIZE:
                {
                    std::string reason = pendingSerializeReasons[tid];
                    const bool isPendingSerialize = reason != "";
                    if (!isPendingSerialize) reason = "serialize magic op";
                    else pendingSerializeReasons[tid] = "";
                    bool exception = Serialize(reason);
                    if (exception) {
                        DEBUG("Marked exception on thread %d, reason: %s", tid, reason.c_str());
                        //curThread->switchCallLooping = true; // prevent issueHead()
                        return NextThread();
                    } else {
                        assert_msg(!isPendingSerialize,
                                   "Task %s became non-speculative while "
                                   "taking exception (reason: %s)",
                                   GetCurTask()->toString().c_str(),
                                   reason.c_str());
                    }
                }
                break;
            case MAGIC_OP_DEEPEN:
                {
                    return HandleDeepenMagicOp(tid, ctxt);
                }
                break;
            case MAGIC_OP_UNDEEPEN:
                {
                    Task* t = GetCurTask();
                    assert_msg(t, "UNDEEPEN called outside task!");
                    t->undeepen();
                }
                break;
            case MAGIC_OP_RDRAND:
                {
                    // To have all 1-core (or fast-forwarded) runs of any
                    // application produce deterministic results, regardless of
                    // how it was compiled with SCC and what tasks get random
                    // hints, swarm::rand64() needs its own private PRNG state.
                    static std::mt19937 gen(5489);
                    static std::uniform_int_distribution<uint64_t> dist(0, UINT64_MAX);

                    uint64_t* randPtr = reinterpret_cast<uint64_t*>(spin::getReg(ctxt, REG::REG_RDI));
                    *randPtr = dist(gen);
                }
                break;
            case MAGIC_OP_SET_GVT:
                {
                    return HandleSetGvtMagicOp(tid, cycle, ctxt);
                }
                break;
            case MAGIC_OP_CLEAR_READ_SET:
                {
                    assert(GetCurTask());
                    GetCurTask()->container_->clearReadSet(*GetCurTask());
                }
                break;
            case MAGIC_OP_RECORD_AS_ABORTED:
                {
                    assert(GetCurTask());
                    GetCurTask()->recordAsAborted = true;
                }
                break;
            case MAGIC_OP_ALLOC:
            {
                AddAllocationPenalty();

                class Observer : public TaskObserver {
                  private:
                    Task* task;
                  public:
                    Observer(Task* t) : task(t) {}
                    void commit() override { simMallocCommit++; }
                    void abort(bool requeue) override {
                        simMallocAbort++;
                        task->unregisterObserver(this);
                    }
                };

                if (!IsInFastForward()) {
                    assert(curThread);
                    Task* t= GetCurTask();
                    auto observer = std::make_unique<Observer>(t);
                    if (t && !t->isIrrevocable())
                        t->registerObserver(std::move(observer));
                    else
                        observer->commit();
                }
            }
            // FALL THROUGH
            case MAGIC_OP_ZERO_CYCLE_ALLOC:
                {
                    void** memptr = reinterpret_cast<void**>(
                                        spin::getReg(ctxt, REG::REG_RDI));
                    size_t bytes = spin::getReg(ctxt, REG::REG_RSI);
                    // With 8-byte/vertex data, this allows speculative
                    // allocation of vertex data arrays for up to 32 M nodes.
                    if (bytes > 256 << 20) {
                        std::stringstream ss;
                        ss << "allocation of " << std::dec << bytes
                           << " bytes is too large to execute speculatively";
                        std::string reason = ss.str();
                        bool exception = Serialize(reason);
                        if (exception) return NextThread();
                    }
                    *memptr = bytes? repl_malloc(bytes) : nullptr;
                }
                break;
            case MAGIC_OP_ZERO_CYCLE_UNTRACKED_ALLOC:
                {
                    // TODO(dsm): Could restrict allocation from the untracked
                    // pool to irrevocable tasks, but at this point we don't
                    // really care...
                    void** memptr = reinterpret_cast<void**>(
                                        spin::getReg(ctxt, REG::REG_RDI));
                    size_t bytes = spin::getReg(ctxt, REG::REG_RSI);
                    *memptr = bytes? repl_untracked_malloc(bytes) : nullptr;
                }
                break;
            case MAGIC_OP_POSIX_MEMALIGN:
                {
                    AddAllocationPenalty();
                    void** memptr = reinterpret_cast<void**>(
                                        spin::getReg(ctxt, REG::REG_RDI));
                    repl_posix_memalign(memptr, spin::getReg(ctxt, REG::REG_RSI),
                                        spin::getReg(ctxt, REG::REG_RDX));
                    // TODO posix_memalign should return a value, but for now in
                    // user space I assume that if memptr is not null, all went
                    // well.
                }
                break;
            case MAGIC_OP_FREE:
            {
                AddAllocationPenalty();

                class Observer : public TaskObserver {
                  private:
                    Task* task;
                  public:
                    Observer(Task* t) : task(t) {}
                    void commit() override { simFreeCommit++; }
                    void abort(bool requeue) override {
                        simFreeAbort++;
                        task->unregisterObserver(this);
                    }
                };

                if (!IsInFastForward()) {
                    assert(curThread);
                    Task *t = GetCurTask();
                    auto observer = std::make_unique<Observer>(t);
                    if (t && !t->isIrrevocable())
                        t->registerObserver(std::move(observer));
                    else
                        observer->commit();
                }
            }
            // FALL THROUGH
            case MAGIC_OP_ZERO_CYCLE_FREE:
                {
                    void* ptr = reinterpret_cast<void*>(
                                    spin::getReg(ctxt, REG::REG_RDI));
                    if (!isAllocAddress(ptr)) {
                        bool exception = Serialize("free() pointer not in heap");
                        assert_msg(exception,
                                   "irrevocable free of non-heap address %p",
                                   ptr);
                        return NextThread();
                    }
                    repl_free(ptr);
                }
                break;
            case MAGIC_OP_MALLOC_USABLE_SIZE:
                {
                    size_t* usableSizePtr = reinterpret_cast<size_t*>(spin::getReg(ctxt, REG::REG_RDI));
                    void* ptr = reinterpret_cast<void*>(spin::getReg(ctxt, REG::REG_RSI));
                    if (!isTrackedAddress(ptr)) {
                        bool exception = Serialize("malloc_usable_size() pointer"
                                                   " not in tracked heap");
                        assert(exception);
                        return NextThread();
                    }
                    *usableSizePtr = repl_malloc_usable_size(ptr);
                }
                break;
            case MAGIC_OP_GET_TIMESTAMP:
                {
                    const Task* t = GetCurTask();
                    if (t) {
                      // The timestamp is undefined for a nontimestamped task
                      assert(!t->noTimestamp);
                      spin::setReg(ctxt, REG::REG_RCX, t->ts.reportToApp());
                    } else {
                      // This tells the caller it is not in a task.
                      spin::setReg(ctxt, REG::REG_RCX, __MAX_TS);
                    }
                    setPCToNextInstr();
                }
                break;
            case MAGIC_OP_GET_TIMESTAMP_SUPER:
                {
                    const Task* t = GetCurTask();
                    assert_msg(t, "GET_TIMESTAMP_SUPER called outside task!");
                    spin::setReg(ctxt, REG::REG_RCX, t->ts.reportToAppSuper());
                    setPCToNextInstr();
                }
                break;
            case MAGIC_OP_PRIV_CALL:
                {
                    assert(curThread);
                    if (!IsInSwarmROI()) break;
                    const Task* task = GetCurTask();
                    assert(task);
                    size_t max = swarm::max_children + ossinfo->extraChildren;
                    if (task->children.size() == max) {
                        bool exception = Serialize("pre-priv max children");
                        assert_msg(exception,
                                   "[%d-%ld] Priv call will not be able to "
                                   "enqueue any more children (%ld) ",
                                   curThread->tid, cycle,
                                   task->children.size());
                        return NextThread();
                    }
                    DEBUG("[%d] Making priv call", curThread->tid);
                    curThread->privCall();
                }
                break;
            case MAGIC_OP_PRIV_RET:
                {
                    assert(curThread);
                    if (!IsInSwarmROI()) break;
                    DEBUG("[%d] Making priv ret", curThread->tid);
                    if (curThread->privRet()) {
                        DEBUG("[%d] Exiting speculative priv mode", curThread->tid);
                        // Privileged mode has finished and we were abortable.
                        // We may be in one of two states that
                        // require corrective action:
                        if (!curThread->task) {
                            // An abort blew our task. Go dequeue a new one.
                            GetCurRob().finishDoomedExecution(curThread->tid);
                            DEBUG("[%d] Restoring checkpoint on doomed priv-mode exit", curThread->tid);
                            assert(curThread->rspCheckpoint);
                            spin::setReg(ctxt, REG::REG_RSP, curThread->rspCheckpoint);
                            spin::setReg(ctxt, REG::REG_RIP, curThread->abortPc);
                        } else if (curThread->task->hasPendingAbort()) {
                            // A pending abort is about to happen. Block.
                            GetCurRob().finishDoomedExecution(curThread->tid);
                            DEBUG("[%d] Blocking on doomed priv-mode exit", curThread->tid);
                            BlockAbortingTaskOnThread(curThread->tid);
                            return NextThread();
                        }
                    }
                }
                break;
            case MAGIC_OP_PRIV_ISDOOMED:
                {
                    assert(curThread);
                    bool doomed = curThread->isPrivDoomed();
                    spin::setReg(ctxt, REG::REG_RCX, doomed);
                    setPCToNextInstr();
                }
                break;
            case MAGIC_OP_GET_TID:  // Deprecated, remove in the future
                spin::setReg(ctxt, REG::REG_RCX, tid);
                setPCToNextInstr();
                break;
            case MAGIC_OP_GET_THREAD_ID: {
                assert(tid != 1);  // watchdog thread occupies thread ID 1.
                auto swarm_tid = tid ? tid-1 : tid;
                assert(swarm_tid < ossinfo->numThreadsPerROB * ossinfo->numROBs);
                spin::setReg(ctxt, REG::REG_RCX, swarm_tid);
                setPCToNextInstr();
                break;
            }
            case MAGIC_OP_GET_TILE_ID: {
                auto tileId = (curThread && curThread->core)
                                  ? getROBIdx(curThread->core->getCid())
                                  : 0;
                const Task* t = GetCurTask();
                assert(!t || !t->hasContainer() || t->container()->getIdx() == tileId);
                if (t && IsInFastForward()) {
                    assert(tid == 0 && tileId == 0 && !t->hasContainer());
                    assert(ROIState == ROI::INSIDE);
                    assert(t->running() && t->isIrrevocable());
                    // Give the appearance that NOHASH tasks are
                    // always at their assigned tile
                    if (t->noHashHint)
                        tileId = ossinfo->taskMapper->getDstROBIdx(
                            0, nullptr, t->hint, true);
                }
                spin::setReg(ctxt, REG::REG_RCX, tileId);
                setPCToNextInstr();
                break;
            }
            case MAGIC_OP_GET_NUM_THREADS:
                spin::setReg(ctxt, REG::REG_RCX,
                             ossinfo->numThreadsPerROB * ossinfo->numROBs);
                setPCToNextInstr();
                break;
            case MAGIC_OP_GET_NUM_TILES:
                spin::setReg(ctxt, REG::REG_RCX, ossinfo->numROBs);
                setPCToNextInstr();
                break;
            case MAGIC_OP_ISIRREVOCABLE:
                {
                    assert(curThread);
                    const Task* curTask = GetCurTask();
                    bool irrevocable = curTask ? curTask->isIrrevocable()
                                               : !curThread->isPrivDoomed();
                    spin::setReg(ctxt, REG::REG_RCX, irrevocable);
                    setPCToNextInstr();
                }
                break;
            case MAGIC_OP_READ_PSEUDOSYSCALL:
            case MAGIC_OP_WRITE_PSEUDOSYSCALL:
                {
                    bool isRead = (op == MAGIC_OP_READ_PSEUDOSYSCALL);
                    if (!curThread->isPriv()) {
                        const char* sysop = isRead? "disk read" : "disk write";
                        bool exception = Serialize(sysop);
                        if (exception) {
                            filtered_info("Marked exception on thread %d: %s pseudosyscall",
                                    tid, sysop);
                            return NextThread();
                        }
                    }
                    size_t bytes = spin::getReg(ctxt, REG::REG_RDI);
                    size_t blocks = (bytes + disk_blockSize - 1) / disk_blockSize;

                    if (blocks) {
                        size_t repRate = isRead? disk_rdRepRate : disk_wrRepRate;
                        size_t lat = isRead? disk_rdLat : disk_wrLat;

                        if (disk_firstAvailCycle < cycle)
                            disk_firstAvailCycle = cycle;
                        uint64_t queueCycles = disk_firstAvailCycle - cycle;

                        uint64_t penalty = queueCycles + lat + (blocks - 1) * repRate;

                        disk_firstAvailCycle += blocks * repRate;
                        AddPenalty(penalty);
                    }
                }
                break;
            case MAGIC_OP_MALLOC_PARTITION:
                {
                    Address startAddr = spin::getReg(ctxt, REG::REG_RDI);
                    Address endAddr = spin::getReg(ctxt, REG::REG_RSI);
                    uint64_t partId = spin::getReg(ctxt, REG::REG_RDX);
                    if (ossinfo->memoryPartition)
                        ossinfo->memoryPartition->insert(startAddr, endAddr,
                                                         partId);
                    DEBUG("start: 0x%lx, end: 0x%lx, partID: %lu", startAddr,
                         endAddr, partId);
                }
                break;
            case MAGIC_OP_GET_PARFUNC:
                panic("Deprecated operation GET_PARFUNC");
                break;

            default:
                panic("Thread %d issued unknown magic op %ld!",
                        curThread ? curThread->tid : -1, op);
        }
    }

    return tid;
}


/* Analysis functions */

/**
 * Need to peek at the following instruction before deciding where to issue from.
 * If its the same thread -> keep going. Else exit without any side effects.
 *
 * Invariant 1 : First ins from each Bbl will be issued from issueHead().
 *               Everything else from simulate().
 * Invariant 2 : Only one thread from each core will be in the RUNNING/QUEUED at a time
 */
spin::ThreadId PreSwitchCall(spin::ThreadId tid, BblInfo* bblInfo) {
    auto curThread = GetCurThread();
    assert(curThread);
    assert(!IsInFastForward());
    DEBUG("[%d] %d Entering SwitchCall", curThread->tid, tid);

    /**
     * Do not model any core related activities if at least one thread is on a barrier.
     * This removes deadlocks for now. but I'm not 100% convinced they'll be gone.
     */
    if (curThread->core && barrierWaiters.size() == 0) {
        bool isActiveThread = curThread->core->recordBbl(bblInfo, curThread->tid);
        if (isActiveThread) isActiveThread = curThread->core->simulate();
        if (!isActiveThread) {
            assert(ossinfo->isScoreboard);
            DEBUG("Not active thread, blocking");
            BlockThreadAfterSwitch(BLOCKED);
            spin::loop();
            return NextThread();
        }
        // For the IPC-1 model, CountInstr() equivalent is done through issueHead()
    }
    return tid;
}


spin::ThreadId SwitchCallHandler(const spin::ThreadId tid,
                spin::ThreadContext* ctxt,
                SwitchCallType type, BblInfo* bblInfo, MemAccessType memAccessType,
                ADDRINT memAddr1, ADDRINT memAddr2, ADDRINT memAddr3,
                uint32_t read_size, uint32_t write_size,
                BOOL isCondBranch, BOOL branchTaken) {
    // [mcj] This function is only injected when the simulator is not in
    // fast-forward mode.
    assert(!IsInFastForward());
    spin::ThreadId retId = PreSwitchCall(tid, bblInfo);
    if (retId != tid || spin::isLoopSet() ) return retId;

    DEBUG("[%d] Switch Call Handler 0x%lx type:%d", tid, bblInfo->addr, type);
    auto curThread = GetCurThread();
    assert(!curThread || tid == curThread->tid);

    switch (type) {
        case SwitchCall_MAGIC_OP:
            retId = HandleMagicOp(tid,ctxt);
            break;
        case SwitchCall_DEQUEUE_OP:
            retId = HandleDequeueMagicOp(tid,ctxt);
            break;
        case SwitchCall_MEM_ACCESS:
            switch(memAccessType){
                case TYPE_L:
                case TYPE_S:
                    retId = SwitchThreadCheckAddr1(tid, memAddr1);
                    break;
                case TYPE_LL:
                case TYPE_LS:
                    retId = SwitchThreadCheckAddr2(tid, memAddr1, memAddr2);
                    break;
                case TYPE_LLS:
                    retId = SwitchThreadCheckAddr3(tid, memAddr1, memAddr2, memAddr3);
                    break;
                default:
                    panic("Invalid %d", memAccessType);
            }
            if (retId==tid && curThread && curThread->core && barrierWaiters.size() ==0) {
                // Corner case where a SwitchThread() returns the same thread
                // after it has processed an abort on the task running on itself
                if ( spin::getReg(spin::getContext(tid), REG::REG_RIP) == curThread->abortPc) return tid;
                switch(memAccessType) {
                case TYPE_L:
                    retId = RecordLoad1(tid, memAddr1, read_size);
                    break;
                case TYPE_LL:
                    retId = RecordLoad1(tid, memAddr1, read_size);
                    // Always need to check if the task has been aborted
                    if (retId == tid && spin::getReg(spin::getContext(tid), REG::REG_RIP) != curThread->abortPc) {
                        retId = RecordLoad2(tid, memAddr2, read_size);
                    }
                    break;
                case TYPE_LLS:
                    retId = RecordLoad1(tid, memAddr1, read_size);
                    if (retId == tid && spin::getReg(spin::getContext(tid), REG::REG_RIP) != curThread->abortPc) {
                        retId = RecordLoad2(tid, memAddr2, read_size);
                    }
                    if (retId == tid && spin::getReg(spin::getContext(tid), REG::REG_RIP) != curThread->abortPc) {
                        retId = RecordStore(tid, memAddr3, write_size);
                    }
                    break;
                case TYPE_S:
                    retId = RecordStore(tid, memAddr1, write_size);
                    break;
                case TYPE_LS:
                    retId = RecordLoad1(tid, memAddr1, read_size);
                    if (retId == tid && spin::getReg(spin::getContext(tid), REG::REG_RIP) != curThread->abortPc) {
                        retId = RecordStore(tid, memAddr2, write_size);
                    }
                    break;
                }
            }

            break;
        case SwitchCall_BBL_BOUNDARY :
        case SwitchCall_COND_BRANCH :
            retId = tid;
            break; // Do nothing
        default:
            panic("Invalid %d", type);
    }

    // There were two situations when switchCallLooping was set but spin::loop was not set
    // 1) After all threads reach a barrier and 2) on a serialize magic op.
    // For (1), the idea was to standardize the cycle count point to be the one after the barrier.
    // But now, the barrier op of last thread to reach the barrier will be counted.
    // For (2), the idea was to prevent an issue after an exception. This can corrupt sbcore state.
    // Threfore sbcore::issueHead should now check whether the task has an exception.
    if (retId!=tid || spin::isLoopSet() ) {
        DEBUG("Looping before issueHead");
        return retId;
    }
    // stuff that should happen only once

    if (barrierWaiters.size() ==0 && spin::getReg(spin::getContext(tid), REG::REG_RIP) != curThread->abortPc) {
        if (isCondBranch) {
            SbCore* sbcore = dynamic_cast<SbCore*>(GetThreadState(tid)->core);
            if(sbcore){
                sbcore->branch(bblInfo->addr, branchTaken, tid);
            }
        }
        curThread->core->issueHead(curThread->tid);
    }
    return retId;
}


VOID PrintTaskAndIndirectTarget(ADDRINT ip, ADDRINT target) {
    const Task* task = GetCurTask();
    if (!task) return;
    info("[%d] Task 0x%lx indirect branch/call from 0x%lx to 0x%lx",
         task->runningTid, task->taskFn, ip, target);
}


/* Instrumentation */
inline bool isMagicOpIns(INS ins) {
    return INS_IsXchg(ins) && INS_OperandReg(ins, 0) == REG::REG_RCX && INS_OperandReg(ins, 1) == REG::REG_RCX;
}

inline bool isDequeueMagicOpIns(INS ins) {
    return INS_IsXchg(ins) && INS_OperandReg(ins, 0) == REG::REG_RDX && INS_OperandReg(ins, 1) == REG::REG_RDX;
}

void FastForwardTrace(TRACE trace, spin::TraceInfo& st) {
    INS firstIns = BBL_InsHead(TRACE_BblHead(trace));
    for (BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl)) {
        for (INS ins = BBL_InsHead(bbl); INS_Valid(ins); ins = INS_Next(ins)) {
            if (isMagicOpIns(ins)) {
                st.insertSwitchCall(ins, IPOINT_BEFORE, (AFUNPTR)HandleMagicOp, IARG_SPIN_THREAD_ID, IARG_SPIN_CONTEXT);
            } else if (isDequeueMagicOpIns(ins)) {
                st.insertSwitchCall(ins, IPOINT_BEFORE, (AFUNPTR)HandleFfDequeueMagicOp, IARG_SPIN_THREAD_ID, IARG_SPIN_CONTEXT);
            } else if (ins == firstIns) {
                st.insertSwitchCall(firstIns, IPOINT_BEFORE,
                                    (AFUNPTR)PreventFfStarvation,
                                    IARG_SPIN_THREAD_ID);
            }
        }
    }
}

void NonFastForwardTrace(TRACE trace, spin::TraceInfo& st) {
    for (BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl)) {
        INS lastCountIns = BBL_InsHead(bbl);
        BOOL isCondBranch = false;
        SwitchCallType type = SwitchCall_BBL_BOUNDARY;
        MemAccessType memType = TYPE_L;
        const IARG_TYPE IARG_DEFAULT = IARG_INST_PTR;
        IARG_TYPE memAddr1 = IARG_DEFAULT;
        IARG_TYPE memAddr2 = IARG_DEFAULT;
        IARG_TYPE memAddr3 = IARG_DEFAULT;
        IARG_TYPE read_size = IARG_DEFAULT;
        IARG_TYPE write_size = IARG_DEFAULT;
        for (INS ins = BBL_InsHead(bbl); INS_Valid(ins); ins = INS_Next(ins)) {
            if (isDequeueMagicOpIns(ins) || isMagicOpIns(ins) || INS_IsMemoryRead(ins) || INS_IsMemoryWrite(ins) || (INS_Category(ins) == XED_CATEGORY_COND_BR) ) {
                if ( ins != lastCountIns ) {
                    BblInfo* bblInfo = Decoder::decodeBbl(bbl,lastCountIns,ins);
                    st.insertSwitchCall(lastCountIns, IPOINT_BEFORE, (AFUNPTR)SwitchCallHandler, IARG_SPIN_THREAD_ID, IARG_SPIN_CONTEXT,
                            IARG_UINT32, (uint32_t) type, IARG_PTR, bblInfo, IARG_UINT32, memType,
                            memAddr1, memAddr2, memAddr3,
                            read_size, write_size,
                            IARG_UINT32, (uint32_t) isCondBranch, IARG_BRANCH_TAKEN);
                    lastCountIns = ins;
                    type = SwitchCall_BBL_BOUNDARY;
                    memAddr1 = IARG_DEFAULT;
                    memAddr2 = IARG_DEFAULT;
                    memAddr3 = IARG_DEFAULT;
                    read_size = IARG_DEFAULT;
                    write_size = IARG_DEFAULT;
                }
            }
            if (INS_Category(ins) == XED_CATEGORY_COND_BR){
                isCondBranch = true;
                type = SwitchCall_COND_BRANCH;
                // Note 1:
                // It's unfortunate to have to treat every conditional branch ins as a switchpoint.
                // A simpler solution is to have a function RecordBranch, instrumented through st.insertCall(), call sbcore->branch()
                // But this happen should before the SwitchCallHandler calls issueHead()
                // No way to make it work with spin, so the last resort is to pass everything to switchCallHandler
                // and make sure sbcore->branch is called before issueHead()

                // (For future reference, there was a corner case where BBl consisted of just one instruction, which was a conditional branch.
                // RecordBranch was called after the SwitchCallHandler, hence the branch was missed)

                // Note 2:
                // If a conditional branch ins cannot access memory, it is possible to get rid of the isCondBranch passed to SwitchCallHandler
                // and infer it from  (type = SwitchCall_COND_BRANCH)
                // But since I don't know for sure whether the ISA doesn't allow it, I'll go safe.
            }
            // [mcj,maleen]: The following can only be "else if" if conditional
            // branch instructions cannot access memory. We are not sure if the
            // ISA allows such branches
            if (isMagicOpIns(ins)) {
                type = SwitchCall_MAGIC_OP;
            } else if (isDequeueMagicOpIns(ins)) {
                type = SwitchCall_DEQUEUE_OP;
            } else {
                bool load = INS_IsMemoryRead(ins);
                bool load2 = INS_HasMemoryRead2(ins);
                bool store = INS_IsMemoryWrite(ins);
                assert(!load2 || load);
                if (load) read_size = IARG_MEMORYREAD_SIZE;
                if (store) write_size = IARG_MEMORYREAD_SIZE;
                if (load || load2 || store) {
                    type = SwitchCall_MEM_ACCESS;
                    if (load && !load2 && !store){
                        // FIXME(victory): prefetch instructions currently fall under this type but perhaps
                        //                 should be handled specially. Pin describes their size as 64 bytes
                        //                 which almost always looks like a line-crossing access.
                        memType = TYPE_L;
                        memAddr1 = IARG_MEMORYREAD_EA;
                    } else if (store && !load2 && !load){
                        memType = TYPE_S;
                        memAddr1 = IARG_MEMORYWRITE_EA;
                    } else if (load && load2 && !store){
                        memType = TYPE_LL;
                        memAddr1 = IARG_MEMORYREAD_EA;
                        memAddr2 = IARG_MEMORYREAD2_EA;
                    } else if (load && !load2 && store){
                        memType = TYPE_LS;
                        memAddr1 = IARG_MEMORYREAD_EA;
                        memAddr2 = IARG_MEMORYWRITE_EA;
                    } else {
                        assert (load && load2 && store);
                        memType = TYPE_LLS;
                        memAddr1 = IARG_MEMORYREAD_EA;
                        memAddr2 = IARG_MEMORYREAD2_EA;
                        memAddr3 = IARG_MEMORYWRITE_EA;
                    }
                }
            }

            if (ossinfo->logIndirectTargets
                    && INS_IsIndirectBranchOrCall(ins)
                    && !INS_IsRet(ins)) {
                INS_InsertCall(ins, IPOINT_BEFORE,
                               (AFUNPTR)PrintTaskAndIndirectTarget,
                               IARG_INST_PTR,
                               IARG_BRANCH_TARGET_ADDR,
                               IARG_END);
            }
        }

        INS invalid = INS_Next(BBL_InsTail(bbl));
        BblInfo* bblInfo = Decoder::decodeBbl(bbl,lastCountIns,invalid);
        st.insertSwitchCall(lastCountIns, IPOINT_BEFORE, (AFUNPTR)SwitchCallHandler, IARG_SPIN_THREAD_ID, IARG_SPIN_CONTEXT,
                IARG_UINT32, (uint32_t) type, IARG_PTR, bblInfo, IARG_UINT32, memType,
                memAddr1, memAddr2, memAddr3,
                read_size, write_size,
                IARG_UINT32, (uint32_t) isCondBranch, IARG_BRANCH_TAKEN);
    }
}

void Trace(TRACE trace, spin::TraceInfo& st) {
   if (IsInFastForward()) FastForwardTrace(trace, st);
   else NonFastForwardTrace(trace, st);
}

void Fini(int32_t code, void* v) {
    // The simulation should have reached ROI End, or else some other
    // invalid termination condition was met.
    assert(ROIState == ROI::AFTER);
    SimEnd();
}

void SimEnd() {
    assert(ossinfo);

    if (endHandler) {
       info("Running userspace handler for sim end");
       // [victory] Just jumping into userspace code like this seems very
       // sketchy, but it appears to work alright if we're in fast-forwarding.
       // We can make this more robust by doing something more similar to how
       // we jump to exceptionHandler.
       endHandler();
    }

    // Since Pin leaks memory on code cache flushes, print utilization to
    // detect when this is the cause of excessive memory consumption.
    info("Pin memory usage: %d KB | code cache usage: %d / %d KB",
         PIN_MemoryAllocatedForPin() >> 10, CODECACHE_CodeMemUsed() >> 10,
         CODECACHE_CacheSizeLimit() >> 10);

    info("Done at cycle %ld", getCurCycle());
    printTaskProfilingInfo();

    // [victory] If exiting during non-fast-forwarding due to maxHeartbeats,
    //           don't bother winding up the cores, which triggers assertion
    //           failures for OoO cores right now.  Hopefully this is a minor
    //           issue as all stats for tasks in flight at termination are
    //           approximate anyway.
    // FIXME(victory): It'd be nice to fix the underlying assertion error in
    //                 OoO's stats/state transitions.
    // TODO(victory): This (and the redundant code in ThreadFini) feels like
    // the wrong place to do this. Couldn't we do this when exiting the ROI or
    // entering fast-forwarding?
    assert(ROIState == ROI::INSIDE || IsInFastForward());
    assert(ROIState != ROI::BEFORE || getCurCycle() == 0);
    if (ROIState == ROI::AFTER) {
        // [maleen] Apparently thread 0 never goes through ThreadFini.
        // Since I'm not sure whether this will always be thread 0, iterate through everything
        for (uint64_t t = 0; t<=ossinfo->numCores*ossinfo->numThreadsPerCore; t++){
            SbCore* sbcore = dynamic_cast<SbCore*>(GetThreadState(t)->core);
            if (sbcore) {
                // [mcj] Remind me again why are getCurCycle and core->getCycle
                // different? getCurCycle is the current thread's cycle? Seems like
                // that should be deprecated.
                uint64_t cycle = std::max(
                        getCurCycle(),
                        sbcore->getCycle(t));
                sbcore->windUp(cycle);
            }
        }
    }

    info("Dumping termination stats");
    for (StatsBackend* backend : ossinfo->statsBackends) {
        backend->dump(false /*unbuffered, write out*/);
    }

    StopWatchdogThread();
    TickWatchdog();

    trace::fini();

    // [victory] If a benchmark terminates in the midst of simulation (e.g., if
    // an autoparallelized SCC app calls std::exit() instead of returning from
    // main()), the Task destructor panics if some non-UNREACHABLE Task is
    // destructed.  So, let's avoid attempting to destruct objects.
    //std::exit(0);
    std::_Exit(0);
}

/* External interface functions (see sim.h) */

uint64_t getCurThreadLocalCycle() {
    auto curThread = GetCurThread();
    if (curThread == nullptr) {
        return -1;
    } else {
        auto threadCycle = curThread->core->getCycle(curThread->tid);
        return std::max(getCurCycle(), threadCycle);
    }
}

Task* GetCurTask() {
    auto curThread = GetCurThread();
    return curThread? curThread->task.get() : nullptr;
}

[[noreturn]] void CppExceptionHandler() noexcept {
    // dsm: Why does C++ make this so difficult?
    std::string exStr = "??";
    std::exception_ptr ep = std::current_exception();
    try {
        if (ep) {
            std::rethrow_exception(ep);
        } else {
            exStr = "invalid exception";
        }
    } catch(const std::exception& e) {
        exStr = e.what();
    }
    // This leverages implementation-defined behavior... gcc does not unwind
    // the stack on an exception. But other compilers might (if so, the
    // backtrace won't be very useful)
    PrintBacktrace();
    panic("C++ exception: %s", exStr.c_str());
}

EXCEPT_HANDLING_RESULT InternalExceptionHandler(THREADID tid,
        EXCEPTION_INFO* pExceptInfo, PHYSICAL_CONTEXT*, VOID*) {
    // TODO: Use PIN_GetPhysicalContextReg() to read the values from the
    // PHYSICAL_CONTEXT* argument.  Note these are the register values
    // for the simulator tool, not the simulated application's registers.
    panic("[%d] Internal Pin/tool exception: %s.\n",
          tid, PIN_ExceptionToString(pExceptInfo).c_str());
    return EHR_UNHANDLED;
}

int main(int argc, char *argv[]) {
    PIN_InitSymbols();
    if (PIN_Init(argc, argv)) panic("Wrong args");

    InitLog("[sim] ", nullptr);
    std::set_terminate(CppExceptionHandler);
    PIN_AddInternalExceptionHandler(InternalExceptionHandler, 0);

    // If the harness dies, ensure we do too
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0) panic("prctl() failed");

    trace::init();
    virt::init();

    SimInit(KnobConfigFile.Value().c_str());

    spin::init(Trace, ThreadStart, ThreadFini, Capture, Uncapture);
    spin::setSyscallEnterCallback(HandleSyscall);
    spin::setSyscallExitCallback(virt::syscallExit);

    // NOTE: spin is transparent to SyscallEnter/Exit/InterceptSignal/ContextChange callbacks
    PIN_AddFiniFunction(Fini, 0);
    PIN_UnblockSignal(SIGINT, true);
    PIN_UnblockSignal(SIGQUIT, true);
    PIN_UnblockSignal(SIGTERM, true);
    for (int sig = 1;  sig < __SIGRTMIN;  sig++) {
        if (sig == SIGWINCH) {
            // Benign signal from external environment should be blocked.
            PIN_InterceptSignal(sig, SuppressSignal, nullptr);
        } else if (matchAny(sig, SIGTSTP, SIGCONT)) {
            // Don't intercept these signals, enable users to pause and resume
            // interactively running simulations.
        } else if (matchAny(sig, SIGCHLD)) {
            // Allow the simulator to spawn and manage child processes?
            // E.g., the watchdog uses std::system to run perf.
        } else if (matchAny(sig, SIGKILL, SIGSTOP)) {
            // These signals cannot be caught, and Pin documentation says we
            // shouldn't call PIN_InterceptSignal with these.
        } else {
            PIN_InterceptSignal(sig, InterceptSignal, nullptr);
        }
    }
    PIN_AddContextChangeFunction(ContextChange, 0);

    InitDriver();

    // Never returns
    PIN_StartProgram();
    return 0;
}
