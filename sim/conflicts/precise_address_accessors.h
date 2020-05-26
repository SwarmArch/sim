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

#pragma once

#include <unordered_map>
#include <unordered_set>

#include "sim/conflicts/address_accessors.h"

class PreciseAddressAccessorTable : public AddressAccessorTable {
    using Base = AddressAccessorTable;
  public:

    class AddressSet : public Base::AddressSet {
      public:
        void insert(Address lineAddr) override { addrs_.insert(lineAddr); }
        bool contains(Address lineAddr) const override {
            return addrs_.count(lineAddr) > 0;
        }
        void clear() override { addrs_.clear(); }
        void insertAsRow(AddressAccessorTable& table,
                         const Task* accessor) const override {
            for (Address a : addrs_) {
                table.update(accessor, a);
            }
        }
      private:
        std::unordered_set<Address> addrs_;
    };


    ~PreciseAddressAccessorTable();
    void update(const Task*, Address) override;
    void insert(const Task*, const Base::AddressSet&) override;
    void clear(const Task*) override;
    bool test(const Task*, Address) const override;
    bool anyAddresses(const Task*) const override;

    Accessors allAccessors(Address) const override;
    bool anyAccessors(Address) const override;

    Base::AddressSet* createAddressSet() const override {
        return new PreciseAddressAccessorTable::AddressSet();
    }

  private:
    // FIXME(mcj) These two maps are admittedly expensive, but I want to get
    // something working and move on. I'm surprised Boost doesn't have some
    // magic in the multi_index concepts to solve this. The idea is for the
    // (common case) query allAccessors to be fast, and all else might suffer
    // marginally.
    // [dsm] Note that we need Accessors pointers; unordered_map resizes
    // would invalidate references to deques otherwise
    std::unordered_map<Address, Accessors*> addr2Tasks_;
    std::unordered_map<const Task*, std::unordered_set<Address>*> task2Addrs_;
};
