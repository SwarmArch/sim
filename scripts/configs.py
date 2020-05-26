#!/usr/bin/python
from __future__ import (absolute_import, division, print_function)

import itertools
import math
import os
#from simConfig import *
from genConfigs import ReadConfig, ExpandConfigs, TrimConfigNames, MB, Long

configsBase = [(ReadConfig("sample.cfg"), "")]
baseCores = configsBase[0][0]["sys.cores.cores"]
baseTiles = configsBase[0][0]["sys.cores.cores"]

coreConfigs = []
for cores in [1,2,4,7,16,64,160,256,512,1024]:
    tiles = max(1, cores // 4)
    mems = max(1, cores // 16)
    cfg = ExpandConfigs(configsBase, [
        ("sys.cores.cores", cores),
        ("sys.caches.l1d.caches", cores),
        ("sys.caches.l2.caches", tiles),
        ("sys.caches.l3.banks", tiles),
        ("sys.caches.l3.size", 2 * MB * tiles),
        ("sys.robs.robs", tiles),
        ("sys.net.nodes", tiles),
        ("sys.mem.controllers", mems)],
        mnemonic="%dc" % cores)
    coreConfigs.append(cfg[0])

multiplePfBtwnL1L2Configs = []
for cores in [baseCores]:
    tiles = max(1, cores // 4)
    mems = max(1, cores // 16)
    cfg = ExpandConfigs(configsBase, [
        ("sys.cores.cores", cores),
        ("sys.caches.l1d.caches", cores),
        ("sys.caches.l1d.parent", "pfL1ToL2"),
        ("sys.caches.pfL1ToL2.prefetchers", cores),
        ("sys.caches.pfL1ToL2.isPrefetcher", True),
        ("sys.caches.pfL1ToL2.parent", "l2"),
        ("sys.caches.l2.caches", tiles),
        ("sys.caches.l3.banks", tiles),
        ("sys.caches.l3.size", 2 * MB * tiles),
        ("sys.robs.robs", tiles),
        ("sys.net.nodes", tiles),
        ("sys.mem.controllers", mems)],
        mnemonic="%dc-multiplePfBtwnL1L2" % cores)
    multiplePfBtwnL1L2Configs.append(cfg[0])

multiplePfBtwnL2L3Configs = []
for cores in [baseCores]:
    tiles = max(1, cores // 4)
    mems = max(1, cores // 16)
    cfg = ExpandConfigs(configsBase, [
        ("sys.cores.cores", cores),
        ("sys.caches.l1d.caches", cores),
        ("sys.caches.l2.caches", tiles),
        ("sys.caches.l2.parent", "pfL2ToL3"),
        ("sys.caches.pfL2ToL3.prefetchers", tiles),
        ("sys.caches.pfL2ToL3.isPrefetcher", True),
        ("sys.caches.pfL2ToL3.parent", "l3"),
        ("sys.caches.l3.banks", tiles),
        ("sys.caches.l3.size", 2 * MB * tiles),
        ("sys.robs.robs", tiles),
        ("sys.net.nodes", tiles),
        ("sys.mem.controllers", mems)],
        mnemonic="%dc-multiplePfBtwnL2L3" % cores)
    multiplePfBtwnL2L3Configs.append(cfg[0])

cprConfigs = ExpandConfigs(configsBase, [("sys.cores.cores", baseTiles), ("sys.caches.l1d.caches", baseTiles)])

netNodes = [1<<x for x in range(int(math.log(baseTiles, 2))-1)]
netConfigs = ExpandConfigs(configsBase, [("sys.net.nodes", netNodes), ("sys.net.contention", [False, True])])
netConfigs += ExpandConfigs(configsBase, ("sys.net.xdim", 1)) # rings
# With multiple subnets, a sender-receiver pair's packets may be delivered out of order
netConfigs += ExpandConfigs(configsBase, ("sys.net.subnets", 4))

memConfigs = [
    ExpandConfigs(configsBase, [("sys.mem.type", "Simple")])[0],
    ExpandConfigs(configsBase, [("sys.mem.type", "LimitedSimple"),
                                ("sys.mem.bandwidth", 1000)])[0]]

mapperConfigs = ExpandConfigs(configsBase, [
    ("sys.robs.taskMapper.type", [
        "Random",
        "Hint",
        "ProportionalBucketTaskMapper",
        "Self",
        ]),
    # [mcj] the Stealing task balancer hasn't worked in 2018, maybe even 2017
    ("sys.robs.taskBalancer.type", ["None"]), #"Stealing"]),
    ])
# Min #tsb entries = 20: 16 to hold initial enqueues, 4 for other reserved slots (3 cores + spiller child)
tsbConfigs = ExpandConfigs(configsBase, ("sys.robs.tsbEntries", [20, 32, 128, 1024]))
gvtConfigs = ExpandConfigs(configsBase, ("sys.robs.gvtUpdatePeriod", [10, 100, 1000, 10000]))
tsarrayConfigs = ExpandConfigs(configsBase, ("sys.caches.l2.linesPerCanary", [1,8]))

queueConfigs = []
for tqCap in [#8, <-- this deadlocks always in 2017. It didn't in 2016
              16, 64, 1024, 100000]:
    cqCap = tqCap // 4
    overflow = 7 * tqCap // 8
    tiedCap = overflow - cqCap
    removableCap = min(tqCap - overflow, 2)
    cfg = ExpandConfigs(configsBase, [
            ("sys.robs.taskQ.capacity", tqCap),
            ("sys.robs.taskQ.overflow", overflow),
            ("sys.robs.taskQ.tiedCapacity", tiedCap),
            ("sys.robs.taskQ.removableCapacity", removableCap),
            ("sys.robs.commitQ.capacity", cqCap),
            ])
    queueConfigs.append(cfg[0])

admConfigs = ExpandConfigs(configsBase, ("sys.robs.commitQ.admissionPolicy", ["Any", "Half", "All"]))

simpleConfig = ExpandConfigs(configsBase, [("sys.cores.type", "Simple")])
sbConfig = ExpandConfigs(configsBase, [("sys.cores.type", "Scoreboard")])
oooConfig = ExpandConfigs(configsBase, [("sys.cores.type", "OoO")])

wideSbConfigs = ExpandConfigs(configsBase, [("sys.cores.type", "Scoreboard"),
                                            ("sys.cores.issueWidth", [2,4])])
wideOooConfigs = ExpandConfigs(configsBase, [("sys.cores.type", "OoO"),
                                             ("sys.cores.issueWidth", [2,4])])

mtConfigs = []
for threads in [2,4,8]:
    cfg = ExpandConfigs(configsBase, [
        ("sys.cores.type", "Scoreboard"),
        ("sys.cores.threads", threads),
        ])
    mtConfigs.append(cfg[0])
    cfg = ExpandConfigs(cfg, [
        ("sys.cores.issueWidth", 2),
        ("sys.robs.abortHandler.stallPolicy", "Adaptive"),
        ])
    mtConfigs.append(cfg[0])


priorityConfig = ExpandConfigs(configsBase, [
        ("sys.cores.type", "Scoreboard"),
        ("sys.cores.threads", 4),
        ("sys.cores.priority", "Timestamp"),
        ])

tiebreakConfigs = ExpandConfigs(configsBase, [
    ("sys.robs.tieBreakPolicy", "Enqueue"),
    # Enqueue + clearTieBreakerOnAbort isn't permitted
    ("sys.robs.clearTieBreakerOnAbort", False),
    ])
tiebreakConfigs += ExpandConfigs(configsBase, [
    ("sys.robs.tieBreakPolicy", ["Dequeue", "Lazy"]),
    ("sys.robs.clearTieBreakerOnAbort", [False, True]),
    ])

unselectiveAbortConfigs = ExpandConfigs(configsBase, [
    ("sys.robs.abortHandler.selectiveAborts", False),
    ("sys.robs.tieBreakPolicy", "Dequeue"),
    ])
unselectiveAbortConfigs += ExpandConfigs(configsBase, [
    ("sys.robs.abortHandler.selectiveAborts", False),
    ("sys.robs.tieBreakPolicy", "Enqueue"),
    # Enqueue + clearTieBreakerOnAbort isn't permitted
    ("sys.robs.clearTieBreakerOnAbort", False),
    ])

stallConfigs = ExpandConfigs(configsBase, [
    ("sys.robs.abortHandler.stallPolicy", ["Never", "Always", "Running", "Adaptive", "AdaptiveSingle"]),
    ])

bloomConfigs = ExpandConfigs(configsBase, [
        ("sys.robs.addressSet.type", "Bloom"),
        # One tiny BF to induce many false positives, and one regular
        ("sys.robs.addressSet.BITS", [8, 2048]),
        ("sys.robs.addressSet.K", 8),
        ])
cores = 512
tiles = max(1, cores // 4)
mems = max(1, cores // 16)
bloomConfigs += ExpandConfigs(configsBase, [
        ("sys.robs.addressSet.type", "Bloom"),
        ("sys.robs.addressSet.BITS", [8, 2048]),
        ("sys.robs.addressSet.K", 8),
        ("sys.cores.cores", cores),
        ("sys.caches.l1d.caches", cores),
        ("sys.caches.l2.caches", tiles),
        ("sys.caches.l3.banks", tiles),
        ("sys.caches.l3.size", 2 * MB * tiles),
        ("sys.robs.robs", tiles),
        ("sys.net.nodes", tiles),
        ("sys.mem.controllers", mems),
        ])

nonspecConfigs = ExpandConfigs(configsBase, [
        ("sys.robs.addressSet.type", "Bloom"),
        ("sys.robs.addressSet.BITS", 2048),
        ("sys.robs.addressSet.K", 8),
        ("sys.cores.cores", cores),
        ("sys.caches.l1d.caches", cores),
        ("sys.caches.l2.caches", tiles),
        ("sys.caches.l3.banks", tiles),
        ("sys.caches.l3.size", 2 * MB * tiles),
        ("sys.robs.robs", tiles),
        ("sys.robs.mayspecSpeculationMode", ["May", "Cant"]),
        ("sys.robs.serializeSpatialIDs", True),
        ("sys.robs.taskMapper.type", "Hint"),
        ("sys.net.nodes", tiles),
        ("sys.mem.controllers", mems),
        ])

ffConfig = ExpandConfigs(configsBase, [("sim.ffHeartbeats", Long(2 ** 64 - 1))])

frameConfigs = []
for frameDepth in [3, 8, 64, 100000]:
    cfg = ExpandConfigs(configsBase, [
            ("sys.robs.maxFrameDepth", frameDepth),
            ])
    frameConfigs.append(cfg[0])

allConfigs = list(itertools.chain(
        [(configsBase[0][0], "base")],
        coreConfigs,
        cprConfigs,
        netConfigs,
        memConfigs,
        mapperConfigs,
        tsbConfigs,
        gvtConfigs,
        queueConfigs,
        admConfigs,
        tsarrayConfigs,
        simpleConfig,
        sbConfig,
        oooConfig,
        wideSbConfigs,
        wideOooConfigs,
        mtConfigs,
        priorityConfig,
        tiebreakConfigs,
        unselectiveAbortConfigs,
        stallConfigs,
        bloomConfigs,
        nonspecConfigs,
        ffConfig,
        frameConfigs,
        multiplePfBtwnL1L2Configs,
        multiplePfBtwnL2L3Configs,
))

def conditionallyDisableSerializeSpatialIDs(cfg):
    """
    Random and Self mapping would never use hint serialization in real apps.
    Since serialization disables other features, disable it for those cases.
    """
    mapper = cfg['sys.robs.taskMapper.type']
    if mapper.lower() in ['random', 'self']:
        cfg['sys.robs.serializeSpatialIDs'] = False;


uniqueCfgs = {}
cfgDir = "configs/"
os.makedirs(cfgDir)
for (cfg, name) in TrimConfigNames(allConfigs):
    conditionallyDisableSerializeSpatialIDs(cfg)
    cfgTxt = str(cfg)
    if cfgTxt in uniqueCfgs:
        print("Skipping %s (same as %s)" % (name, uniqueCfgs[cfgTxt]))
        continue
    uniqueCfgs[cfgTxt] = name
    cfgFile = os.path.join(cfgDir, name + ".cfg")
    print("Saving", cfgFile)
    with open(cfgFile, "w") as f:
        print(cfgTxt, file=f)
print("Saved %d configs" % len(uniqueCfgs))
