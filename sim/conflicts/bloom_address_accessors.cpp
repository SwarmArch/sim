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

#include "sim/conflicts/bloom_address_accessors.h"

#include <stdexcept>
#include "sim/assert.h"
#include "sim/bithacks.h"

BloomAddressAccessorTable::BloomAddressAccessorTable(
        uint32_t nbits, uint32_t nfcns, const H3HashFamily& hash)
  : nbits_(nbits), nfcns_(nfcns),
    // Ideally the hash function should be created as a function of nbits and
    // nfcns, or alternatively the hash function's API would tell us its
    // properties. But it is more important to have one global instance of the
    // hash function, rather than several modular copies.
    hash_(hash) {
    assert(nbits % nfcns == 0);
    assert(isPow2(nbits / nfcns));
}


BloomAddressAccessorTable::BloomAddressAccessorTable(uint32_t b, uint32_t f)
  : BloomAddressAccessorTable(b, f,
          // FIXME(mcj) this should get deleted, but it's only used in tests
          *(new H3HashFamily(f, ilog2(b / f), 0xCAFE101A)))
{}


BloomAddressAccessorTable::~BloomAddressAccessorTable() {
    for (auto& pair : task2BF_) {
        delete pair.second;
    }
}

void BloomAddressAccessorTable::update(const Task* task, Address lineAddr) {
    AddressSet * set;

    auto it = task2BF_.find(task);
    if (it != task2BF_.end()) {
        set = it->second;
    } else {
        set = new AddressSet(nbits_, nfcns_, hash_);
        task2BF_[task] = set;
    }
    set->insert(lineAddr);
}

void BloomAddressAccessorTable::insert(const Task*,
        const AddressAccessorTable::AddressSet&) {
    throw std::runtime_error("unimplemented");
}

void BloomAddressAccessorTable::clear(const Task* task) {
    auto it = task2BF_.find(task);
    if (it != task2BF_.end()) {
        auto* bf = it->second;
        delete bf;
        task2BF_.erase(it);
    }
}


bool BloomAddressAccessorTable::test(const Task* task, Address a) const {
    auto it = task2BF_.find(task);
    return (it == task2BF_.end()) ? false : it->second->contains(a);
}


bool BloomAddressAccessorTable::anyAddresses(const Task* task) const {
    auto it = task2BF_.find(task);
    if (it != task2BF_.end()) {
        auto* bf = it->second;
        // [mcj] For now, if a BF appears in the task2BF_ map, it must have some
        // bits set. But in case that isn't always true, let's play it safe.
        return !bf->empty();
    }
    return false;
}

Accessors BloomAddressAccessorTable::allAccessors(Address lineAddr) const {
    Accessors accessors;
    bloom::hash_t* hashes = new bloom::hash_t[nfcns_];
    for (uint32_t i = 0; i < nfcns_; i++) {
        hashes[i] = hash_.hash(i, lineAddr);
    }

    for (auto& pair : task2BF_) {
        if (pair.second->containsHashes(hashes)) accessors.insert(pair.first);
    }
    delete [] hashes;
    return accessors;
}

bool BloomAddressAccessorTable::anyAccessors(Address lineAddr) const {
    bloom::hash_t* hashes = new bloom::hash_t[nfcns_];
    for (uint32_t i = 0; i < nfcns_; i++) {
        hashes[i] = hash_.hash(i, lineAddr);
    }

    bool matchFound = false;
    for (auto& pair : task2BF_) {
        if (pair.second->containsHashes(hashes)) {
            matchFound = true;
            break;
        }
    }
    delete [] hashes;
    return matchFound;
}
