The Swarm Architecture Simulator
================================

This repository contains source code to simulate
the [Swarm](http://swarm.csail.mit.edu) architecture
\[[MICRO 2015](https://doi.org/10.1145/2830772.2830777),
[TopPicks 2016](https://doi.org/10.1109/MM.2016.12)\]
with Spatial Hints \[[MICRO 2016](https://doi.org/10.1109/MICRO.2016.7783708)\],
Fractal \[[ISCA 2017](https://doi.acm.org/10.1145/3079856.3080218)\],
SAM \[[PACT 2017](https://doi.org/10.1109/PACT.2017.37)\],
and Espresso/Capsules \[[MICRO 2018](https://doi.org/10.1109/MICRO.2018.00026)\].
Before adopting the name Swarm, we called this the ordered speculation simulator
(a.k.a. `ordspecsim`).

Directory structure
-------------------

- `doc/`: documentation
- `sim/`: simulator source code
- `sim/tests/`: unit tests
- `googletest/`: library for unit tests
- `libspin/`: Sequential Pin library
- `plsalloc/`: scalable memory allocator
- `runtime/`: other source files shared with other repos (e.g., shared with benchmarks)
- `runtime/include/`: header files used by Swarm programs.
- `tests/`: system tests ("microbenchmarks")
- `scripts/`: tools that help you conduct simulations or interpret results
- `misc/`: tools for analyzing and editing simulator source code
- `build/`: compiled files (binaries) for the simulator and tests

Setup
-----

Dependencies:
- Linux on x86_64: We've tested with Ubuntu 14.04, 16.04, and 18.04.
  If you want to run this in a VM, see Vagrant setup below.
- GCC: Version 4.8 or newer will suffice to build the simulator itself, which
  is written in C++11 and depends on a particular GCC ABI.  (Clang won't work.)
  Test applications are written in C++14 so they can build with GCC 5+ or Clang.
- Pin version 2.14: Download this from
  [here](https://software.intel.com/sites/landingpage/pintool/downloads/pin-2.14-71313-gcc.4.4.7-linux.tar.gz)
  (mirrors:
  [1](http://swarm.csail.mit.edu/tools/pin-2.14-71313-gcc.4.4.7-linux.tar.gz),
  [2](https://web.archive.org/web/20150808154819/https://software.intel.com/sites/landingpage/pintool/downloads/pin-2.14-71313-gcc.4.4.7-linux.tar.gz),
  [3](https://perma.cc/TL4N-VYZC)).
  Set the environment variable `PINPATH` to point to the base directory
  extracted from the tarball.  Avoid versions of Pin other than v2.14 (71313).
  - To test whether Pin works, run these four commands
    to [count instructions](https://software.intel.com/sites/landingpage/pintool/docs/71313/Pin/html/index.html#SimpleCount)
    for copying a file:
    ```bash
    cd $PINPATH/source/tools/ManualExamples
    make CXX="g++ -fabi-version=2 -D_GLIBCXX_USE_CXX11_ABI=0" obj-intel64/inscount0.so TARGET=intel64
    $PINPATH/intel64/bin/pinbin -ifeellucky -t obj-intel64/inscount0.so -- cp makefile obj-intel64/inscount0.makefile.copy
    cat inscount.out
    ```
    Pin must use ptrace to inject itself into the application process at runtime.
    If you get injection errors, fix them by giving Pin ptrace capabilities:
    ```bash
    sudo apt-get install libcap2-bin
    sudo setcap cap_sys_ptrace=eip $PINPATH/intel64/bin/pinbin
    ```
    Unfortunately, `setcap` relies on xattr, which is unsupported on
    some filesystems.  A workaround is to set
    the [sysctl parameter `ptrace_scope`](https://www.kernel.org/doc/html/latest/admin-guide/LSM/Yama.html#ptrace-scope)
    to `0`, giving ptrace capabilities to *all* programs on the system:
    ```bash
    echo 0 | sudo tee /proc/sys/kernel/yama/ptrace_scope
    ```
    This sysctl parameter may be reset after every reboot.
    To automatically set it so you do not need to set it manually after reboots:
    ```bash
    echo "kernel.yama.ptrace_scope = 0" | sudo tee /etc/sysctl.d/90-ptrace.conf
    ```
- SCons: This is widely available under the name `scons` in package managers.
  You can also install SCons from PyPI using `pip3 install SCons`.
- Boost (headers only): On Debian/Ubuntu, install the package `libboost-dev`.
- libconfig: On Debian/Ubuntu, you may install the package `libconfig++-dev`.
  Or build libconfig from sources available [here](https://hyperrealm.github.io/libconfig/),
  in which case you must set the `LIBCONFIGPATH` environment variable to point
  to the base directory where you install libconfig.
- HDF5: On Ubuntu, you may install the package `libhdf5-serial-dev`.
  Or build HDF5 from sources available [here](https://www.hdfgroup.org/),
  in which case you must set the `HDF5PATH` environment variable to point to
  the base directory where you install HDF5.
- (Optional) Armadillo: Currently, we only use simple matrix arithmetic to
  perform load balancing, and this uses Armadillo in some configurations.
  The matrix routines we use are defined entirely in Armadillo's header files,
  so we don't need to link against libarmadillo.  You can obtain the headers we
  use in the `libarmadillo-dev` package on Ubuntu/Debian.
  Or download Armadillo from [here](http://arma.sourceforge.net/) and set the
  environment variable `ARMADILLOPATH` to point to the base directory extracted
  from the tarball.
- (Optional) TCMalloc: Test applications can link against TCMalloc to compare
  performance.  Install the `libgoogle-perftools-dev` package on Debian/Ubuntu.
- (Optional) GNU Readline: Used in our interactive debugging mode.
  Install the `libreadline-dev` package on Debian/Ubuntu.
- (Optional) h5py and libconf for Python 2: libraries used by our Python
  scripts to run test applications and check simulation results.  These can be
  easily obtained from PyPI using `pip`.

### (Optional) Vagrant setup
To run the simulator on a non-Linux system, you need a Linux VM.  You could
manually follow the notes above to install the simulator's dependencies on any
Ubuntu VM.  (Using WSL2 works on Windows.)  Alternatively, we provide automated
VM configuration with Vagrant so you don't need to manually install all the
dependencies.

```bash
$ cp Vagrantfile.sample Vagrantfile
```
Customize (uncomment) the memory and cpu offerings of the Vagrantfile in
the `config.vm.provider` block that is relevant to you: You can use Hyper-V to
host the VM on Windows, or VirtualBox on any host OS.  If you will use SCC/T4,
give the VM at least 12 GB of memory to avoid issues with building LLVM/Clang.
You can point a synced folder to your benchmarks or SCC repositories to offer
this VM access to their contents.

Then
```bash
$ vagrant up
$ vagrant ssh
vagrant@sim:$ cd /vagrant
```

Build
-----

In this repo's root directory, run:
```bash
$ scons -j4
```
This builds the simulator, some micro test applications, and its (googletest) unit
tests.

You can specify a directory to build.  For example, to build only the unit tests:
```bash
$ scons -j4 sim/test
```

Run
---
As an example, run the micro test `bfs` with a simulated Swarm system of 4
tiles (aka ROBs) and 4 cores per tile. Therefore 16 total worker threads are
concurrently executing tasks.
```bash
$ ./build/opt/sim/sim -config sample.cfg -- ./build/opt/tests/swarm/bfs 2000 8
```
The `-config` option specifies a configuration file for the simulated hardware.
Everything following the `--` is the application command being executed.

While running, the simulator updates a file named `heartbeat` in the current
working directory. This allows you to check the progress of the simulation.
After the simulation finishes, a summary of the results is printed to stdout.
This summary shows the total number of cycles the application took to execute,
and breaks down how many cycles tasks spent waiting to run, running, and
waiting to commit. More detailed statistics are saved to disk in text and HDF5
files.

To run the unit tests:
```bash
$ ./build/opt/sim/test/runTests
```

To run all (thousands of) system tests:
```bash
$ ./scripts/configs.py
$ ./tests/run_all.py -c ./configs/*.cfg -d build/opt/tests/swarm/
```

For more documentation on using Swarm and interpreting simulation results,
see the [User Guide](doc/UserGuide.mdown).
We will release documentation to separate repositories on how to configure the
simulator to perform specific experiments described in Swarm papers.

License and copyright
---------------------

This simulator is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, version 2.

This simulator was developed at MIT by several people over six years, who are
the authors of the Swarm papers linked above.  Some files were taken from
[zsim](http://zsim.csail.mit.edu/), which is also available under a GPLv2
license.  zsim was first written by Daniel Sanchez at Stanford University, and
later substantially modified and enhanced at MIT.  Files adapted from zsim have
headers that show their copyright attribution to Stanford University and MIT.

Additionally, if you use this software in your research, we request that you
reference the Swarm MICRO 2015 paper ("A Scalable Architecture for Ordered
Parallelism", Jeffrey et al., MICRO-48, December 2015) as the source of the
simulator in any publications that use this software, and that you send a
citation of your work to swarm@csail.mit.edu.
