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

/* General logging/info/warn/panic routines */

#ifdef SWARM_SIM_NO_LOG
#error "Don't include sim/log.h in unit test builds, use sim/assert.h instead."
#endif //SWARM_SIM_NO_LOG

#ifndef LOG_H_
#define LOG_H_

#include <iterator>
#include <set>
#include <sstream>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "sim/assert.h"

void __log_lock();
void __log_unlock();
[[noreturn]] void __log_fail();
ssize_t __count_filtered_log(const char* fmtStr);

#ifdef MT_SAFE_LOG
#define log_lock() __log_lock()
#define log_unlock() __log_unlock()
#else
#define log_lock()
#define log_unlock()
#endif

#define PANIC_EXIT_CODE (112)

typedef enum {
    LOG_Harness,
    LOG_Config,
    LOG_Process,
    LOG_Cache,
    LOG_Mem,
    LOG_Sched,
    LOG_FSVirt,
    LOG_TimeVirt,
} LogType;

// defined in log.cpp
extern const char* logTypeNames[];
extern const char* logHeader;
extern FILE* logFdOut;
extern FILE* logFdErr;

/* Set per-process header for log/info/warn/panic messages
 * Calling this is not needed (the default header is ""),
 * but it helps in multi-process runs
 * If file is NULL or InitLog is not called, logs to stdout/stderr
 */
void InitLog(const char* header, const char* file = NULL);

void PrintBacktrace();

void CopyMaps();

/* Helper class to print expression with values
 * Inpired by Phil Nash's CATCH, https://github.com/philsquared/Catch
 * const enough that asserts that use this are still optimized through
 * loop-invariant code motion
 */
class PrintExpr {
    private:
        std::stringstream& ss;

    public:
        PrintExpr(std::stringstream& _ss) : ss(_ss) {}

        // Start capturing values
        template<typename T> const PrintExpr operator->* (T t) const { capture(t, true); return *this; }

        // Overloads for all lower-precedence operators
        template<typename T> const PrintExpr operator == (T t) const { equal(t, true); return *this; }
        template<typename T> const PrintExpr operator != (T t) const { nonequal(t, true); return *this; }
        template<typename T> const PrintExpr operator <= (T t) const { ss << " <= " << t; return *this; }
        template<typename T> const PrintExpr operator >= (T t) const { ss << " >= " << t; return *this; }
        template<typename T> const PrintExpr operator <  (T t) const { ss << " < "  << t; return *this; }
        template<typename T> const PrintExpr operator >  (T t) const { ss << " > "  << t; return *this; }
        template<typename T> const PrintExpr operator &  (T t) const { ss << " & "  << t; return *this; }
        template<typename T> const PrintExpr operator |  (T t) const { ss << " | "  << t; return *this; }
        template<typename T> const PrintExpr operator ^  (T t) const { ss << " ^ "  << t; return *this; }
        template<typename T> const PrintExpr operator && (T t) const { ss << " && " << t; return *this; }
        template<typename T> const PrintExpr operator || (T t) const { ss << " || " << t; return *this; }
        template<typename T> const PrintExpr operator +  (T t) const { ss << " + "  << t; return *this; }
        template<typename T> const PrintExpr operator -  (T t) const { ss << " - "  << t; return *this; }
        template<typename T> const PrintExpr operator *  (T t) const { ss << " * "  << t; return *this; }
        template<typename T> const PrintExpr operator /  (T t) const { ss << " / "  << t; return *this; }
        template<typename T> const PrintExpr operator %  (T t) const { ss << " % "  << t; return *this; }
        template<typename T> const PrintExpr operator << (T t) const { ss << " << " << t; return *this; }
        template<typename T> const PrintExpr operator >> (T t) const { ss << " >> " << t; return *this; }

        // std::nullptr_t overloads (for nullptr's in assertions)
        // Only a few are needed, since most ops w/ nullptr are invalid
        const PrintExpr operator->* (std::nullptr_t t) const { ss << "nullptr"; return *this; }
        const PrintExpr operator == (std::nullptr_t t) const { ss << " == nullptr"; return *this; }
        const PrintExpr operator != (std::nullptr_t t) const { ss << " != nullptr"; return *this; }

    private:
        template<typename T> const PrintExpr operator =  (T t) const;  // will fail, can't assign in assertion

        // Verify cool magic that doesn't even use std::enable_if<...>
        // http://stackoverflow.com/a/23988669
        template<typename T>
        auto capture(T t, bool) const -> decltype(ss << t, void(), std::string{}) { ss << t; return ss.str(); }
        template<typename T>
        auto capture(T t, int) const -> std::string { ss << "*" << &t; return ss.str(); }

        template<typename T>
        auto equal(T t, bool) const -> decltype(ss << t, void(), std::string{}) { ss << " == " << t; return ss.str(); }
        template<typename T>
        auto equal(T t, int) const -> std::string { ss << " == *" << &t; return ss.str(); }

        template<typename T>
        auto nonequal(T t, bool) const -> decltype(ss << t, void(), std::string{}) { ss << " != " << t; return ss.str(); }
        template<typename T>
        auto nonequal(T t, int) const -> std::string { ss << " != *" << &t; return ss.str(); }
};


#define panic(args...) \
{ \
    fprintf(logFdErr, "%sPanic on %s:%d: ", logHeader, __FILE__, __LINE__); \
    fprintf(logFdErr, args); \
    fprintf(logFdErr, "\n"); \
    fflush(logFdErr); \
    __log_fail(); \
}

#define warn(args...) \
{ \
    log_lock(); \
    fprintf(logFdErr, "%sWARN: ", logHeader); \
    fprintf(logFdErr, args); \
    fprintf(logFdErr, "\n"); \
    fflush(logFdErr); \
    log_unlock(); \
}

static inline void info(const char* fmt, ...) {
    log_lock();
    fprintf(logFdOut, "%s", logHeader);
    // https://stackoverflow.com/a/20639708
    va_list args;
    va_start(args, fmt);
    vfprintf(logFdOut, fmt, args);
    va_end(args);
    fprintf(logFdOut, "\n");
    fflush(logFdOut);
    log_unlock();
}

// Like info(), but filters frequent calls to reduce output size. Filtering
// happens at an exponentially increasing rate and is done per format string,
// not per message (so frequent infos will be filtered even if their arguments
// change constantly).
#define filtered_info(fmtStr, ...) \
{ \
    log_lock(); /* grab early to serialize __count_filtered_log calls */ \
    ssize_t shouldPrint = __count_filtered_log(fmtStr); \
    if (shouldPrint) {\
        fprintf(logFdOut, "%s", logHeader); \
        fprintf(logFdOut, fmtStr, ##__VA_ARGS__); \
        if (shouldPrint < 0) { \
            fprintf(logFdOut, " [filtering further messages]"); \
        } else if (shouldPrint > 1) { \
            fprintf(logFdOut, " [%ld previous messages filtered]", shouldPrint); \
        } \
        fprintf(logFdOut, "\n"); \
        fflush(logFdOut); \
    } \
    log_unlock(); \
}

#define checkpoint()                                            \
    do {                                                        \
        info("%s:%d %s", __FILE__, __LINE__, __FUNCTION__);     \
    } while (0)

#endif  // LOG_H_
