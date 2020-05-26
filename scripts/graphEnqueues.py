#!/usr/bin/pypy -u
from __future__ import (absolute_import, division, print_function)

import argparse
import collections
try:
    import graphviz as gv
except ImportError:
    gv = None

import parseTrace


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("-t", "--trace", type=str, default="trace.out",
            help="trace file")
    parser.add_argument("--aborted", default=False, action='store_true',
            help="graph aborted tasks instead of committed ones (helps analyze aborts)")
    args = parser.parse_args()

    tasks, _ = parseTrace.getTasks(args.trace)
    print("Trace read")

    # Build enqueue graph
    taskFns = set()
    taskEnqueues = {}
    for t in tasks[1:]:
        if t.outcomeCycle == -1: continue # partial trace
        taskFns.add(t.taskFn)
        isCommit = t.outcome[0] == "Commit"
        if isCommit == args.aborted: continue
        parentFn = tasks[t.parent].taskFn if t.parent else None
        if parentFn not in taskEnqueues:
            taskEnqueues[parentFn] = collections.Counter()
        taskEnqueues[parentFn][t.taskFn] += 1
    taskFns = sorted(taskFns)

    print("\nEnqueues")
    print("%16s %s" % ("Parent\Child PC", ' '.join("%16x" % child for child in taskFns)))
    print("%16s %s" % ("No parent", ' '.join(
            "%16d" % taskEnqueues[None][child] for child in taskFns)))
    for parent in taskFns:
        if parent in taskEnqueues:
            print("%16s %s" % ("%16x" % parent, ' '.join(
                    "%16d" % taskEnqueues[parent][child] for child in taskFns)))

    if gv:
        graph = gv.Digraph(format="png")
        for child in taskFns:
            if taskEnqueues[None][child]:
                graph.edge("No parent", format(child, 'x'),
                           label=str(taskEnqueues[0][child]))
        for parent in taskFns:
            if parent in taskEnqueues:
                for child in taskFns:
                    if taskEnqueues[parent][child]:
                        graph.edge(format(parent, 'x'), format(child, 'x'),
                                   label=str(taskEnqueues[parent][child]))
        graph.render("enqueue_graph.gv", view=False)
        print("enqueue_graph.gv.png written.")
    else:
        print("graphviz not found")
