#!/usr/bin/python
from __future__ import (absolute_import, division, print_function)

import argparse

import numpy as np
import h5py
import matplotlib
import matplotlib.pyplot as plt

np.set_printoptions(threshold=np.nan)

parser = argparse.ArgumentParser()
parser.add_argument("--desiredBuckets", "-b", type=int, default=[512, 64], nargs="+", help="Desired bucket size for heatmap") 
parser.add_argument("--outputFile", "-o", type=str, default="sim", help="Output filename")
parser.add_argument("--ignoreBuckets", "-i", action="store_true", default=False, help="No bucket stats, ignore it")
parser.add_argument("--sortBuckets", "-s", action="store_true", default=False, help="Sort buckets in heatmap")
args = parser.parse_args()


# Global state
f = h5py.File("sim-tm.h5", "r")
stats = f["stats"]["task-mapper"]

xmin = 0 
xmax = stats["cycles"][-1]
xmin_lim, xmax_lim = xmin, xmax
num_samples = 10000

# Select which samples you want. Trace stats must always call this
def sampleStat(stat):
    global num_samples
    # Pick a rate that includes the whole range and includes num_samples samples
    assert len(stat) >= 2
    smin = np.searchsorted(stats["cycles"], xmin, side="left")
    smax = np.searchsorted(stats["cycles"], xmax, side="right")
    while stats["cycles"][smin+1] >= xmin and smin > 0: smin -= 1
    if smax == smin:
        if smin > 0: smin -= 1
        else: smax += 1
    rate = max(1, (smax - smin + 1) // num_samples)
    return stat[smin:smax+1:rate]

def drawBucketCommits():
    bucketLoads  = stats[-1]["task-mapper"]["bucketLoads"][0]
    plt.plot([i for i in range(len(bucketLoads))], bucketLoads)
    plt.xlabel('Bucket')
    plt.ylabel('Bucket Load')
    plt.savefig(args.outputFile + '-bucketLoads-line.png')
    plt.close()
    plt.scatter([i for i in range(len(bucketLoads))], bucketLoads)
    plt.savefig(args.outputFile + '-bucketLoads-scatter.png')
    plt.close()

def getHeatMapStats(d, toSort):
    bucketLoads  = np.diff(sampleStat(stats)["task-mapper"]["bucketLoads"], axis=0)
    bucketLoads  = bucketLoads.reshape(bucketLoads.shape[0], bucketLoads.shape[2])
    samples     = bucketLoads.shape[0]
    bins        = bucketLoads.shape[1]

    desiredBuckets = d 
    assert(bins % desiredBuckets == 0)
    if (bins == desiredBuckets): buckets = bucketLoads
    else:
        reshaped = bucketLoads.reshape(samples, bins // desiredBuckets, desiredBuckets)
        buckets  = reshaped.sum(axis=1)

    if toSort:
        buckets = buckets[:, buckets.sum(axis=0).argsort()]

    samples = buckets.shape[0]
    bins    = buckets.shape[1]
    buckets.shape = (samples, bins)
    tasksPerSample = buckets.sum(axis=1)
    #assert(np.unique(tasksPerSample).shape[0] <= 2)     # Only the last sample can have lesser tasks
    return np.transpose(buckets)

def drawHeatMap(d, toSort):
    data = getHeatMapStats(d, toSort) 

    fig, ax = plt.subplots()
    heatmap = ax.pcolor(data, cmap=matplotlib.cm.OrRd)
    cbar = plt.colorbar(heatmap)
    cycles_per_sample = int(np.mean(np.diff(sampleStat(stats["cycles"]))))
    plt.xlabel('Samples (Approximate cycles per sample:' + str(cycles_per_sample) + ')')
    plt.savefig(args.outputFile + '-' + str(d) + 'b-' + ('sorted-' if toSort else 'nominal-') + 'heatMap.png')
    plt.close()

def drawRobLoads():
    # Overall RoB loads
    robLoads        = stats[-1]["task-mapper"]["robLoads"][0]
    totalCycles     = np.sum(robLoads, axis=0)
    avgCyclesPerRob = totalCycles / robLoads.shape[0]
    robLoads        = robLoads / avgCyclesPerRob

    # Per phase loads
    robLoads        = np.diff(sampleStat(stats)["task-mapper"]["robLoads"], axis=0)
    robLoads        = robLoads.reshape(robLoads.shape[0], robLoads.shape[2])
    robLoads        = robLoads[~np.all(robLoads == 0, axis=1)]                          # Remove all zero rows (ie. phase where there were no commits -- which is only the last phase)

    # Coalesce rob phase loads
    samples         = robLoads.shape[0]
    # if coalesceFactor > 1:
    #     # I want to sum every coalesceFactor rows. reshape followed by sum does not work
    #     # Note that sum(x[m+1], .... , x[m+N]) = cumsum(till x[m+N]) - cumsum(till x[m])
    #     # 1. Get cumsum --> Slice to get rows N-1, 2N-1, 3N-1, ...
    #     # 2. Get the difference with the previous row
    #     # This automatically ignores the last few samples, if #samples % coalesceFactor != 0
    #     # There might be an easier way to do this...
    #     result      = np.cumsum(robLoads,0)[coalesceFactor-1::coalesceFactor]
    #     result[1:] -= result[:-1]
    #     robLoads    = result

    avgCycles       = np.sum(robLoads, axis=1) / robLoads.shape[1]
    robLoads        = np.true_divide(robLoads, avgCycles[:, None])                      # true_divide so we retain the float
    meanLoads       = np.average(robLoads, axis=1, weights=robLoads.astype(bool))       # only take mean over non-zero values
    maxLoads        = np.amax(robLoads, axis=1)
    minLoads        = np.amin(np.ma.masked_equal(robLoads, 0), axis=1).data             # min of non-zero loads
    err             = np.array([meanLoads-minLoads, maxLoads-meanLoads])
    phase           = np.array(list(range(1, robLoads.shape[0]+1)))
    cycles_per_sample = int(np.mean(np.diff(stats["cycles"])))
    plt.xlabel('Phase (Approximate cycles per sample:' + str(cycles_per_sample) + ')')
    plt.ylabel('rob-load')
    plt.errorbar(phase, meanLoads, yerr=err)
    plt.savefig(args.outputFile + '-robLoads.png')
    plt.close()


def drawPlots():
    if not args.ignoreBuckets:
        drawBucketCommits() 
        for d in args.desiredBuckets:
            drawHeatMap(d, False)
            if args.sortBuckets:
                drawHeatMap(d, True)
    drawRobLoads()

if __name__ == "__main__":
    drawPlots()
