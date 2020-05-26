from __future__ import (absolute_import, division, print_function)
from simConfig import SimConfig, Long

# A heuristic-based function to produce short config names that can be used as directory names
def getPropName(prop, val):
    if prop == "sys.numCores":
        return str(val) + "c"
    elif prop == "sim.numThreads":
        return str(val) + "t"
    elif prop.endswith(".size"):  # when configuring a cache level, start with the size and it'll look nice
        szStr =  "%dKB" % (val // 1024,)
        cacheLevel = prop.split(".")[2]
        return cacheLevel + "-" + szStr
    elif prop.endswith("dir.coverage"):
        szStr =  "%dKB" % (val // 1024,)
        cacheLevel = prop.split(".")[2] + "dir"
        return cacheLevel + "-" + szStr
    elif isinstance(val, str):
        return val # string values are descriptive enough to not need the property name
    elif isinstance(val, bool):
        propName = prop.split(".")[-1]
        if not val:
            return "no" + propName[0].capitalize() + propName[1:]
        else:
            return propName
    else:
        return prop.split(".")[-1] + "-" + str(val)

def ReadConfig(file):
    fh = open(file, 'r')
    cfgTxt = fh.read()
    fh.close()
    cfg = SimConfig()
    cfg.parse(cfgTxt)
    return cfg

def ExpandConfigs(configs, paramListVal, silent=False, mnemonic = None):
    newConfigs = []
    # syntactic sugar, auto-wrap in list if not a list
    if not isinstance(paramListVal, list):
        paramListVal = [paramListVal]
    for (config, name) in configs:
        #print paramListVal
        (prop, vals) = paramListVal[0]
        if not isinstance(vals, list):  # syntactic sugar
            vals = [vals]
        for val in vals:
            newConfig = config.clone()
            if val is not None:
                newConfig[prop] = val
            else:
                if newConfig.haskey(prop):
                    del newConfig[prop]
            newName = name
            if not silent:
                if not name == "": newName += "-"
                newName += getPropName(prop, val) if mnemonic is None else mnemonic
            newConfigs.append((newConfig, newName))
    if len(paramListVal) == 1:
        return newConfigs
    else:
        # expand next property
        return ExpandConfigs(newConfigs, paramListVal[1:], silent=silent if mnemonic is None else True)

def TrimConfigNames(configs):
    # Eliminate common substrings
    patterns = {}
    for (cfg, name) in configs:
        props = name.split("-")
        for prop in props:
            if not prop in patterns: patterns[prop] = 0
            patterns[prop] += 1
    numCfgs = len(configs)

    def shortName(name):
        res = "-".join(prop for prop in name.split("-") if patterns[prop] < numCfgs)
        if len(res) == 0: return  "-".join([name.split("-")[0], name.split("-")[-2], name.split("-")[-1]])
        return res

    return [(cfg, shortName(name)) for (cfg, name) in configs]

## Common multiplier defs
KB = 1024
MB = KB*KB
GB = MB*KB

K = 1000
M = K*K
B = M*K

