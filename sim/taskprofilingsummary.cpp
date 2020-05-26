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

#include "sim/taskprofilingsummary.h"

#include <boost/tokenizer.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <cxxabi.h>
#include <string>
#include <unordered_map>
#include <vector>
#include "pin/pin.H"

#include "sim/sim.h"
#include "sim/str.h"

static TaskCyclesSummary rootSummary;
static bool perPcProfiling;
static std::unordered_map<void*, TaskCyclesSummary*> perPcSummaries;

void InitTaskProfiling(bool profileByPc) { perPcProfiling = profileByPc; }

std::unique_ptr<TaskProfiler> createTaskProfiler(void* taskPtr) {
    if (!perPcProfiling) {
        return std::make_unique<TaskProfiler>(&rootSummary, getCurCycleRef());
    } else {
        auto it = perPcSummaries.find(taskPtr);
        TaskCyclesSummary* pcSummary;
        if (it == perPcSummaries.end()) {
            pcSummary = new TaskCyclesSummary(&rootSummary);
            perPcSummaries.insert(std::make_pair(taskPtr, pcSummary));
        } else {
            pcSummary = it->second;
        }
        return std::make_unique<TaskProfiler>(pcSummary, getCurCycleRef());
    }
}

const TaskCyclesSummary& taskCyclesSummary() { return rootSummary; }

// Helper function to figure out what in the world we are calling. This is
// needed for manually-parallelized Swarm apps because of the bareRunner etc.
// wrappers, which make the actual function name too long to print. This is
// also used for SCC-parallelized apps to deal with the long generated
// function names. This code is tightly linked to the Swarm runtime and SCC.
static std::string functionName(void* pc) {
    const std::string rawFn = RTN_FindNameByAddress((ADDRINT)pc).c_str();

    // Strip off suffixes added by SCC
    // N.B. tasks derived from separate parts of the same original function
    // will result in the same name, which will at least be demangled.
    size_t pos = rawFn.find(".par");
    if (pos == string::npos) pos = rawFn.find(".outline");
    std::string origFn = (pos == string::npos) ? rawFn : rawFn.substr(0, pos);

    if (origFn.compare(0, 2, "_Z") == 0) {
        // Attempt to demangle
        int status;
        char* demangled = abi::__cxa_demangle(origFn.c_str(), 0, 0, &status);
        // Sometimes cxa_demangle can't demangle... looks like readelf or Pin
        // trims long names. Use mangled name in this case...
        if (demangled) {
            origFn = demangled;
            free(demangled);
        }
    }

    std::string res = origFn;

    if (origFn.find("Runner<") != -1UL) { //for non-SCC Swarm tasks
        class DelimiterCounter {
            std::vector<char> _brackets;         // <> () {}
            std::vector<uint32_t> _commaCounts;  // by bracket nesting level

          public:
            void pushLevel(std::string bracket) {
                assert(bracket.size() == 1);
                char c = bracket[0];
                _brackets.push_back(c);
                _commaCounts.push_back(0);
            }

            void popLevel() {
                _brackets.pop_back();
                _commaCounts.pop_back();
            }

            void comma() {
                _commaCounts.back()++;
            }

            uint32_t numCommas(uint32_t level) {
                return _commaCounts[level];
            }

            const std::vector<char>& brackets() {
                return _brackets;
            }
        };
        DelimiterCounter dc;

        // The function name we want is usually the second template argument to
        // the *Runner function template.  Unless it was a swarm::enqueueLambda
        // task, in which case we want the third template argument (the first
        // argument type, which is the lambda/functor type).
        uint32_t desiredPreceedingCommas =
            1 + (origFn.find("callLambdaFunc<") != -1UL ||
                 origFn.find("callLambdaPointer<") != -1UL);
        auto shouldCapture = [&]() {
            auto b = dc.brackets();
            auto it = std::find(b.begin(), b.end(), '<');
            return it != b.end() &&
                   dc.numCommas(it - b.begin()) == desiredPreceedingCommas;
        };

        boost::char_separator<char> sep("", ",<>(){}");
        boost::tokenizer<boost::char_separator<char>> tokens(origFn, sep);
        res = "";
        for (const auto& t : tokens) {
            bool preSc = shouldCapture();
            if (t == "<" || t == "(" || t == "{") dc.pushLevel(t);
            else if (t == ">" || t == ")" || t == "}") dc.popLevel();
            else if (t == ",") dc.comma();
            if (preSc && shouldCapture()) res += t;
        }

        boost::trim(res);
        if (res.size() == 0) res = origFn;
        assert(res.size());

        // Remove redundant & and/or wrapping ()
        boost::trim_left_if(res, [](char c) { return c == '&'; });
        while (res.size() > 2 && res[0] == '(' && res[res.size()-1] == ')') {
            res = res.substr(1, res.size()-2);
        }
    }

    const std::string retVoid = "void ";
    if (boost::starts_with(res, retVoid)) {
        res.erase(0, retVoid.size());
    }

    // Don't use so many precious characters for tasks in anonymous namespaces
    boost::replace_all(res, "(anonymous namespace)", "(anon)");

    // Trim size
    if (res.size() > 26) {
        res = res.substr(0, 23) + "...";
    }

    // Summarize runner type (helps diagnose performance issues)
    // N.B. If an inner detach is outlined multiple times, look at the end of
    //      the funciton name for the suffix that indicates the runner type
    size_t sccSuffixPos;
    if ((sccSuffixPos = rawFn.rfind('.')) != -1UL) {
        std::string sccSuffix = rawFn.substr(sccSuffixPos + 1);

        size_t d2cSuffixPos;
        if ((d2cSuffixPos = rawFn.rfind(".d2c")) != -1UL) {
            int detachKind = std::stoi(rawFn.substr(d2cSuffixPos + 4));
            char buf[3];
            sprintf(buf, "%2d", detachKind);
            res += buf;
        } else {
            res += " |";
        }

        if (sccSuffix.find("samehint") != -1UL) res += "S";
        else if (sccSuffix.find("hint") != -1UL) res += "H";
        else res += "N"; // NOHINT

        if (sccSuffix.find("bare") != -1UL) res += "B";
        else if (sccSuffix.find("reg") != -1UL) res += "R";
        else if (sccSuffix.find("mem") != -1UL) res += "M";
        else if (sccSuffix == "le") res += "L";
        else res += "?"; //panic("Unrecognized SCC runner type");
    } else if (origFn.find("bareRunner") != -1ul) res += " [B]";
    else if (origFn.find("regTupleRunner") != -1ul) res += " [R]";
    else if (origFn.find("memTupleRunner") != -1ul) res += " [M]";
    else res += " [N]";  // naked function call (spillers only?)

    return res;
}

template <class T> static inline T safeDiv(T a, T b) { return b ? a / b : 0; }

void printTaskProfilingInfo() {
    if (!perPcProfiling) {
        TaskCyclesSummary& s = rootSummary;
        info("Total committed tasks %9ld, cycles (idle, %12ld), (executed, %12ld), (waiting, %12ld)",
            s.commit.tasks, s.commit.idle, s.commit.exec, s.commit.wait);
        info("Total   aborted tasks %9ld, cycles (idle, %12ld), (executed, %12ld), (waiting, %12ld)",
            s.abort.tasks, s.abort.idle, s.abort.exec, s.abort.wait);
    } else {
        info("%12s  %30s  %9s %12s  %1s %22s  %9s %12s  %1s %22s  %19s", "", "",
             "Committed", "", "", "Committed Cycles", "Aborted", "", "", "Aborted Cycles",
             "Cycles/Task");
        info("%12s  %30s  %9s %12s %12s %12s  %9s %12s %12s %12s  %9s %9s",
             "PC", "Function", "Tasks", "Idle", "Executed", "Waiting", "Tasks",
             "Idle", "Executed", "Waiting", "Mean", "Max");

        // Sort by number of executed cycles
        using STup = std::tuple<void*, TaskCyclesSummary*>;
        std::vector<STup> pcVec;
        for (auto& it : perPcSummaries) {
            pcVec.push_back(std::make_tuple(it.first, it.second));
        }
        std::sort(pcVec.begin(), pcVec.end(), [](const STup& a, const STup& b) {
            auto weightFn = [](TaskCyclesSummary* t) {
                //return t->tasks;
                return t->commit.exec + t->abort.exec;
            };
            return weightFn(std::get<1>(a)) > weightFn(std::get<1>(b));
        });

        auto print = [](TaskCyclesSummary& s, const char* pc, const char* fn) {
            info("%12s  %30s  %9ld %12ld %12ld %12ld  %9ld %12ld %12ld %12ld  %9ld %9ld",
                pc, fn, s.commit.tasks, s.commit.idle, s.commit.exec, s.commit.wait,
                s.abort.tasks, s.abort.idle, s.abort.exec, s.abort.wait,
                safeDiv(s.commit.exec, s.commit.tasks), s.commit.maxExec);
        };

        for (auto& t : pcVec) {
            void* pc = std::get<0>(t);
            TaskCyclesSummary& s = *std::get<1>(t);
            char pcBuf[1024];
            snprintf(pcBuf, sizeof(pcBuf), "%p", pc);
            print(s, pcBuf, functionName(pc).c_str());
        }
        print(rootSummary, "Total", "");
    }
}
