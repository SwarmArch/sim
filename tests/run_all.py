#!/usr/bin/python
from __future__ import (absolute_import, division, print_function)

import sys
import os
import subprocess as sp
import multiprocessing
import tempfile
import shutil
import argparse
import signal
import time
from ctypes import cdll

import statscheck

parser = argparse.ArgumentParser()
parser.add_argument("-d", "--dir", type=str,
        default=os.path.dirname(os.path.realpath(__file__)),
        help="tests directory")
parser.add_argument("-b", "--rundir", type=str,
        # TODO(mcj) point to /data/scratch/<user> if on the csail cluster
        # but /tmp/<user>/test_runs is better than an AFS directory
        default="/tmp/{}/test_runs".format(os.environ['USER']),
        help="base directory for test runs")
parser.add_argument("-c", "--configs", type=str,
        default=["sample.cfg"],
        nargs='+',
        help="config files")
parser.add_argument("-t", "--test", nargs='+', default=[],
        help="select one or more specific tests")
parser.add_argument("-e", "--exclude", nargs='+', default=[],
        help="select one or more specific tests to exclude")
parser.add_argument("-s", "--slowsim",
        default=False, action="store_true",
        help="use slowsim to run tests")
parser.add_argument("-a", "--abspaths",
        default=False, action="store_true",
        help="give absolute paths for failing run commands (o/w, all paths are relative to cwd)")
parser.add_argument("-v", "--verbose",
        default=False, action="store_true",
        help="print out more information")
parser.add_argument("--workers", type=int,
        default=0,
        help="Workers for pmap calls")
parser.add_argument("-r", "--repeats", type=int,
        default=1,
        help="Repeats per test")

args = parser.parse_args()

# Call from an arbitrary process to ensure it terminates if the parent does
def autoterm():
    PR_SET_PDEATHSIG = 1
    SIGTERM = 15
    result = cdll['libc.so.6'].prctl(PR_SET_PDEATHSIG, SIGTERM)
    if result != 0:
        print("ERROR: prctl() failed")
        sys.exit(0)

def run_and_verify(argList):
    (progname, cmdlist) = argList
    suffix = "_" + "".join(progname.split())
    temp_dir = tempfile.mkdtemp(dir=args.rundir, suffix=suffix)
    #print 'Execute {} in {}'.format(' '.join(cmdlist), temp_dir)

    # As stdout can be very large when DEBUG flags are enabled, dump to file, do
    # not buffer in memory.
    cout_path = os.path.join(temp_dir, 'cout.log')
    cerr_path = os.path.join(temp_dir, 'cerr.log')
    with open(cout_path, 'w') as cout, open(cerr_path, 'w') as cerr:
        p = sp.Popen(cmdlist, stdout=cout, stderr=cerr, cwd=temp_dir, preexec_fn=autoterm)
        p.wait()

    if args.abspaths:
        cmd = ' '.join(cmdlist)
    else:
        def relpath(p):
            if len(p) and p[0] == "/":
                rp = "./" + os.path.relpath(p)
                if len(rp) < len(p): return rp
            return p
        cmd = ' '.join([relpath(x) for x in cmdlist])

    verified = 'Verify: OK'
    verify_fail = 'Verify: Incorrect'
    signalRegex = 'contextchange.*signal.*[0-9]\+'
    if sp.call(['grep', verify_fail, cout_path], stdout=sp.PIPE) == 0:
        retVal = (progname, "FAIL: Found {} in stdout".format(verify_fail), cmd, temp_dir)
    elif sp.call(['grep', verified, cout_path], stdout=sp.PIPE) != 0:
        retVal = (progname, "FAIL: Could not find {} in stdout".format(verified), cmd, temp_dir)
    elif sp.call(['grep', '-i', 'stalled for', cout_path, cerr_path],
                 stdout=sp.PIPE) == 0:
        retVal = (progname, "FAIL: Simulator stalled", cmd, temp_dir)
    elif sp.call(['grep', '-i', signalRegex, cout_path, cerr_path],
                 stdout=sp.PIPE) == 0:
        retVal = (progname, "FAIL: Signal raised", cmd, temp_dir)
    elif not statscheck.succeeds(temp_dir):
        # FIXME(mcj) be specific about the stat failure
        retVal = (progname, "FAIL: Stats check failure", cmd, temp_dir)
    else:
        retVal = (progname, "OK", cmd, temp_dir)

    if retVal[1] is "OK":
        shutil.rmtree(temp_dir)
    else:
        sp.call(['bzip2', cout_path])
        sp.call(['bzip2', cerr_path])

    return retVal



class Sampler(object):
    def __init__(self, nsamples):
        self.nsamples = nsamples
        self.samples = []
        self.lastSample = 0
        self.sum = 0.0

    def sample(self, val):
        if len(self.samples) < self.nsamples:
            self.samples.append(val)
        else:
            self.sum -= self.samples[self.lastSample]
            self.samples[self.lastSample] = val
            self.lastSample = (self.lastSample + 1)  % len(self.samples)
        self.sum += val

    def estimate(self):
        return self.sum / len(self.samples)

testsDone = 0
tSampler = Sampler(200)

def print_error_info(dir):
    def cmd(c):
        p = os.popen(c)
        r = p.read()
        p.close()
        return r
    def bk(s, prefix):
        s = s.replace("\n", "\n" + prefix)
        return prefix + s
    llo = cmd("bzgrep -E 'assertion|Panic|Deadlock|Pin\)' %s | tail -5" % (dir + "/cout.log.bz2",)).strip()
    if len(llo): print(bk(llo, " [out] "))
    lle = cmd("bzgrep -E 'assertion|Panic|Deadlock|Pin\)' %s | tail -5" % (dir + "/cerr.log.bz2",)).strip()
    if len(lle): print(bk(lle, " [err] "))
    if len(llo) + len(lle) == 0:
        lle = cmd("bzcat %s | tail -1" % (dir + "/cerr.log.bz2",)).strip()
        if len(lle): print(bk(lle, " [err] "))

def print_result(argList):
    global testsDone, ntests, tstart, tSampler
    testsDone += 1
    (progname, status, cmd, runDir) = argList
    if status != "OK" or args.verbose:
        if not args.verbose: print("") # Don't follow status line
        print(progname, '.'*(60 - len(progname) - len(status)), status)
    if status != "OK":
        print(" Command:", cmd)
        print(" Directory:", runDir)
        print_error_info(runDir)
    if not args.verbose:
        # Print status line (important to keep it fixed-width)
        pct = 100.0 * testsDone / ntests
        timeElapsed = time.time() - tstart
        timePerTest = timeElapsed / testsDone
        tSampler.sample(timePerTest)
        timeLeft = (ntests - testsDone) * tSampler.estimate()
        header = "Completed %d tests" % testsDone
        trailer = "%3.1f%% | %.0fs left" % (pct, timeLeft)
        sys.stdout.write("\r%s %s %s" % (header, '.'*(60 - len(header) - len(trailer)), trailer))
        sys.stdout.flush()


# Parallel map procedure
# dsm: Based on http://noswap.com/blog/python-multiprocessing-keyboardinterrupt
def __init_pmap_worker():
    autoterm()
    signal.signal(signal.SIGINT, signal.SIG_IGN)

def pmap(func, iterable, workers, callback):
    if len(iterable) == 0: return []
    #if workers == 1: return map(func, iterable)

    pool = multiprocessing.Pool(workers, initializer=__init_pmap_worker) if workers > 0 else multiprocessing.Pool(initializer=__init_pmap_worker) # as many workers as HW threads
    try:
        res = [pool.apply_async(func, [i], callback=callback) for i in iterable]
        pool.close()
        ret = []
        for r in res:
            while not r.ready(): r.wait(0.05) # 50ms, to catch keyboard interrupts
            ret.append(r.get())
    except KeyboardInterrupt:
        print("Caught KeyboardInterrupt, terminating workers")
        pool.terminate()
        pool.join()
        raise KeyboardInterrupt
    else:
        #print "pmap finished normally"
        pool.join()
        return ret

test_dir = os.path.abspath(args.dir)
if not os.path.exists(args.rundir): os.makedirs(args.rundir)

# The test's desired inputs or else a default
inputs = {
        'bfs' : ['2000', '8'],
        'bfs_cg' : ['2000', '8'],
        'concurrent_malloc' : ['32', '1024'],
        'counter' : ['1999', '129'],
        'counter_spatial' : ['1999', '129'],
        'counter_spatial_collide' : ['1999', '129'],
        'dfs' : ['2000', '5'],
        'enqueue_all_whints' : ['50000'],
        'exception' : ['128'],
        'fill' : ['171992'],
        'overwrite_spill_buffers' : ['1024'],
        'overwrite_dlmalloc_metadata' : ['8000'],
        'overwrite_undologs' : ['1000'],
        'list' : ['128'],
        'malloc_producer' : ['512', '4096'],
        'many_read_syscalls' : ['129'],
        'multi_hint_rw_mayspec' : ['500'],
        'notimestamp' : ['8193'],
        'precede_waiters' : ['64'],
        'reduce' : ['10000'],
        'taskbomb' : ['128'],
        'taskbomb_deepen' : ['1024'],
        'reducible_counter' : ['256'],
        'reduction' : ['8192'],
        'reduction_hint' : ['8192'],
        'fractal' : ['4'],
        'deepen_chain' : ['7'],
        'deepen_fanout_serial' : ['7'],
        'enqueue_to_parent' : ['5'],
        'malloc' : ['100000'],
}
default_inputs = ['512']

excluded_tests = set([
    ##### TESTS THAT REVEAL ERRORS WE HAVEN'T FIXED ####
    # Fails because the LVT and GVT can reach infinity during doomed priv mode,
    # and because of poor interactions with the scoreboarded core and stealing.
    'priv',

    ##### TESTS THAT THE AUTHOR DOESN'T THINK SHOULD PASS? #####
    # Fails only when frame is disabled, which is only on the 1-core config.
    'fractal_deadlock', # Fails because of unbounded nesting without zooming
    # Fails because the 2-phase GVT protocol isn't implemented
    'multi_hint_rw_mayspec',
    ### Intended to manually test/demonstrate simulator failure:
    'deadlock',
    'malloc_unimplemented',
    'segfault',

    ##### Tests that are innocuous to exclude? #########
    # Trivially passes on master, re-enable on twophase? Or perhaps this shows
    # the need to create a mayspec subdirectory, and only run those tests with
    # configs where mayspec is enabled?
    'nohint_cas_mayspec',
    # Doesn't strenuously test the simulator, so no point?
    'transform',
    # Takes a very long time
    # [ssub] Could not get deepen to spill easily. taskbomb_deepen
    # does spill, but takes a long time to complete.
    'taskbomb_deepen',
    ### The following must be tested manually
    'heartbeats',
    'rand',
    'serialize',
    'simguard',
    'roi_exit',
    'roi_exit_syscall',
    'roi_exit_group_syscall',
    'in_ff',
    'register_end_handler',
    # Stress-tests memory/NoC bandwidth, not anything Swarm-specific
    'stream',
    # [maleen] The following test used to leak simulator memory a while ago,
    # but I can't seem to reproduce it now.
    'malloc',
    ])

excluded_tests |= set(args.exclude)

def is_program(path):
    fname = os.path.basename(path)
    # Filter out executables with a . (e.g., .so files, and .o files in some platforms)
    return os.path.isfile(path) and os.access(path, os.X_OK) and "." not in fname

tests = [path for path in [os.path.abspath(os.path.join(test_dir, f))
                           for f in os.listdir(test_dir)
                           if f != 'run.py']
         if is_program(path)]

# Run slow tests first
slow_tests = ["list", "reducible_counter", "precede_stallers", "precede_waiters"]
def prio(test):
    for p in range(len(slow_tests)):
        if test.find("/" + slow_tests[p]) >= 0:
            return p
    return len(slow_tests)
tests = [test for (p, test) in sorted([(prio(test), test) for test in tests])]

if args.test:
    tests = [t for t in tests if os.path.basename(t) in args.test]
else:
    tests = [t for t in tests if os.path.basename(t) not in excluded_tests]

git_root = sp.check_output(['git', 'rev-parse', '--show-toplevel']).strip()
simscript = 'slowsim' if args.slowsim else 'sim'
sim_exec = os.path.abspath(os.path.join(git_root, 'build/opt/sim', simscript))

configs = [os.path.abspath(c) for c in args.configs]
for config in configs:
    assert os.path.exists(config), config
assert os.path.exists(sim_exec), sim_exec

pre_cmds = [[sim_exec, '-config', config, '--'] for config in configs]
cmds = [(os.path.basename(t), pre_cmd + [t] +
        (inputs[os.path.basename(t)]
         if os.path.basename(t) in inputs else default_inputs))
        for t in tests for pre_cmd in pre_cmds]

cmds = cmds * args.repeats

# Enumerate large command lists
if len(cmds) > 50:
    for i in range(len(cmds)):
        cmds[i] = (("%4d " % i) + cmds[i][0], cmds[i][1])

# dsm: Choose workers to maximize throughput
ntests = len(cmds)
workers = args.workers
if workers == 0:
    workers = multiprocessing.cpu_count()
    if ntests < 2*workers: workers = ntests

tstart = time.time()
if len(args.configs) > 1:
    configStr = "{} configs".format(len(args.configs))
else:
    configStr = args.configs[0]
print("Running {} tests | {} workers | {} | {}".format(ntests, workers, simscript, configStr))
res = pmap(run_and_verify, cmds, workers, print_result)
print("\n%d/%d tests completed successfully in %.1f s" % (len([x for x in res if x[1] == "OK"]), ntests, time.time() - tstart))
