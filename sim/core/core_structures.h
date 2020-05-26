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

#ifndef CORE_STRUCTURES_H
#define CORE_STRUCTURES_H

#include <map>
#include <stdint.h>
#include <utility>

#include "sim/assert.h"
#include "sim/spin_cv.h"

template<uint32_t H, uint32_t WSZ>
class WindowStructure {
    private:
        // NOTE: Nehalem has POPCNT, but we want this to run reasonably fast on Core2's, so let's keep track of both count and mask.
        struct WinCycle {
            uint8_t occUnits;
            uint8_t count;
            inline void set(uint8_t o, uint8_t c) {occUnits = o; count = c;}
        };

        WinCycle* curWin;
        WinCycle* nextWin;
        typedef std::map<uint64_t, WinCycle> UBWin;
        typedef typename UBWin::iterator UBWinIterator;
        UBWin ubWin;
        uint32_t occupancy;  // elements scheduled in the future

        uint32_t curPos;

        uint8_t lastPort;

    public:
        WindowStructure() {
            curWin = (WinCycle*) calloc(H*sizeof(WinCycle),1);
            nextWin = (WinCycle*) calloc(H*sizeof(WinCycle),1);
            curPos = 0;
            occupancy = 0;
        }

        bool advanceTillNotFull(uint64_t& curCycle) {
            bool advanced = false;
            while (occupancy == WSZ) {
                advanced = true;
                advancePos(curCycle);
            }
            return advanced;
        }

        void schedule(uint64_t& curCycle, uint64_t& schedCycle, uint8_t portMask, uint32_t extraSlots = 0) {
            if (!extraSlots) {
                scheduleInternal<true, false>(curCycle, schedCycle, portMask);
            } else {
                scheduleInternal<true, true>(curCycle, schedCycle, portMask);
                uint64_t extraSlotCycle = schedCycle+1;
                uint8_t extraSlotPortMask = 1 << lastPort;
                // This is not entirely accurate, as an instruction may have been scheduled already
                // on this port and we'll have a non-contiguous allocation. In practice, this is rare.
                for (uint32_t i = 0; i < extraSlots; i++) {
                    scheduleInternal<false, false>(curCycle, extraSlotCycle, extraSlotPortMask);
                    // info("extra slot %d allocated on cycle %ld", i, extraSlotCycle);
                    extraSlotCycle++;
                }
            }
            assert(occupancy <= WSZ);
        }

        inline void advancePos(uint64_t& curCycle) {
            occupancy -= curWin[curPos].count;
            curWin[curPos].set(0, 0);
            curPos++;
            curCycle++;

            if (curPos == H) {  // rebase
                // info("[%ld] Rebasing, curCycle=%ld", curCycle/H, curCycle);
                std::swap(curWin, nextWin);
                curPos = 0;
                uint64_t nextWinHorizon = curCycle + 2*H;  // first cycle out of range

                if (!ubWin.empty()) {
                    UBWinIterator it = ubWin.begin();
                    while (it != ubWin.end() && it->first < nextWinHorizon) {
                        uint32_t nextWinPos = it->first - H - curCycle;
                        assert_msg(nextWinPos < H, "WindowStructure: ubWin elem exceeds limit cycle=%ld curCycle=%ld nextWinPos=%d", it->first, curCycle, nextWinPos);
                        nextWin[nextWinPos] = it->second;
                        // info("Moved %d events from unbounded window, cycle %ld (%d cycles away)", it->second, it->first, it->first - curCycle);
                        it++;
                    }
                    ubWin.erase(ubWin.begin(), it);
                }
            }
        }

        void longAdvance(uint64_t& curCycle, uint64_t targetCycle) {
            assert(curCycle <= targetCycle);

            // Drain IW
            while (occupancy && curCycle < targetCycle) {
                advancePos(curCycle);
            }

            if (occupancy) {
                // info("advance: window not drained at %ld, %d uops left", curCycle, occupancy);
                assert(curCycle == targetCycle);
            } else {
                // info("advance: window drained at %ld, jumping to %ld", curCycle, targetCycle);
                assert(curCycle <= targetCycle);
                curCycle = targetCycle;  // with zero occupancy, we can just jump to it
            }
        }

        // Poisons a range of cycles; used by the LSU to apply backpressure to the IW
        void poisonRange(uint64_t curCycle, uint64_t targetCycle, uint8_t portMask) {
            uint64_t startCycle = curCycle;  // curCycle should not be modified...
            uint64_t poisonCycle = curCycle;
            while (poisonCycle < targetCycle) {
                scheduleInternal<false, false>(curCycle, poisonCycle, portMask);
            }
            // info("Poisoned port mask %x from %ld to %ld (tgt %ld)", portMask, curCycle, poisonCycle, targetCycle);
            assert(startCycle == curCycle);
        }

    private:
        template <bool touchOccupancy, bool recordPort>
        void scheduleInternal(uint64_t& curCycle, uint64_t& schedCycle, uint8_t portMask) {
            // If the window is full, advance curPos until it's not
            while (touchOccupancy && occupancy == WSZ) {
                advancePos(curCycle);
            }

            uint32_t delay = (schedCycle > curCycle)? (schedCycle - curCycle) : 0;

            // Schedule, progressively increasing delay if we cannot find a slot
            uint32_t curWinPos = curPos + delay;
            while (curWinPos < H) {
                if (trySchedule<touchOccupancy, recordPort>(curWin[curWinPos], portMask)) {
                    schedCycle = curCycle + (curWinPos - curPos);
                    break;
                } else {
                    curWinPos++;
                }
            }
            if (curWinPos >= H) {
                uint32_t nextWinPos = curWinPos - H;
                while (nextWinPos < H) {
                    if (trySchedule<touchOccupancy, recordPort>(nextWin[nextWinPos], portMask)) {
                        schedCycle = curCycle + (nextWinPos + H - curPos);
                        break;
                    } else {
                        nextWinPos++;
                    }
                }
                if (nextWinPos >= H) {
                    schedCycle = curCycle + (nextWinPos + H - curPos);
                    UBWinIterator it = ubWin.lower_bound(schedCycle);
                    while (true) {
                        if (it == ubWin.end()) {
                            WinCycle wc = {0, 0};
                            bool success = trySchedule<touchOccupancy, recordPort>(wc, portMask);
                            assert(success);
                            ubWin.insert(std::pair<uint64_t, WinCycle>(schedCycle, wc));
                        } else if (it->first != schedCycle) {
                            WinCycle wc = {0, 0};
                            bool success = trySchedule<touchOccupancy, recordPort>(wc, portMask);
                            assert(success);
                            ubWin.insert(it /*hint, makes insert faster*/, std::pair<uint64_t, WinCycle>(schedCycle, wc));
                        } else {
                            if (!trySchedule<touchOccupancy, recordPort>(it->second, portMask)) {
                                // Try next cycle
                                it++;
                                schedCycle++;
                                continue;
                            }  // else scheduled correctly
                        }
                        break;
                    }
                    // info("Scheduled event in unbounded window, cycle %ld", schedCycle);
                }
            }
            if (touchOccupancy) occupancy++;
        }

        template <bool touchOccupancy, bool recordPort>
        inline uint8_t trySchedule(WinCycle& wc, uint8_t portMask) {
            static_assert(!(recordPort && !touchOccupancy), "Can't have recordPort and !touchOccupancy");
            if (touchOccupancy) {
                uint8_t availMask = (~wc.occUnits) & portMask;
                if (availMask) {
                    // info("PRE: occUnits=%x portMask=%x availMask=%x", wc.occUnits, portMask, availMask);
                    uint8_t firstAvail = __builtin_ffs(availMask) - 1;
                    // NOTE: This is not fair across ports. I tried round-robin scheduling, and there is no measurable difference
                    // (in our case, fairness comes from following program order)
                    if (recordPort) lastPort = firstAvail;
                    wc.occUnits |= 1 << firstAvail;
                    wc.count++;
                    // info("POST: occUnits=%x count=%x firstAvail=%d", wc.occUnits, wc.count, firstAvail);
                }
                return availMask;
            } else {
                // This is a shadow req, port has only 1 bit set
                uint8_t availMask = (~wc.occUnits) & portMask;
                wc.occUnits |= portMask;  // or anyway, no conditionals
                return availMask;
            }
        }
};

class ReorderBuffer {
    private:
        uint64_t* buf;
        uint32_t* idx;

        uint32_t SZ;
        uint32_t numPartitions;
        uint32_t entriesPerPartition;

    public:
        ReorderBuffer(uint32_t _SZ, uint32_t _numPartitions) {
            SZ = _SZ;
            numPartitions = _numPartitions;
            assert((SZ % numPartitions) ==0);
            entriesPerPartition = SZ/ numPartitions;
            buf = new uint64_t[SZ];
            idx = new uint32_t[numPartitions];
            for (uint32_t i = 0; i < SZ; i++) buf[i] = 0;
            for (uint32_t i = 0; i < numPartitions; i++) idx[i] = 0;
        }

        virtual inline uint64_t minAllocCycle(uint32_t pid) {
            if (pid >= numPartitions) pid = 0; // HACK: Keep interface constant regardless of partitioning
            return buf[pid* entriesPerPartition + idx[pid]];
        }

        virtual inline void markRetire(uint64_t minRetireCycle, uint32_t pid) {
            // Do not enforce limitation of max retires per cycle.
            // This is hard to keep track of with a partitioned ROB
            if (pid >= numPartitions) pid = 0; // HACK
            buf[pid* entriesPerPartition + idx[pid]] = minRetireCycle;
            idx[pid]++;
            if (idx[pid] == entriesPerPartition) idx[pid] = 0;
        }

        virtual inline uint32_t getNextIndex(uint32_t pid) {
            if (pid >= numPartitions) pid = 0; // HACK
            return pid* entriesPerPartition + idx[pid];
        }
};

// class ReorderBuffer {
//     private:
//         uint32_t SZ, W;
//
//         uint64_t* buf;
//
//         uint64_t curRetireCycle;
//         uint32_t curCycleRetires;
//         uint32_t idx;
//
//     public:
//         ReorderBuffer(uint32_t _SZ, uint32_t _W) {
//             SZ = _SZ;
//             W = _W;
//             buf = new uint64_t[SZ];
//
//             for (uint32_t i = 0; i < SZ; i++) buf[i] = 0;
//             idx = 0;
//             curRetireCycle = 0;
//             curCycleRetires = 1;
//         }
//
//         inline uint64_t minAllocCycle() {
//             return buf[idx];
//         }
//
//         inline void markRetire(uint64_t minRetireCycle) {
//             if (minRetireCycle <= curRetireCycle) {  // retire with bundle
//                 if (curCycleRetires == W) {
//                     curRetireCycle++;
//                     curCycleRetires = 0;
//                 } else {
//                     curCycleRetires++;
//                 }
//
//                 /* No branches version (careful, width should be power of 2...)
//                  * curRetireCycle += curCycleRetires/W;
//                  * curCycleRetires = (curCycleRetires + 1) % W;
//                  *  NOTE: After profiling, version with branch seems faster
//                  */
//             } else {  // advance
//                 curRetireCycle = minRetireCycle;
//                 curCycleRetires = 1;
//             }
//
//             buf[idx++] = curRetireCycle;
//             if (idx == SZ) idx = 0;
//         }
// };

#endif
