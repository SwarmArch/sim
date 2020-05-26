#!/usr/bin/python
from __future__ import (absolute_import, division, print_function)

import os, re, sys

#dryRun = True
dryRun = False

srcs = sys.argv[1:]

def sortIncludes(lines, fname):
    def prefix(l):
        if l.find("<") >= 0:
            return "2"
            # if you want to differentiate...
            #if l.find(".h") >= 0: return "2" # C system headers
            #else: return "3" # C++ system headers
        else:
            if l.find('"' + fname + '.') >= 0: return "1" # Our own header, rel path
            if l.find('/' + fname + '.') >= 0: return "1" # Our own header, abs path
            return "4" # Program headers

    ll = [prefix(l) + l.strip() for l in lines if len(l.strip()) > 0]
    sl = [l[1:] for l in sorted(ll)]
    if lines[-1].strip() == "": sl += [""]
    #print sl
    return sl

# Canonicalize include paths so that they start from this directory
def processIncludes(lines, srcFile):
    res = []
    for line in lines:
        m = re.search(r'"(.*)"', line)
        if m:
            headerFile = m.group(1)
            if not os.path.exists(headerFile):
                relFile = os.path.join(os.path.dirname(srcFile), headerFile)
                if os.path.exists(relFile):
                    # With symlinks, "../..", etc, resolved
                    substFile = os.path.relpath(os.path.realpath(relFile), ".")
                    print("Substituting", headerFile, "->", substFile)
                    line = re.sub(headerFile, substFile, line)
        res.append(line)
    return res

for src in srcs:
    f = open(src, 'r+')  # we open for read/write here to fail early on read-only files
    txt = f.read()
    f.close()

    bName = os.path.basename(src).split(".")[0]
    print(bName)

    lines = [l for l in txt.split("\n")]

    includeBlocks = []
    blockStart = -1
    for i in range(len(lines)):
        l = lines[i].strip()
        isInclude = l.startswith("#include") and l.find("NOLINT") == -1
        isEmpty = l == ""
        if blockStart == -1:
            if isInclude: blockStart = i  # start block
        else:
            if not (isInclude or isEmpty): # close block
                includeBlocks.append((blockStart, i))
                blockStart = -1

    print(src, len(includeBlocks), "blocks")
    newIncludes = [(s , e, sortIncludes(processIncludes(lines[s:e], src), bName)) for (s, e) in includeBlocks]
    for (s , e, ii) in newIncludes:
        # Print?
        if ii == lines[s:e]:
            print("Block in lines %d-%d matches" % (s, e-1))
            continue
        for i in range(s, e):
            print("%3d: %s%s | %s" % (i, lines[i], " "*(45 - len(lines[i][:44])), ii[i-s] if i-s < len(ii) else ""))
        print("")

    prevIdx = 0
    newLines = []
    for (s , e, ii) in newIncludes:
        newLines += lines[prevIdx:s] + ii
        prevIdx = e
    newLines += lines[prevIdx:]

    if not dryRun and len(includeBlocks):
        outTxt = "\n".join(newLines)
        f = open(src, 'w')
        f.write(outTxt)
        f.close()

print("Done!")
