#!/usr/bin/python
from __future__ import (absolute_import, division, print_function)

import argparse
import resource
import sys

import matplotlib
matplotlib.use("Agg")

import matplotlib.pyplot as plt
import matplotlib.patches as mpatches

import parseTrace


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("-t", "--trace", type=str, default="trace.out",
            help="Trace filename. (Default: trace.out)")
    parser.add_argument("start_cycle", metavar="START", type=int, default=0, nargs='?',
            help="The earliest cycle that will be included. (Default: 0)")
    parser.add_argument("num_cycles", metavar="LENGTH", type=int, default=None, nargs='?',
            help="The number of cycles to include. " +
                 "If not specified, all cycles until the end of the trace will be used.")
    parser.add_argument("-n", "--no-edges", action="store_true", default=False,
            help="Do not draw outlines around tasks.")
    parser.add_argument("-l", "--hide-legend", action="store_true", default=False,
            help="Do not produce a legend.")
    parser.add_argument("-r", "--rect-height", type=float, default=0.8,
            help="Height of task rectangles as fraction of y-axis core spacing. (Default: 0.8)")
    parser.add_argument("-s", "--size", type=float, default=5.0,
            help="Set the rough height of the plot in nominal inches. (Default: 5.0)")
    parser.add_argument("-a", "--aspect-ratio", type=float, default=2.0,
            help="Aspect ratio of plot. (Default: 2.0)")
    parser.add_argument("-o", "--output-filename", type=str, default="timeline",
            help="Filename for timeline image. (Default: timeline)")

    args = parser.parse_args()

    assert(args.start_cycle >= 0)

    print("Reading trace...")
    tasks, lastCycle = parseTrace.getTasks(args.trace)
    print("Trace read.")

    tasks = [t for t in tasks[1:] if
             # only look at real tasks
             t.outcomeCycle >= 0 and t.startCycle >= 0 and
             # only look at tasks that might show up in the plot
             t.finalCycle() >= args.start_cycle and
             (not args.num_cycles or t.startCycle < args.start_cycle + args.num_cycles)
            ]

    # Determine task functions, cores, and range of cycles to plot
    # victory: I don't know if the heartbeats thread always takes thread ID #1,
    #          or what else might affect the mapping of thread IDs.
    #          To be on the safe side, I'll build my own mapping of core IDs here.
    taskFns = set()
    threadIDs = set()
    for t in tasks:
        taskFns.add(t.taskFn)
        threadIDs.add(t.tid)
    taskFns = sorted(taskFns)
    threadIDs = sorted(threadIDs)
    numCores = len(threadIDs)
    threadID2CoreMap = {}
    for idx, tid in enumerate(threadIDs):
        threadID2CoreMap[tid] = idx
    if args.num_cycles:
        lastCycle = min(lastCycle, args.start_cycle + args.num_cycles - 1)
    assert(lastCycle > args.start_cycle)

    # Pick colors and other visual options
    colormap = plt.get_cmap('gist_earth' if args.no_edges else 'gist_rainbow') # avoid red if using red for aborted tasks.
    def color(taskFn):
        return colormap(float(taskFns.index(taskFn)) / len(taskFns))[:3]
    def darken(color):
        return (color[0] / 2.0, color[1] / 2.0, color[2] / 2.0)
    def lighten(color):
        return (color[0] / 2.0 + 0.5, color[1] / 2.0 + 0.5, color[2] / 2.0 + 0.5)
    def facecolor(taskFn, is_aborted):
        if args.no_edges and is_aborted:
            return 'r'
        return lighten(color(taskFn)) if is_aborted else color(taskFn)
    def linewidth(is_aborted):
        if args.no_edges:
            return 0.0
        return 1.0 if is_aborted else 0.1
    def linestyle(is_aborted):
        if args.no_edges:
            return "solid"
        return "dotted" if is_aborted else "solid"
    def edgecolor(is_aborted):
        if args.no_edges:
            return "none"
        return 'r' if is_aborted else 'k'

    print("Plotting cycles %d through %d." % (args.start_cycle, lastCycle))
    print(numCores, "cores.")
    print(len(taskFns), "task functions:")
    for taskFn in taskFns:
        print(hex(taskFn), "has color", color(taskFn))

    # Set up the axes
    fig = plt.figure(figsize=(2*args.aspect_ratio*args.size, args.size))
    ax = fig.add_subplot(121)
    ax.set_xlabel("Cycles")
    ax.set_xlim([args.start_cycle, lastCycle + 1])
    ax.set_ylabel("Cores")
    ax.set_ylim([-1, numCores - 0.25])
    ax.set_aspect((lastCycle + 1 - args.start_cycle) / (numCores * args.aspect_ratio))
    ax.spines['top'].set_visible(False)
    ax.spines['right'].set_visible(False)
    ax.spines['left'].set_visible(False)
    ax.xaxis.set_ticks_position('bottom')
    ax.yaxis.set_ticks_position('left')
    ax.yaxis.set_tick_params(direction='out', length=0)
    ax.set_yticks(list(range(numCores)))

    # Now plot the tasks
    for idx, t in enumerate(tasks):
        # Determine how the task ended up
        assert(t.outcome[0] in ["Abort", "Commit"])
        is_aborted = (t.outcome[0] == "Abort")

        ax.add_patch(mpatches.Rectangle(
                (t.startCycle, threadID2CoreMap[t.tid]-args.rect_height/2.0), #(x, y) of lower left corner
                t.finalCycle() - t.startCycle + 1, #width
                args.rect_height,
                facecolor=facecolor(t.taskFn, is_aborted),
                linewidth=linewidth(is_aborted),
                linestyle=linestyle(is_aborted),
                edgecolor=edgecolor(is_aborted)
                ))

        sys.stdout.write("%d/%d (%d%%) tasks plotted. Peak resident memory use: %d MB\r" %
                         (idx+1, len(tasks), 100*(idx+1)//len(tasks),
                          resource.getrusage(resource.RUSAGE_SELF).ru_maxrss // 1024))

    # Now add the legend
    if not args.hide_legend:
        legend_patches = []
        legend_labels = []
        for is_aborted in [False, True]:
            for taskFn in taskFns:
                legend_patches.append(mpatches.Patch(
                        facecolor=facecolor(taskFn, is_aborted),
                        linewidth=linewidth(is_aborted),
                        linestyle=linestyle(is_aborted),
                        edgecolor=edgecolor(is_aborted)
                        ))
                legend_labels.append(hex(taskFn) if is_aborted else "")
        plt.legend(legend_patches, legend_labels,
                   bbox_to_anchor=(1.05, 1), loc=2, ncol=2,
                   title="committed   aborted                       .")

    print("\nPlotting complete. Rendering and saving image...")

    # Save plot as image
    if args.output_filename.endswith(".png"):
        args.output_filename = args.output_filename[:-4]
    fig.savefig(args.output_filename, bbox_inches='tight')

    print("Done.")
