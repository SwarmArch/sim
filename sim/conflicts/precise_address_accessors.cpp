/** $lic$
 * Copyright (C) 2014-2021 by Massachusetts Institute of Technology
 *
 * This file is part of the Swarm simulator.
 *
 * This simulator is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, version 2.
 *
 * This simulator was developed as part of the Swarm architecture project. If
 * you use this software in your research, we request that you reference the
 * Swarm MICRO 2018 paper ("Harmonizing Speculative and Non-Speculative
 * Execution in Architectures for Ordered Parallelism", Jeffrey et al.,
 * MICRO-51, 2018) as the source of this simulator in any publications that use
 * this software, and that you send us a citation of your work.
 *
 * This simulator is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "sim/assert.h"
#include "sim/conflicts/precise_address_accessors.h"

#undef DEBUG
#define DEBUG(args...) //info(args)

PreciseAddressAccessorTable::~PreciseAddressAccessorTable() {
    for (auto& pair : addr2Tasks_) {
        delete pair.second;
    }
    for (auto& pair : task2Addrs_) {
        delete pair.second;
    }
}

void PreciseAddressAccessorTable::update(const Task* task, Address lineAddr) {
    // [mcj] Don't use task->toString() because any file that #includes task.h
    // is a pain to unit test.
    DEBUG("precise_table::update lineAddr 0x%lx by %p",
          lineAddr, task);
    Accessors * lineAccs;
    std::unordered_set<Address>* addrSet;

    // Sure wish we had C++17 to replace the following
    // http://en.cppreference.com/w/cpp/container/unordered_map/insert_or_assign
    auto it = addr2Tasks_.find(lineAddr);
    if (it != addr2Tasks_.end()) {
        lineAccs = it->second;
        assert(!lineAccs->empty());
    } else {
        lineAccs = new Accessors();
        addr2Tasks_[lineAddr] = lineAccs;
    }
    auto itt = task2Addrs_.find(task);
    if (itt != task2Addrs_.end()) {
        addrSet = itt->second;
    } else {
        addrSet = new std::unordered_set<Address>();
        task2Addrs_[task] = addrSet;
    }
    lineAccs->insert(task);
    addrSet->insert(lineAddr);
}

void PreciseAddressAccessorTable::insert(const Task* task,
                                         const Base::AddressSet& addrSet) {
    // Visitor pattern to avoid up-casting to PreciseAddressSet
    // https://en.wikipedia.org/wiki/Visitor_pattern
    addrSet.insertAsRow(*this, task);
}

void PreciseAddressAccessorTable::clear(const Task* task) {
    DEBUG("precise_table::clear %p", task);
    auto it = task2Addrs_.find(task);
    if (it != task2Addrs_.end()) {
        auto* set = it->second;
        for (Address a : *set) {
            Accessors* accs = addr2Tasks_[a];
            assert(accs);
            size_t count = accs->erase(task);
            assert(count);
            if (accs->empty()) {
                delete accs;
                addr2Tasks_.erase(a);
            }
        }
        delete set;
        task2Addrs_.erase(it);
    }
}


bool PreciseAddressAccessorTable::test(const Task* task, Address a) const {
    auto it = task2Addrs_.find(task);
    return (it == task2Addrs_.end()) ? false : it->second->count(a);
}


bool PreciseAddressAccessorTable::anyAddresses(const Task* task) const {
    auto it = task2Addrs_.find(task);
    return (it == task2Addrs_.end()) ? false : !it->second->empty();
}

Accessors PreciseAddressAccessorTable::allAccessors(Address lineAddr) const {
    Accessors accs;

    auto it = addr2Tasks_.find(lineAddr);
    if (it != addr2Tasks_.end()) {
        accs.insert(it->second->begin(), it->second->end());
    }
    // "Common limitations of copy elision are:
    //  * multiple return points."
    // http://stackoverflow.com/a/12953150
    return accs;
}

bool PreciseAddressAccessorTable::anyAccessors(Address lineAddr) const {
    auto it = addr2Tasks_.find(lineAddr);
    return (it == addr2Tasks_.end()) ? false : !it->second->empty();
}
