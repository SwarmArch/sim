#!/bin/bash

# Prereqs: gcov and lcov installed

# Usage:
# 1. Compile simulator with scons mode=cov
# 2. Run lcov --base-directory . --directory . --zerocounters to clear all gcov files
# 3. Run tests (preferably with several config files); gcov is multiprocess-safe
# 4. Run this script

lcov --rc lcov_branch_coverage=1 --base-directory . --directory build/cov/sim/ -c -o coverage.info
lcov --rc lcov_branch_coverage=1 --remove coverage.info "/data/sanchez/*" -o coverage.info
lcov --rc lcov_branch_coverage=1 --remove coverage.info "/usr*" -o coverage.info
genhtml --rc lcov_branch_coverage=1 --rc genhtml_branch_coverage=1 -o lcov_html -t "simulator coverage" --num-spaces 4 coverage.info
rm coverage.info
