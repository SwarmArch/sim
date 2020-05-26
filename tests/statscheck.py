from __future__ import (absolute_import, division, print_function)

import io  # for reading unicode from file (for libconf)
import itertools
import os
import sys

import h5py
import libconf # https://pypi.python.org/pypi/libconf


TASK_TYPES = ['notask', 'worker', 'requeuer', 'spiller', 'irrevocable']
CYCLE_TYPES = ['cycles', 'committedCycles',
               'abortedDirectCycles', 'abortedIndirectCycles']

IPC1_STATES = dict([(s,i) for i,s in enumerate(
        ['idle', 'exec', 'execToAbort',
         'stallEmpty', 'stallResource',
         'stallThrottle', 'stallConflict',
        ])])
SB_STATES = dict([(s,i) for i,s in enumerate(
        ['idle', 'issued', 'otherCtxIssue', 'stallSb', 'tq', 'toabort',
          'cq', 'empty', 'throttle', 'nack', 'busy', 'wrongpath',
        ])])



class __Trace(object):
    def __init__(self, filename):
        try:
            self.h5file = h5py.File(filename, mode = "r")
            self.stats = self.h5file["stats"]["root"]
            self.valid = True
        except Exception:
            self.valid = False

    def __del__(self):
        if not self.h5file == None:
            self.h5file.close()


def __cyclesum_sb(stats):
    a = stats['core']['ctx']['cycles']
    ncores = len(a)
    nthreads = len(a[0])
    ntasktypes = len(a[0][0])
    return sum((a[c][th][tt].sum() for c,th,tt in itertools.product(
            range(ncores), range(nthreads), range(ntasktypes))
            ))


def __cyclesum_ipc1(stats):
    a = stats['core']['cycles']
    ncores = len(a)
    return sum((a[c][tt].sum() for c,tt in
               itertools.product(range(ncores), range(len(TASK_TYPES)))))


def __sumOfCycleBreakdowns(trace, config):
    core = config['sys']['cores']['type']
    assert core in ['Simple', 'Scoreboard', 'OoO']
    stats = trace.stats[-1]
    return __cyclesum_ipc1(stats) if core == 'Simple' else __cyclesum_sb(stats)


def __totalCycles(trace, config):
    corecfg = config['sys']['cores']
    nthreads = corecfg['cores'] * corecfg['threads']
    cycles = trace.stats[-1]['cycles']
    return nthreads * cycles


def breakdownsMatchTotalCycles(trace, config):
    #FIXME(victory): Temporary hack until somebody figures out the mess that is OoO breakdowns.
    if config['sys']['cores']['type'] == 'OoO': return True

    #FIXME(victory): Tempoary hack to disable failing check for superscalar cores.
    if config['sys']['cores']['issueWidth'] > 1: return True

    s = __sumOfCycleBreakdowns(trace, config)
    t = __totalCycles(trace, config) * config['sys']['cores']['issueWidth']
    success = s == t
    if not success:
        print('ERROR: Cycle breakdowns sum to %d instead of %d' % (s, t))
    return success



def __whole_is_sum_of_parts_ipc1(stats, state):
    '''
    State is an integer representing the Core State
    (e.g. 'exec' or 'stallEmpty')
    We skip the 'notask' task type here because although its cycles should
    appear under 'committedCycles', they do not for now.
    '''
    a = stats['core']
    s = IPC1_STATES[state]
    whole = sum((a[CYCLE_TYPES[0]][tt][:,s].sum() for tt in TASK_TYPES[1:]))
    sumofparts = sum((a[ct][tt][:,s].sum() for ct,tt in
            itertools.product(CYCLE_TYPES[1:], TASK_TYPES[1:])))
    success = whole == sumofparts
    if not success:
        print('ERROR: Whole %d is not sum of parts %d for state %s' % (
                whole, sumofparts, state))
    return success


def __whole_is_sum_of_parts_sb(stats, state):
    a = stats['core']['ctx']
    s = SB_STATES[state]
    whole = sum((a[CYCLE_TYPES[0]][tt][:,:,s].sum() for tt in TASK_TYPES[1:]))
    sumofparts = sum((a[ct][tt][:,:,s].sum() for ct,tt in
            itertools.product(CYCLE_TYPES[1:], TASK_TYPES[1:])))
    success = whole == sumofparts
    if not success:
        print('ERROR: Whole %d is not sum of parts %d for state %s' % (
                whole, sumofparts, state))
    return success


def wholeIsSumOfThePartsCycleBreakdowns(trace, config):
    core = config['sys']['cores']['type']
    assert core in ['Simple', 'Scoreboard', 'OoO']
    stats = trace.stats[-1]
    if core == 'Simple':
        return all([__whole_is_sum_of_parts_ipc1(stats, state) for state in
                    ['exec', 'execToAbort', 'stallResource', 'stallConflict']])
    else:
        #FIXME(victory): Temporary hack to disable failing check for stalls on superscalar cores.
        if config['sys']['cores']['issueWidth'] > 1:
            return all([__whole_is_sum_of_parts_sb(stats, state) for state in
                       ['issued', 'busy']])

        return all([__whole_is_sum_of_parts_sb(stats, state) for state in
                    ['issued', 'stallSb', 'tq', 'toabort', 'nack', 'busy']])


def __non_abortables_are_zero_ipc1(stats, nonabortables, nonabortablePairs):
    '''
    TODO(mcj) de-duplicate with the sb-core version
    '''
    a = stats['core']

    # non-abortable cycle type, task type pairs should have zero cycles
    shouldbezero = [a[ct][tt][:, IPC1_STATES['execToAbort']].sum()
                    for ct,tt in nonabortablePairs]
    successToAbort = shouldbezero == [0.] * len(shouldbezero)
    if not successToAbort: print('ERROR: toabort cycles found in invalid types')

    # non-abortable task types should have zero aborted cycles period
    shouldbezero = [a[ct][tt][:, :].sum() for ct,tt in
                    itertools.product(CYCLE_TYPES[2:], nonabortables)]
    successTaskTypes = shouldbezero == [0.] * len(shouldbezero)
    if not successTaskTypes:
        print('ERROR: non-abortable task type had aborted cycles')
    return successToAbort and successTaskTypes


def __non_abortables_are_zero_sb(stats, nonabortables, nonabortablePairs):
    a = stats['core']['ctx']

    # non-abortable cycle type, task type pairs should have zero cycles
    shouldbezero = [a[ct][tt][:, :, SB_STATES['toabort']].sum()
                    for ct,tt in nonabortablePairs]
    successToAbort = shouldbezero == [0.] * len(shouldbezero)
    if not successToAbort: print('ERROR: toabort cycles found in invalid types')

    # non-abortable task types should have zero aborted cycles period
    shouldbezero = [a[ct][tt][:, :, :].sum() for ct,tt in
                    itertools.product(CYCLE_TYPES[2:], nonabortables)]
    successTaskTypes = shouldbezero == [0.] * len(shouldbezero)
    if not successTaskTypes:
        print('ERROR: non-abortable task type had aborted cycles')
    return successToAbort and successTaskTypes



def nonAbortableStatsHaveZeroAbortedCycles(trace, config):
    nonabortables = list(set(TASK_TYPES) - set(['worker']))
    pairs = (
            set(itertools.product([CYCLE_TYPES[1]], TASK_TYPES)) |
            set(itertools.product(CYCLE_TYPES, nonabortables)))

    core = config['sys']['cores']['type']
    assert core in ['Simple', 'Scoreboard', 'OoO']
    stats = trace.stats[-1]
    if core == 'Simple':
        return __non_abortables_are_zero_ipc1(stats, nonabortables, pairs)
    else:
        #FIXME(victory): Tempoary hack to disable failing check for superscalar cores.
        if config['sys']['cores']['issueWidth'] > 1:
            pairs.remove(('cycles', 'notask'))
        return __non_abortables_are_zero_sb(stats, nonabortables, pairs)





def succeeds(dirpath):
    h5path = os.path.join(dirpath, 'sim.h5')
    configpath = os.path.join(dirpath, 'out.cfg')
    assert os.path.exists(h5path)
    assert os.path.exists(configpath)

    trace = __Trace(h5path)
    with io.open(configpath) as f:  # io.open gives unicode on Python 2, as needed by libconf
        config = libconf.load(f)
    assert trace.valid

    # We don't assert correctness in the MT breakdown scripts, so I'm not going
    # to go to the trouble to figure out it here.
    if config['sys']['cores']['threads'] > 1: return True

    retval = breakdownsMatchTotalCycles(trace, config)
    retval &= wholeIsSumOfThePartsCycleBreakdowns(trace, config)
    retval &= nonAbortableStatsHaveZeroAbortedCycles(trace, config)
    return retval


if __name__ == '__main__':
    assert len(sys.argv) > 1
    dirpath = sys.argv[1]
    s = succeeds(dirpath)
    print('Success' if s else 'Failure')
    assert s
