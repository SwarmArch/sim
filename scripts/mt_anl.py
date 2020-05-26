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
parser.add_argument("--multiThread", "-m", action="store_true", default=False, help="Single threaded analysis")
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
cols = 6 if args.multiThread else 4
fig = plt.figure(figsize=(6*cols, 2.2*rows))
gs = gridspec.GridSpec(rows, cols)
xmin = 0
xmax = stats["cycles"][-1]
xmin_lim, xmax_lim = xmin, xmax


spans = None # not read, but refs are saved here to avoid garbage-collecting span widgets

# FIXME: Should dump names in stats...
Idle = 0
Issued = 1
StallSb = 3
StallTQ = 4
StallCQ = 6
Empty = 7

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

def groupPerROB(cs):
    samples = cs.shape[0]
    cores = cs.shape[1]
    classes = cs.shape[2]
    robs = len(stats[-1]["rob"])

    rs = np.zeros((samples, robs, classes), dtype = cs.dtype)
    for r in range(robs):
        rs[:, r, :] = np.sum(cs[:, (r*cores//robs):((r+1)*cores//robs), :], axis = 1)
    return rs

def getCoreStatByType(s,t):
    ds = sampleStat(stats)["core"]['ctx']
    numContexts = len(ds[-1][0])
    # Only called for single thread analysis
    assert numContexts == 1
    totalCycles = np.full_like(ds[s]['notask'][:,:,0],0)
    for i in range(0,numContexts):
        totalCycles += ds[s][t][:,:,i]
    cs = np.diff(totalCycles, axis=0)

    # Group per ROB
    return groupPerROB(cs)

def getCoreStat(s):
    taskTypes = ['notask','worker','requeuer','spiller','irrevocable']
    ds = sampleStat(stats)["core"]['ctx']
    numContexts = len(ds[-1][0])
    totalCycles = np.full_like(ds[s]['notask'][:,:,0],0)
    for tt in taskTypes:
        for i in range(0,numContexts):
            totalCycles += ds[s][tt][:,:,i]
    cs = np.diff(totalCycles, axis=0)

    # Group per ROB
    return groupPerROB(cs)

def getRobStat(s):
    return np.diff(sampleStat(stats)["rob"][s], axis=0)

def getAvailableThreadCount():
    hasTT = True
    try:
      tt = stats["threadThrottler"]
    except:
      hasTT = False
    if (hasTT):
        startThreads = stats["threadThrottler"]["availableThreads"][-1][0]
        arr = sampleStat(stats)["threadThrottler"]["availableThreads"]
        arr_remove_first_sample = np.delete(arr, 0, 0)
        return arr_remove_first_sample
    else: # returns 1 of suitable dimension
        arr = sampleStat(stats)["rob"]
        temparr = np.ones(arr.shape)
        temparr_remove_first_sample = np.delete(temparr, 0, 0)
        return temparr_remove_first_sample

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
    colNum = 1

    if not args.multiThread:
        workerExec = getCoreStatByType("cycles","worker")[:,:,Issued]
        workerMem = getCoreStatByType("cycles","worker")[:,:,StallSb]
        irrevExec = getCoreStatByType("cycles","irrevocable")[:,:,Issued]
        irrevMem = getCoreStatByType("cycles","irrevocable")[:,:,StallSb]
        spiller = np.sum(getCoreStatByType("cycles", "spiller"),axis=2)
        requeuer = np.sum(getCoreStatByType("cycles", "requeuer"),axis=2)
        stallTQ = sum([getCoreStatByType("cycles",tt)[:,:,StallTQ] \
                            for tt in ["worker","irrevocable"]])
        stallCQ = getCoreStatByType("cycles","notask")[:,:,StallCQ]
        stallEmpty = getCoreStatByType("cycles","notask")[:,:,Empty]

        # TODO More asserts
        assert np.sum(getCoreStatByType("cycles","worker")[:,:,StallCQ]) == 0

        allCycles = np.sum(reduce(lambda x, y:\
            x+y, [getCoreStatByType("cycles", s) for s in
                  ["notask", "worker", "requeuer", "spiller", "irrevocable"]
                  ]
            ), axis = -1)
        breakdownSum = sum((workerExec,workerMem,spiller,requeuer,\
                            irrevExec, irrevMem, stallTQ,stallCQ,stallEmpty))

        if not (breakdownSum <= allCycles).all():
            breakdownSum = np.minimum(breakdownSum, allCycles)
            assert (breakdownSum <= allCycles).all()
        other = allCycles - breakdownSum

        cyclesPerSample = 1.0*np.diff(sampleStat(stats["cycles"]))
        ys = [b / cyclesPerSample[:,np.newaxis]
                for b in [workerExec, workerMem, irrevExec, irrevMem,
                          spiller, requeuer, stallEmpty, stallTQ,
                          stallCQ, other]
                ]
        x = 1.0*sampleStat(stats["cycles"])
        x = x[1:]
        colors = ['green', 'teal', 'cyan','blue', 'yellow', 'grey', 'black', 'orange',
                  'purple', 'magenta']
        axes = drawTrace(x, ys, colors, gs, 0)
        axes[0].set_title("Cycle breakdown")
        legendProxies = [plt.Rectangle((0, 0), 1, 1, fc=c) for c in colors]
        axes[0].legend(legendProxies, ["worker", "workermem", "irrev",
                                       "irrevmem", "spill", "requeue",
                                       "stallEmpty", "stallTQ", "stallCQ",
                                       "other"],
                       ncol = 2, loc = 'lower right',
                       framealpha = 0.3, fontsize = 9)
        for i in range(len(axes)): axes[i].set_ylabel("Tile %d core cycles" % i)
        axes[-1].set_xlabel("Cycles")
        allAxes += axes
    else:
        committedInstrs = getCoreStat("committedCycles")[:,:,Issued]
        stalledCycles   = getCoreStat("cycles")[:,:,StallSb]
        abortedCycles   = getCoreStat("abortedDirectCycles")[:,:,Issued] + \
                          getCoreStat("abortedIndirectCycles")[:,:,Issued]
        # 1a. Show committed cycles
        ys = [committedInstrs]
        x = 1.0*sampleStat(stats["cycles"])
        x = x[1:]
        colors = ['green']
        axes = drawTrace(x, ys, colors, gs, 0)
        axes[0].set_title("Committed Instructions")
        legendProxies = [plt.Rectangle((0, 0), 1, 1, fc=c) for c in colors]
        axes[0].legend(legendProxies, ["committed"],
                       ncol = 2, loc = 'lower right',
                       framealpha = 0.3, fontsize = 9)
        for i in range(len(axes)): axes[i].set_ylabel("Tile %d core cycles" % i)
        axes[-1].set_xlabel("Cycles")
        allAxes += axes

        # 1b. Show stalled cycles
        ys = [stalledCycles]
        x = 1.0*sampleStat(stats["cycles"])
        x = x[1:]
        colors = ['orange']
        axes = drawTrace(x, ys, colors, gs, colNum)
        colNum += 1
        axes[0].set_title("Stalled Cycles")
        legendProxies = [plt.Rectangle((0, 0), 1, 1, fc=c) for c in colors]
        axes[0].legend(legendProxies, ["stalled"],
                ncol = 2, loc = 'lower right', framealpha = 0.3, fontsize = 9)
        for i in range(len(axes)): axes[i].set_ylabel("Tile %d core cycles" % i)
        axes[-1].set_xlabel("Cycles")
        allAxes += axes


        # 1c. Show aborted cycles
        ys = [abortedCycles]
        x = 1.0*sampleStat(stats["cycles"])
        x = x[1:]
        colors = ['red']
        axes = drawTrace(x, ys, colors, gs, colNum)
        colNum += 1
        axes[0].set_title("Aborted Cycles")
        legendProxies = [plt.Rectangle((0, 0), 1, 1, fc=c) for c in colors]
        axes[0].legend(legendProxies, ["aborted"],
                ncol = 2, loc = 'lower right', framealpha = 0.3, fontsize = 9)
        for i in range(len(axes)): axes[i].set_ylabel("Tile %d core cycles" % i)
        axes[-1].set_xlabel("Cycles")
        allAxes += axes

    # 2. Commits and aborts
    taskAborts = getRobStat("abrt")
    taskCommits = getRobStat("taskcom")
    spillCommits = getRobStat("spillcom")

    ys = [taskCommits, spillCommits, taskAborts]
    colors = ['#31B404', '#104297', '#FF8000']
    axes = drawTrace(x, ys, colors, gs, colNum)
    colNum += 1
    axes[0].set_title("Task commits/aborts")
    legendProxies = [plt.Rectangle((0, 0), 1, 1, fc=c) for c in colors]
    axes[0].legend(legendProxies, ["taskCommits", "spillCommits", "taskAborts"],
            ncol = 3, loc = 'upper right', framealpha = 0.3, fontsize = 9)
    for i in range(len(axes)): axes[i].set_ylabel("ROB %d tasks" % i)
    axes[-1].set_xlabel("Cycles")
    allAxes += axes

    # 3. Task, commit queue lengths
    runQ = getAvgQueueLength("runQLength")
    commitQ = getAvgQueueLength("commitQLength")
    taskQ = runQ + getAvgQueueLength("execQLength") + commitQ

    print('runQ average', runQ.mean(), "std. dev", runQ.std())
    print('taskQ average', taskQ.mean(), "std. dev", taskQ.std())
    print('commitQ average', commitQ.mean(), "std. dev", commitQ.std())

    ys = [commitQ, taskQ-commitQ]
    colors = ['#ff6600', '#104297']
    axes = drawTrace(x, ys, colors, gs, colNum)
    colNum += 1
    axes[0].set_title("Queue lengths")
    legendProxies = [plt.Rectangle((0, 0), 1, 1, fc=c) for c in colors]
    axes[0].legend(legendProxies, ["commitQ", "taskQ"],
            ncol = 2, loc = 'upper right', framealpha = 0.3, fontsize = 9)
    for i in range(len(axes)): axes[i].set_ylabel("ROB %d tasks" % i)
    axes[-1].set_xlabel("Cycles")
    allAxes += axes

    if args.multiThread:
        # 4. Number of available threads
        threadCount = getAvailableThreadCount()
        ys = [threadCount]
        colors = ['#31B404']
        axes = drawTrace(x, ys, colors, gs, colNum)
        colNum += 1
        axes[0].set_title("Available thread count")
        legendProxies = [plt.Rectangle((0, 0), 1, 1, fc=c) for c in colors]
        axes[0].legend(legendProxies, ["availableThreadCount"],
                ncol = 3, loc = 'upper right', framealpha = 0.3, fontsize = 9)
        for i in range(len(axes)): axes[i].set_ylabel("ROB %d threads" % i)
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

