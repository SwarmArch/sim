#!/bin/bash
# dsm: Flatten the source tree... I'm tired of the extra keystrokes and this org messes up with my muscle memory

mkdir -p src/
find sim -name "*.h" | sort | xargs -n1 -IX ln -s `pwd`/X src/
find sim -name "*.cpp" | sort | xargs -n1 -IX ln -s `pwd`/X src/
