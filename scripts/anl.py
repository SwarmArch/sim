#!/usr/bin/python
from __future__ import (absolute_import, division, print_function)

import argparse
from functools import reduce

import h5py
import numpy as np

parser = argparse.ArgumentParser()
parser.add_argument("--interactive", "-i", action="store_true", default=False, help="Run in interactive mode")
parser.add_argument("--numRows", "-r", type=int, default=4, help="Number of rows = #ROBs")
parser.add_argument("--outputFile", "-o", type=str, default="sim", help="Output filename")
parser.add_argument("--inputFile", "-f", type=str, default="sim.h5", help="Input filename")
args = parser.parse_args()

# NOTE: Backend must be set right after first matplotlib include
import matplotlib
matplotlib.use("WXAgg" if args.interactive else "Agg")

import matplotlib.pyplot as plt
import matplotlib.gridspec as gridspec
from matplotlib.widgets import SpanSelector

# Global state
f = h5py.File(args.inputFile, "r")
stats = f["stats"]["root"]

rows = args.numRows
cols = 4 # if you add more analyses, add more columns...
fig = plt.figure(figsize=(6*cols, 2*rows))
gs = gridspec.GridSpec(rows, cols)
xmin = 0
xmax = stats["cycles"][-1]
xmin_lim, xmax_lim = xmin, xmax


spans = None # not read, but refs are saved here to avoid garbage-collecting span widgets

# FIXME: Should dump names in stats...
(Idle,
 Exec,
 ExecToAbort,
 StallEmpty,
 StallResource,
 StallThrottle,
 StallConflict,
 ) = list(range(7))

# Select which samples you want. Trace stats must always call this
def sampleStat(stat):
    # Pick a rate that includes the whole range and includes ~200 samples
    assert len(stat) >= 2
    smin = np.searchsorted(stats["cycles"], xmin, side="left")
    smax = np.searchsorted(stats["cycles"], xmax, side="right")
    while stats["cycles"][smin+1] >= xmin and smin > 0: smin -= 1
    if smax == smin:
        if smin > 0: smin -= 1
        else: smax += 1
    rate = max(1, (smax - smin + 1) // 200)
    return stat[smin:smax+1:rate]

def getCoreStat(c, s):
    # Dims: sample, core, class (Idle, etc)
    cs = np.diff(sampleStat(stats)["core"][c][s], axis=0)

    # Group per ROB
    samples = cs.shape[0]
    cores = cs.shape[1]
    classes = cs.shape[2]
    robs = stats[-1]["rob"].shape[0]

    rs = np.zeros((samples, robs, classes), dtype = cs.dtype)
    for r in range(robs):
        rs[:, r, :] = np.sum(cs[:, (r*cores//robs):((r+1)*cores//robs), :], axis = 1)
    return rs


def getCoreStatsOfTrio(ct, tt, state):
    if isinstance(tt, str):
        return getCoreStat(ct, tt)[:, :, state]
    else:
        return sum([getCoreStat(ct, t)[:, :, state] for t in tt])


def getRobStat(s):
    return np.diff(sampleStat(stats)["rob"][s], axis=0)

def getAvgQueueLength(s):
    qs = getRobStat(s)
    normStat = np.array(qs[:, :, 1], dtype=np.double) / (np.array(qs[:, :, 0], dtype=np.double)+1e-15)
    # Repair zero values (due to nothing changing)
    for i in range(1, qs.shape[0]):
        for j in range(qs.shape[1]):
            if qs[i,j,0] == 0:
                normStat[i,j] = normStat[i-1,j]
    return normStat

def drawTrace(x, ys, colors, gs, col):
    plots = []
    axes = []
    totals = []
    nplots = ys[0].shape[1]
    for i in range(nplots):
        if not len(axes):
            ax = plt.subplot(gs[i, col])
        else:
            ax = plt.subplot(gs[i, col], sharex = axes[0])
        if i < nplots - 1:
            plt.setp(ax.get_xticklabels(), visible=False)
        axes.append(ax)
        base = np.zeros(len(x))
        axplots = []
        for j in range(len(ys)):
            #print ys[j].shape
            y = ys[j][:, i]
            plot = ax.fill_between(x, base, base + y, facecolor=colors[j], interpolate=True)
            base += y
            axplots.append(plot)
        totals.append(base)
        plots.append(axplots)
    for ax in axes:
        ax.axis([xmin, xmax, 0, max([max(t) for t in totals])])
    return axes

def drawPlots():
    allAxes = []

    # dims : sample, rob, class
    allCycles = np.sum(reduce(lambda x, y:
        x+y, [getCoreStat("cycles", s) for s in
              ["notask", "worker", "requeuer", "spiller", "irrevocable"]
              ]
        ), axis = -1)

    # 1. Show cycle breakdown
    stallTQ = getCoreStatsOfTrio("committedCycles",
                                 ["worker", "irrevocable"], StallResource)
    stallCQ = getCoreStatsOfTrio("cycles", "notask", StallResource)
    stallEmpty = getCoreStatsOfTrio("cycles", "notask", StallEmpty)
    stallThrottle = getCoreStatsOfTrio("cycles", "notask", StallThrottle)

    spiller = np.sum(getCoreStat("cycles", "spiller")[:, :, :], axis=-1)
    requeuer = np.sum(getCoreStat("cycles", "requeuer")[:, :, :], axis=-1)

    workerExec = sum([getCoreStatsOfTrio("cycles", ["worker", "irrevocable"], s)
                      for s in [Exec, ExecToAbort]])
    workerCommit = getCoreStatsOfTrio("committedCycles",
            ["worker", "irrevocable"], Exec)
    if not (workerCommit <= workerExec).all():
        workerCommit = np.minimum(workerCommit, workerExec)
        assert (workerCommit <= workerExec).all()
    workerCommitInstrs = getCoreStatsOfTrio("committedInstrs",
            ["worker", "irrevocable"], Exec)
    if not (workerCommitInstrs <= workerCommit).all():
        workerCommitInstrs = np.minimum(workerCommitInstrs, workerCommit)
        assert (workerCommitInstrs <= workerCommit).all()
    workerCommitMem = workerCommit - workerCommitInstrs
    assert (workerCommitMem <= workerExec).all()

    breakdownSum = sum((workerExec, spiller, requeuer, stallEmpty,
                        stallTQ, stallCQ, stallThrottle))
    if not (breakdownSum <= allCycles).all():
        breakdownSum = np.minimum(breakdownSum, allCycles)
        assert (breakdownSum <= allCycles).all()
    other = allCycles - breakdownSum

    cyclesPerSample = 1.0*np.diff(sampleStat(stats["cycles"]))
    ys = [b / cyclesPerSample[:,np.newaxis]
            for b in [workerExec - workerCommitMem, workerCommitMem, spiller,
                      requeuer, stallEmpty, stallTQ,
                      stallCQ, stallThrottle, other]
            ]
    x = 1.0*sampleStat(stats["cycles"])
    x = x[1:]
    #print xmin, xmax, x[0], x[-1]
    colors = ['green', 'teal', 'blue', 'yellow', 'grey', 'black', 'orange',
              'purple', 'magenta']
    axes = drawTrace(x, ys, colors, gs, 0)
    axes[0].set_title("Cycle breakdown")
    legendProxies = [plt.Rectangle((0, 0), 1, 1, fc=c) for c in colors]
    axes[0].legend(legendProxies, ["worker", "mem", "spill", "requeue",
                                   "stallEmpty", "stallTQ", "stallCQ",
                                   "stallThrot", "other"],
                   ncol = 2, loc = 'lower right',
                   framealpha = 0.3, fontsize = 9)
    for i in range(len(axes)): axes[i].set_ylabel("Tile %d core cycles" % i)
    axes[-1].set_xlabel("Cycles")
    allAxes += axes

    # 2. Show cycle breakdown
    # OMG why is this so duplicated
    stallRes = stallTQ + stallCQ + stallThrottle
    workerAbortDirect = np.sum(
            getCoreStat("abortedDirectCycles", "worker")[:, :, :], axis=-1)
    workerAbortIndirect = np.sum(
            getCoreStat("abortedIndirectCycles", "worker")[:, :, :], axis=-1)
    workerAbort = workerExec - workerCommit
    if not (workerAbortDirect <= workerAbort).all():
        workerAbortDirect = np.minimum(workerAbortDirect, workerAbort)
        assert (workerAbortDirect <= workerAbort).all()
    if not (workerAbortIndirect <= workerAbort - workerAbortDirect).all():
        workerAbortIndirect = np.minimum(workerAbortIndirect,
                                         workerAbort - workerAbortDirect)

    breakdowns = [workerCommit, workerAbort,#workerAbortDirect, workerAbortIndirect,
                  spiller, requeuer,
                  stallEmpty, stallRes]
    breakdownSum = sum(breakdowns)
    if not (breakdownSum <= allCycles).all():
        breakdownSum = np.minimum(breakdownSum, allCycles)
        assert (breakdownSum <= allCycles).all()
    other = allCycles - breakdownSum

    cyclesPerSample = 1.0*np.diff(sampleStat(stats["cycles"]))
    ys = [b / cyclesPerSample[:,np.newaxis] for b in (breakdowns + [other])]
    x = 1.0*sampleStat(stats["cycles"])
    x = x[1:]
    #print xmin, xmax, x[0], x[-1]
    colors = ['green', 'red', 'blue', 'yellow', 'grey', 'orange', 'magenta']
    axes = drawTrace(x, ys, colors, gs, 1)
    axes[0].set_title("Cycle breakdown")
    legendProxies = [plt.Rectangle((0, 0), 1, 1, fc=c) for c in colors]
    axes[0].legend(legendProxies, ["commit", "abort", "spill", "requeue", "stallEmpty", "stallRes", "other"],
            ncol = 2, loc = 'lower right', framealpha = 0.3, fontsize = 9)
    for i in range(len(axes)): axes[i].set_ylabel("Tile %d core cycles" % i)
    axes[-1].set_xlabel("Cycles")
    allAxes += axes


    # Show task queue utilization
    taskQ = getAvgQueueLength("runQLength") + getAvgQueueLength("execQLength") + getAvgQueueLength("commitQLength")
    commitQ = getAvgQueueLength("commitQLength")

    ys = [commitQ, taskQ-commitQ]
    colors = ['#ff6600', '#104297']
    axes = drawTrace(x, ys, colors, gs, 2)
    axes[0].set_title("Queue lengths")
    legendProxies = [plt.Rectangle((0, 0), 1, 1, fc=c) for c in colors]
    axes[0].legend(legendProxies, ["commitQ", "taskQ"],
            ncol = 2, loc = 'upper right', framealpha = 0.3, fontsize = 9)
    for i in range(len(axes)): axes[i].set_ylabel("ROB %d tasks" % i)
    axes[-1].set_xlabel("Cycles")
    allAxes += axes

    # 4. Commits and aborts
    cascadingAborts = getRobStat("abrtFP")
    totalAborts = getRobStat("abrt")
    if not (cascadingAborts <= totalAborts).all():
        totalAborts = np.maximum(totalAborts, cascadingAborts)
        assert (cascadingAborts <= totalAborts).all()
    directAborts = totalAborts - cascadingAborts
    taskCommits = getRobStat("taskcom")
    spillCommits = getRobStat("spillcom")

    #ys = [commits, directAborts, cascadingAborts, throttles]
    ys = [taskCommits, spillCommits, directAborts, cascadingAborts]
    colors = ['#31B404', '#104297', '#FF8000', '#DF0101']
    axes = drawTrace(x, ys, colors, gs, 3)
    axes[0].set_title("Task commits/aborts")
    legendProxies = [plt.Rectangle((0, 0), 1, 1, fc=c) for c in colors]
    axes[0].legend(legendProxies, ["taskCommits", "spillCommits", "directAborts", "cascadingAborts"],
            ncol = 3, loc = 'upper right', framealpha = 0.3, fontsize = 9)
    for i in range(len(axes)): axes[i].set_ylabel("ROB %d tasks" % i)
    axes[-1].set_xlabel("Cycles")
    allAxes += axes
    return allAxes

def drawInteractive():
    fig.clf()
    allAxes = drawPlots()
    spans = []

    def onselect(_xmin, _xmax):
        global xmin, xmax, spans
        xmin = _xmin
        xmax = _xmax
        spans = drawInteractive()
        fig.canvas.draw()

    for ax in allAxes:
        span = SpanSelector(ax, onselect, 'horizontal', useblit=True,
            rectprops=dict(alpha=0.3, facecolor='blue'))
        spans.append(span)
    return spans

if __name__ == "__main__":
    if args.interactive:
        spans = drawInteractive()
        gs.tight_layout(fig) # expensive, only needed once

        def onkeypress(event):
            global xmin, xmax, spans
            #print event.key
            xmid = (xmin + xmax)/2.0
            w = (xmax - xmin)/2.0
            if event.key == 'down':
                xmin = max(xmin_lim, xmid - 2*w)
                xmax = min(xmax_lim, xmid + 2*w)
            elif event.key == 'up':
                if xmax - xmin > 1.0:
                    xmin += w/2.0
                    xmax -= w/2.0
            elif event.key == "left":
                d = min(xmin - xmin_lim, w)
                xmin -= d
                xmax -= d
            elif event.key == "right":
                d = min(xmax_lim - xmax, w)
                xmin += d
                xmax += d
            elif event.key == "r":
                xmin = xmin_lim
                xmax = xmax_lim
            #print xmin, xmax
            spans = drawInteractive()
            fig.canvas.draw()

        fig.canvas.mpl_connect('key_press_event', onkeypress)


        plt.show()
    else:
        drawPlots()
        gs.tight_layout(fig)
        plt.savefig(args.outputFile + '-trace.png')

