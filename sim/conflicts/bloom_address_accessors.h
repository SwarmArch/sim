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

#include <cstdint>
#include <unordered_map>

#include "sim/conflicts/address_accessors.h"
#include "sim/bloom/bf_partitioned.h"

class BloomAddressAccessorTable : public AddressAccessorTable {
  public:
    class AddressSet: public bloom::BloomFilterPartitioned<Address>,
                      public AddressAccessorTable::AddressSet {
        using Base = BloomFilterPartitioned<Address>;
        friend class BloomAddressAccessorTable;
      public:
        using Base::Base;
        ~AddressSet() = default;
        void insert(Address a) override { Base::insert(a); }
        bool contains(Address a) const override { return Base::contains(a); }
        void clear() override { Base::clear(); }
        void insertAsRow(AddressAccessorTable&, const Task*) const override {}
    };

    BloomAddressAccessorTable(uint32_t nbits, uint32_t nfcns,
                              const H3HashFamily&);
    // For testing only
    BloomAddressAccessorTable(uint32_t nbits, uint32_t nfcns);

    ~BloomAddressAccessorTable();
    void update(const Task*, Address) override;
    void insert(const Task*, const AddressAccessorTable::AddressSet&) override;
    void clear(const Task*) override;
    bool test(const Task*, Address) const override;
    bool anyAddresses(const Task*) const override;
    Accessors allAccessors(Address) const override;
    bool anyAccessors(Address) const override;

    AddressAccessorTable::AddressSet* createAddressSet() const override {
        return new BloomAddressAccessorTable::AddressSet(
                nbits_, nfcns_, hash_);
    }

  private:
    const uint32_t nbits_;
    const uint32_t nfcns_;
    const H3HashFamily& hash_;

    std::unordered_map<const Task*, AddressSet*> task2BF_;
};
