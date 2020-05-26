from __future__ import (absolute_import, division, print_function)

import sys

class Task(object):
    def __init__(self, cycle, parent, taskFn):
        self.enqueueCycle = cycle
        self.startCycle = -1
        self.finishCycle = -1 # might never be set if task aborted while running.
        self.parent = parent
        self.taskFn = taskFn
        self.tid = -1
        self.outcomeCycle = -1 # should be set for all tasks, unless it is a partial trace.
        self.outcome = None # ("Commit/Yield/Spill", 0) or ("Abort", succUid -- can be 0 if not requeued)
        self.abortCause = None # ("Data/Resource/Exception", ...)
        self.abortedTasks = []

    def finalCycle(self):
        return self.finishCycle if self.finishCycle != -1 else self.outcomeCycle

    def executed(self):
        if self.startCycle == -1:
            return 0
        else:
            return self.finalCycle() - self.startCycle

def getTasks(traceFile):
    trace = open(traceFile, "r")
    # NOTE: Tasks are refered to in the trace by IDs starting from 1, with an
    # ID of 0 to referring to no task.  (See sim/trace.cpp)  So to allow
    # indexing into this tasks list using IDs, we start the list with a None.
    tasks = [None]

    cycle = 0
    for line in trace:
        fields = line.strip().split(" ")
        cycle = int(fields[0])
        type = fields[1]
        uid = int(fields[2])

        if type == "T" or type == "Y": # new task (or yield)
            assert uid == len(tasks)
            tasks.append(Task(cycle, int(fields[3]), int(fields[4])))
        elif type == "B": # start
            tasks[uid].startCycle = cycle
            tasks[uid].tid = int(fields[3])
        elif type == "E": # finish
            tasks[uid].finishCycle = cycle
        elif type == "C": # commit
            tasks[uid].outcomeCycle = cycle
            tasks[uid].outcome = ("Commit", fields[3])
        elif type == "A": # abort
            tasks[uid].outcomeCycle = cycle
            newUid = int(fields[3])
            if newUid != 0:
                assert newUid == len(tasks)
                tasks.append(Task(cycle, uid, tasks[uid].taskFn))
            tasks[uid].outcome = ("Abort", newUid)
        elif type == "S": # spill
            tasks[uid].outcomeCycle = cycle
            tasks[uid].outcome = ("Spill", 0)
        elif type in ["M", "Z"]: # steal or discard
            # nothing to do for now
            a = 1
        elif type == "D": # data dependence abort
            if tasks[uid].abortCause == None:
                aborterUid = int(fields[3])
                tasks[uid].abortCause = ("Data", aborterUid, int(fields[4]), int(fields[5]), fields[6], fields[7])
                assert aborterUid > 0
                tasks[aborterUid].abortedTasks.append(uid)
        elif type == "X": # parent-child abort
            if tasks[uid].abortCause == None:
                tasks[uid].abortCause = ("Parent",)
                assert tasks[uid].parent > 0
                tasks[tasks[uid].parent].abortedTasks.append(uid)
        elif type == "R":
            if tasks[uid].abortCause == None:
                tasks[uid].abortCause = ("Resource",)
        elif type == "F":
            if tasks[uid].abortCause == None:
                tasks[uid].abortCause = ("Zooming",)
        else:
            print("Unrecognized event type:", type, line.strip())
            sys.exit(1)

    return tasks, cycle

