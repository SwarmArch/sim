#!/usr/bin/python

# Builds a clang compilation database from the output of scons. Use with:
#    scons -n --clang | ./misc/build_compilation_db.py
#
# CAVEAT: Trivial, but tuned to our build process. So for now don't expect

from __future__ import (absolute_import, division, print_function)
import json, os, sys

directory = os.path.abspath(".")
db = []

for line in sys.stdin:
    if line.startswith("clang") and ' -c ' in line:
        line = line.strip()
        entry = { "directory" : directory, "command" : line, "file" : line.split(" ")[-1] }
        db.append(entry)

dbfilename = "compile_commands.json"
dbfile = open(dbfilename, "w")
print(json.dumps(db, indent=1), file=dbfile)
dbfile.close()
