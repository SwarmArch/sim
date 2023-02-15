from __future__ import (absolute_import, division, print_function)

import os
import subprocess

# Options
AddOption('--clang', default=False, action='store_true',
          help='Build with clang (better error messages)')
AddOption('--trace', default=False, action='store_true',
          help='Have simulator produce traces.')
AddOption('--interactive-sim', default=False, action='store_true',
          help='Run simulations in an interactive debugging mode.')

# get the mode flag from the command line
mode = ARGUMENTS.get('mode', 'opt')

allowedModes = ['debug', 'opt', 'release', 'cov']
if mode not in allowedModes:
    print("Error: invalid mode", mode, "allowed:", allowedModes)
    Exit(1)

env = Environment(ENV = os.environ, tools = ['default', 'textfile'])
env.Append(LINKFLAGS=["-Wl,--no-undefined"])

for submodulePath in subprocess.check_output(
                ['git', 'submodule', '--quiet', 'foreach', 'realpath .'],
                universal_newlines=True).split('\n'):
    if not submodulePath.strip():
        continue

    # Automatically clone submodules if user hasn't done so
    if not os.path.exists(submodulePath) or not os.listdir(submodulePath):
        print('Cloning %s...' % submodulePath)
        env.Execute('git submodule update --init --remote ' + submodulePath)

    commits = ''
    try:
        commits = subprocess.check_output(
                'cd %s && git rev-list HEAD' % submodulePath,
                shell=True, universal_newlines=True)
    except subprocess.CalledProcessError:
        pass
    _, neededCommit, _ = subprocess.check_output(
            ['git', 'ls-files', '--stage', submodulePath],
            universal_newlines=True).split(maxsplit=2)
    if neededCommit not in commits:
        print('ERROR: commit', neededCommit, 'not found in', submodulePath)
        print('Please run:')
        print('    git submodule update --init --remote --checkout', submodulePath)
        Exit(1)

if GetOption('clang'):
    env['CC'] = 'clang'
    env['CXX'] = 'clang++'

# All sub projects will use the runtime's include directory,
# which appears under the project root
env.Append(CPPPATH = [os.path.join(Dir('#').srcnode().abspath, d)
                      for d in ['runtime/include', '']])
env.Append(CPPFLAGS = ['-g', '-Wall'])
modeFlags = {
    'opt' : ['-O3','-gdwarf-3', '-march=core2'],
    'release' : ['-O3', '-DNDEBUG', '-DNASSERT', '-gdwarf-3', '-march=core2'],
    'debug' : ['-Og', '-fno-omit-frame-pointer', '-gdwarf-3'],
    'cov' : ['-O0', '-g', '--coverage']
}
env.Append(CPPFLAGS = modeFlags[mode])

if mode == 'cov':
    env.Append(LINKFLAGS = ['--coverage'])

sim_env = env.Clone()
sim_name = 'sim'
if GetOption('trace'):
    sim_name = 'trace_' + sim_name
    sim_env.Append(CPPFLAGS = ['-DENABLE_TRACING'])
if GetOption('interactive_sim'):
    sim_name = 'interactive_' + sim_name
    sim_env.Append(CPPFLAGS = ['-DENABLE_INTERACTIVE'])
sim_variant_dir=os.path.join('build', mode, sim_name)

# Record version
versionFile = os.path.join(sim_variant_dir, 'version')
env.Execute(' && '.join([
        'mkdir -p ' + sim_variant_dir,
        'date >| ' + versionFile,
        'python misc/gitver.py >> ' + versionFile,
        'echo >> ' + versionFile,
        'git diff -U0 >> ' + versionFile]))

SConscript('sim/SConscript',
    variant_dir=sim_variant_dir,
    exports = {'env' : sim_env},
    duplicate=0)

swarmIncludePath = os.path.join(Dir('#').abspath, 'runtime/include')
for runtime in ['swarm', 'seq']:
    libswarm, libsimalloc = SConscript('runtime/lib/SConscript',
            variant_dir=os.path.join('build', mode, 'runtime/lib', runtime),
            exports={'runtime' : runtime}, duplicate=0)
    testenv = env.Clone()
    if libswarm: testenv.Append(LIBS = [libswarm])

    SConscript('tests/SConscript',
            variant_dir=os.path.join('build', mode, 'tests', runtime),
            exports={'env':testenv,
                     'libsimalloc':libsimalloc,
                     'runtime':runtime},
            duplicate=0)
