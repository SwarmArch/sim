#!/usr/bin/pypy -u
# Before running this, you might need to do:
# sudo apt install pypy pypy-six
from __future__ import (absolute_import, division, print_function)

import argparse
import bisect
from functools import reduce
import subprocess

from six.moves import zip_longest

import parseTrace

parser = argparse.ArgumentParser()
parser.add_argument("-p", "--program", type=str, default="",
        help="program binary")
parser.add_argument("-t", "--trace", type=str, default="trace.out",
        help="trace file")
parser.add_argument("-w", "--wcycles", default=False, action="store_true",
        help="report parallelism-weighted cycles (expensive for long traces)")
args = parser.parse_args()

tasks, finalCycle = parseTrace.getTasks(args.trace)
print("Trace read")

# Account for idle cores by accounting their cycles into running tasks
if args.wcycles:
    deltas = []
    for t in tasks[1:]:
        if t.startCycle == -1: continue
        deltas.append((t.startCycle, +1))
        if t.finalCycle() == -1: continue # partial trace
        deltas.append((t.finalCycle(), -1))
    deltas.sort()

    cycleTasks = [(0,0)]
    for (cycle, diff) in deltas:
        (lastCycle, ntasks) = cycleTasks[-1]
        newEntry = (cycle, ntasks + diff)
        if lastCycle == cycle or (len(cycleTasks) > 1 and cycleTasks[-2][1] == ntasks):
            cycleTasks[-1] = newEntry
        else:
            cycleTasks.append(newEntry)

    maxActiveTasks = max(ntasks for cycles, ntasks in cycleTasks)

    print("Trace processed")

    def avgActiveTasks(startCycle, finishCycle):
        if finishCycle == startCycle: finishCycle += 1 # HACK FIXME
        assert finishCycle > startCycle
        # Use nonzero number of task to get the right cycle counts on equality
        b = bisect.bisect_left(cycleTasks, (startCycle, 10000)) - 1
        e = bisect.bisect_right(cycleTasks, (finishCycle, 10000))
        #print cycleTasks[b], cycleTasks[e]

        taskcycles = 0
        lastCycle = startCycle
        lastTasks = cycleTasks[b][1]
        for i in range(b+1,e):
            (cycle, tasks) = cycleTasks[i]
            taskcycles += (cycle - lastCycle) * lastTasks
            lastCycle = cycle
            lastTasks = tasks
        taskcycles += (finishCycle - lastCycle) * lastTasks
        if taskcycles == 0: taskcycles = 1 # HACK FIXME (can happen with superscalar cores?)
        assert taskcycles
        return 1. * taskcycles / (finishCycle - startCycle)

    def taskweight(t):
        return maxActiveTasks / avgActiveTasks(t.startCycle, t.finalCycle())
else:
    def taskweight(t): return 1.0

# Per-PC analysis
execStats = {}
commitExec = 0
abortExec = 0
for t in tasks[1:]:
    if t.outcomeCycle == -1: continue # partial trace
    execCycles = t.executed()
    wCycles = execCycles * taskweight(t)
    if t.taskFn not in execStats:
        execStats[t.taskFn] = [0,]*8 # committed tasks/cycles/wcycles, aborted tasks/cycles/wcycles, max cycles/wcycles
    if t.outcome[0] == "Commit":
        execStats[t.taskFn][0] += 1
        execStats[t.taskFn][1] += execCycles
        execStats[t.taskFn][2] += wCycles

        execStats[t.taskFn][6] = max(execStats[t.taskFn][6], execCycles)
        execStats[t.taskFn][7] = max(execStats[t.taskFn][7], wCycles)
        commitExec += execCycles
    elif t.outcome[0] == "Abort":
        execStats[t.taskFn][3] += 1
        execStats[t.taskFn][4] += execCycles
        execStats[t.taskFn][5] += wCycles
        abortExec += execCycles
    else:
        # Spills/steals
        assert execCycles == 0

execLst = [(pc, vals) for u, pc, vals in sorted([(-vals[1], pc, vals) for pc, vals in execStats.items()])]
totalCycles = reduce(lambda a, b: [x + y for x, y in zip(a, b)], [vals[:6] for pc, vals in execLst])
maxCycles = reduce(lambda a, b: [max(x, y) for x, y in zip(a, b)], [vals[6:] for pc, vals in execLst])

if args.wcycles:
    print("Per-task breakdown (WCycles = cycles weighted to include idle cores)")
    print("%16s   %9s %12s %12s   %9s %12s %12s   %12s %12s" % ("Task PC", "ComTasks", "ComCycles", "ComWCycles","AbTasks", "AbCycles", "AbWCycles", "MaxCycles", "MaxWCycles"))
    for pc, vals in execLst:
        print("%16x   %9d %12d %12d   %9d %12d %12d   %12d %12d" % ((pc,) + tuple(vals)))
    print("%16s   %9d %12d %12d   %9d %12d %12d   %12d %12d" % (("Total",) + tuple(totalCycles + maxCycles)))
else:
    print("Per-task breakdown")
    print("%16s   %9s %12s   %9s %12s   %12s" % ("Task PC", "ComTasks", "ComCycles", "AbTasks", "AbCycles", "MaxCycles"))
    for pc, vals in execLst:
        print("%16x   %9d %12d   %9d %12d   %12d" % ((pc,) + tuple(vals[:2] + vals[3:5] + vals[6:7])))
    print("%16s   %9d %12d   %9d %12d   %12d" % (("Total",) + tuple(totalCycles[:2] + totalCycles[3:5] + maxCycles[:-1])))

print("\nAvg parallelism %.2f (%.1f%% committed cycles)" % (1. * commitExec / finalCycle, 100. * commitExec / (commitExec + abortExec)))

# Task length distribution
taskLengths = {}
for t in tasks[1:]:
    if t.outcomeCycle == -1: continue # partial trace
    if t.outcome[0] != "Commit": continue
    execCycles = t.executed()
    if t.taskFn not in taskLengths:
        taskLengths[t.taskFn] = [execCycles]
    else:
        taskLengths[t.taskFn].append(execCycles)

def percentile(lst, pct):
    if len(lst) == 0: return 0
    if len(lst) == 1: return lst[0]
    pos = pct * (len(lst) - 1) / 100.
    # Interpolate two closest values
    l = int(pos) # rounds down
    u = l + 1
    w = pos - l
    return lst[l] * (1. - w) + lst[u] * w

def lenDist(lst):
    ll = sorted(lst)
    lmean = sum(ll) * 1. / len(ll)
    lmax = ll[-1]
    pcts = [50., 90., 99., 99.9, 99.99, 99.999]
    lps = [percentile(ll, p) for p in pcts]
    return (lmean,) + tuple(lps) + (lmax,)

print("\nTask length distribution")
print("%16s   %12s %12s %12s %12s %12s %12s %12s %12s" % ("Task PC", "Mean", "Median", "90p","99p", "99.9p", "99.99p", "99.999p", "Max"))
for pc, vals in execLst:
    if pc not in taskLengths: continue
    print("%16x   %12d %12d %12d %12d %12d %12d %12d %12d" % ((pc,) + lenDist(taskLengths[pc])))
allTaskLengths = reduce(lambda x, y: x + y, list(taskLengths.values()))
print("%16s   %12d %12d %12d %12d %12d %12d %12d %12d" % (("Total",) + lenDist(allTaskLengths)))


# Abort analysis
def findTotalAbortCycles(uid):
    res = [tasks[uid].executed()]
    cascadeCycles = []
    for cid in tasks[uid].abortedTasks:
        assert tasks[cid].abortCause[0] in ["Parent", "Data"]
        if tasks[cid].abortCause[0] == "Data" and tasks[cid].abortCause[2] != 0:
            # This is a direct data abort (nonzero aborterPc), so not part of this cascade
            continue
        # otherwise, this is either a parent-child abort, or a data abort with
        # PC=0 (which means the parent was aborting, hence part of the cascade)
        cascadeCycles.append(findTotalAbortCycles(cid))

    # Sum across all, filling in with zeros
    cascadeTotals = [sum(x) for x in zip_longest(*cascadeCycles, fillvalue=0)]
    #print res, cascadeCycles, cascadeTotals, res + cascadeTotals
    return res + cascadeTotals

def sumFill(lists):
    return [sum(x) for x in zip_longest(*lists, fillvalue=0)]

def addr2line(pc, maxLen):
    if len(args.program):
        line = subprocess.check_output(["addr2line", hex(pc), "-e", args.program],
                                       universal_newlines=True
                                      ).split("\n")[0].strip()
        if len(line) > maxLen:
            line = "..." + line[-(maxLen-3):]
        return line
    else:
        return ""

abortStats = {}
resourceAbortCycles = []
zoomingAbortCycles = []
for uid in range(1, len(tasks)):
    if tasks[uid].outcomeCycle == -1: continue # partial trace
    if tasks[uid].abortCause == None: continue
    if tasks[uid].abortCause[0] == "Resource":
        resourceAbortCycles = sumFill([findTotalAbortCycles(uid), resourceAbortCycles])
        continue
    if tasks[uid].abortCause[0] == "Zooming":
        zoomingAbortCycles = sumFill([findTotalAbortCycles(uid), zoomingAbortCycles])
        continue
    if tasks[uid].abortCause[0] == "Parent": continue
    assert tasks[uid].abortCause[0] == "Data"
    aborterPc = tasks[uid].abortCause[2]
    if aborterPc == 0: continue

    cycles = findTotalAbortCycles(uid)
    if aborterPc not in abortStats:
        abortStats[aborterPc] = cycles
    else:
        abortStats[aborterPc] = sumFill([abortStats[aborterPc], cycles])

print("\nRoot causes of aborts")
print("%16s %44s %12s %s" % ("PC", "Line", "Abort cycles", "Abort cycles by depth"))
abortLst = [(pc, vals) for u, pc, vals in sorted([(-sum(vals), pc, vals) for pc, vals in abortStats.items()])]
for pc, vals in abortLst:
    print("%16x %44s %12d %s" % (pc, addr2line(pc,44), sum(vals), str(vals)))

totalAbortCycles = sumFill(list(abortStats.values()))
print("%16s %44s %12d %s" % ("Total data", "", sum(totalAbortCycles), str(totalAbortCycles)))
print("%16s %44s %12d %s" % ("Resource", "", sum(resourceAbortCycles), str(resourceAbortCycles)))
print("%16s %44s %12d %s" % ("Zooming", "", sum(zoomingAbortCycles), str(zoomingAbortCycles)))


# Another view: task->task direct conflict aborts
taskAbortStats = {}
for uid in range(1, len(tasks)):
    if tasks[uid].outcomeCycle == -1: continue # partial trace
    if tasks[uid].abortCause == None: continue
    if tasks[uid].abortCause[0] == "Data":
        aborterPc = tasks[uid].abortCause[2]
        if aborterPc == 0: continue # skip cascading data aborts
        aborter = tasks[uid].abortCause[1]
        rwset = tasks[uid].abortCause[4]
        rw = tasks[uid].abortCause[5]
        name = "%x @ %x -> %x    %s %s" % (tasks[aborter].taskFn, aborterPc, tasks[uid].taskFn, rw, rwset)
    elif tasks[uid].abortCause:
        name = "%x  %8s" % (tasks[uid].taskFn, tasks[uid].abortCause[0])
    else:
        continue
    if name not in taskAbortStats:
        taskAbortStats[name] = [0, 0]
    taskAbortStats[name][0] += tasks[uid].executed()
    taskAbortStats[name][1] += sum(findTotalAbortCycles(uid))

print("\nBreakdown of abort root causes")
abortLst = [(name, direct, -ntotal) for ntotal, name, direct in sorted([(-vals[1], name, vals[0]) for name, vals in taskAbortStats.items()])]
print("%44s %10s %10s" % ("Aborter -> Abortee      Type", "DCycles", "TCycles"))
for name, direct, total in abortLst:
    print("%44s %10d %10d" % (name, direct, total))
dtotal = sum([direct for name, direct, total in abortLst])
ttotal = sum([total for name, direct, total in abortLst])
print("%44s %10d %10d" % ("Total", dtotal, ttotal))
