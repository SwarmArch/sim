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

#include "sim/log.h"

#include "sim/driver.h"
#include "sim/locks.h"
#include "sim/taskprofilingsummary.h"
#include "spin.h"

#include <csignal>
#include <execinfo.h>
#include <fstream>
#include <stdlib.h>
#include <string.h>
#include <unordered_map>

const char* logHeader = "";

const char* logTypeNames[] = {"Harness", "Config", "Process", "Cache",
                              "Mem",     "Sched",  "FSVirt",  "TimeVirt"};

FILE* logFdOut = stdout;
FILE* logFdErr = stderr;

static lock_t log_printLock;

void InitLog(const char* header, const char* file) {
    logHeader = strdup(header);
    futex_init(&log_printLock);

    if (file) {
        FILE* fd = fopen(file, "a");
        if (fd == NULL) {
            perror("fopen() failed");
            // We can panic in InitLog (will dump to stderr)
            panic("Could not open logfile %s", file);
        }
        logFdOut = fd;
        logFdErr = fd;
        // NOTE: We technically never close this fd, but always flush it
    }
}

void __log_lock() { futex_lock(&log_printLock); }
void __log_unlock() { futex_unlock(&log_printLock); }

void __log_fail() {
    static bool failing = false;
    if (failing) {
        fprintf(logFdErr, "%sFailure during panic.\n", logHeader);
        fflush(logFdErr);
        PrintBacktrace();
        raise(SIGABRT); // allow capturing in gdb
        std::_Exit(PANIC_EXIT_CODE);
    }
    failing = true;

    ThreadState* curThread = GetCurThread();
    if (curThread) {
        auto tid = curThread->tid;
        fprintf(logFdErr,
                "%sCurrent thread ID=%d,"
                " rsp=0x%lx\n",
                logHeader, tid,
                spin::getReg(spin::getContext(tid), REG::REG_RSP));
        fprintf(logFdErr, "%sCurrent thread state: %s\n", logHeader,
                curThread->toString().c_str());
        fflush(logFdErr);
    } else {
        fprintf(logFdErr, "%sNo current thread.\n", logHeader);
        fflush(logFdErr);
    }

    fprintf(logFdOut, "%sTask profile up to moment of failure:\n", logHeader);
    printTaskProfilingInfo();

    fprintf(logFdOut, "%sCopying /proc/self/maps to memory_map\n", logHeader);
    CopyMaps();

    PrintBacktrace();

    raise(SIGABRT); // allow capturing in gdb
    std::_Exit(PANIC_EXIT_CODE);
}

static std::unordered_map<const char*, size_t> filteredLogCounts;

ssize_t __count_filtered_log(const char* fmtStr) {
    auto it = filteredLogCounts.find(fmtStr);
    if (it == filteredLogCounts.end()) {
        filteredLogCounts.insert({fmtStr, 1});
        return 1;
    } else {
        const size_t filterThreshold = 64;  // must be a power of 2
        size_t count = ++(it->second);
        if (count < filterThreshold) {
            return 1;  // print
        } else if (count == filterThreshold) {
            return -1;  // signal we'll start filtering
        } else if (count & (count - 1)) {
            return 0;  // not a power of 2, filter
        } else {
            return (count / 2) - 1;  // report how many we've filtered since
        }
    }
}

void PrintBacktrace() {
    void* array[40];
    size_t size = backtrace(array, 40);
    char** strings = backtrace_symbols(array, size);
    fprintf(stderr, "%sBacktrace (%ld/%d max frames)\n", logHeader, size, 40);
    for (uint32_t i = 0; i < size; i++) {
        // For serializer.so addresses, call addr2line to get symbol info (can't
        // use -rdynamic on libzsim.so because of Pin's linker script)
        // NOTE: May be system-dependent, may not handle malformed strings well.
        // We're going to die anyway, so in for a penny, in for a pound...
        std::string s = strings[i];
        uint32_t lp = s.find_first_of("(");
        uint32_t cp = s.find_first_of(")");
        std::string fname = s.substr(0, lp);
        std::string faddr = s.substr(lp + 1, cp - (lp + 1));
        if (fname.find("speculator_") != std::string::npos) {
            std::string cmd = "addr2line -f -C -e " + fname + " " + faddr;
            FILE* f = popen(cmd.c_str(), "r");
            if (f) {
                char buf[1024];
                std::string func, loc;
                func = fgets(buf, 1024, f);  // first line is function name
                loc = fgets(buf, 1024, f);  // second is location
                // Remove line breaks
                func = func.substr(0, func.size() - 1);
                loc = loc.substr(0, loc.size() - 1);

                int status = pclose(f);
                if (status == 0) {
                    s = loc + " / " + func;
                }
            }
        }

        fprintf(stderr, "%s  %s\n", logHeader, s.c_str());
    }
    fflush(stderr);
}

void CopyMaps() {
    std::ifstream src("/proc/self/maps", std::ios::binary);
    std::ofstream dst("memory_map", std::ios::binary);
    dst << src.rdbuf();
}
