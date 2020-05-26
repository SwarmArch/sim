#!/bin/bash

set -x

apt-get -yq update

apt-get -yq install software-properties-common  # for add-apt-repository
apt-get -yq install build-essential
apt-get -yq install make g++
apt-get -yq install python
apt-get -yq install libreadline-dev
apt-get -yq install pypy pypy-six
apt-get -yq install git
apt-get -yq install gdb
apt-get -yq install linux-tools-common  # for perf
apt-get -yq install cmake ninja-build  # for building SCC

apt-get -yq install libconfig++-dev
apt-get -yq install libhdf5-serial-dev
apt-get -yq install libboost-dev
apt-get -yq install libarmadillo-dev

apt-get -yq install python3-pip
pip3 install SCons

# Benchmarks/tests can be linked with other allocators, like tcmalloc
apt-get -yq install libtcmalloc-minimal4
# The following symlink is not part of the libtcmalloc-minimal4 Debian package,
# but is in libgoogle-perftools-dev.  We don't need the rest of perftools, so
# let's just create this symlink ourselves.
# This is how we can get GCC to find -ltcmalloc_minimal
ln -s /usr/lib/libtcmalloc_minimal.so.4 /usr/lib/libtcmalloc_minimal.so

# Make multiple versions of GCC available for testing
# Ubuntu 14.04 LTS shipped with GCC 4.8, which may give you a better
# chance of ABI compatibility with Pin 2.14.
apt-get -yq install g++-4.8
# Ubuntu 16.04 LTS shipped with GCC 5
apt-get -yq install g++-5
# Ubuntu 18.04 LTS shipped with GCC 7
apt-get -yq install g++-7  # Should be already be installed as dependency of g++
# Ubuntu 20.04 LTS shipped with GCC 9
add-apt-repository -y ppa:ubuntu-toolchain-r/test
apt-get -yq update
apt-get -yq install g++-9

VMUSER="vagrant"
HOME="/home/${VMUSER}"

PINDIR="pin-2.14-71313-gcc.4.4.7-linux"
wget --no-verbose --no-clobber http://software.intel.com/sites/landingpage/pintool/downloads/${PINDIR}.tar.gz
tar zxf ${PINDIR}.tar.gz -C /opt/
chown -R ${VMUSER} /opt/${PINDIR}
# If in a VM that is only used for Swarm experiments, no need to be paranoid
# about security, so globally enable classic (lax) ptrace permissions
echo 0 > /proc/sys/kernel/yama/ptrace_scope
echo "kernel.yama.ptrace_scope = 0" > /etc/sysctl.d/90-ptrace.conf

function appendToFileUniquely {
    STRING=$1
    FILE=$2
    #http://stackoverflow.com/a/13027535
    if ! grep -qe "^${STRING}$" ${FILE}; then
        echo ${STRING} >> $FILE
    fi
}

appendToFileUniquely "export PINPATH=/opt/${PINDIR}" ${HOME}/.bashrc

# Install enough to run microbenchmarks with run_all.py under Python 2
apt-get -yq install python-pip
pip install 'numpy<1.17'  # NumPy supported Python 2 through 1.16.x
pip install h5py
pip install libconf

# To run orig_silo from the simulator's VM, install db++-dev
# (but unfortunately orig_silo still doesn't run to completion in the VM)
apt-get -yq install libdb++-dev
