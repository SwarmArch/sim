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

#include <unordered_set>
#include "sim/types.h" // For Address, FIXME but memory_hierarchy.h also works

class Task;
// FIXME(mcj) we'll likely end up with pain if this remains const, but it
// offers better guarantees to the caller of the Table.
using Accessors = std::unordered_set<const Task*>;

/**
 * Abstractly this is a boolean matrix with task rows and address columns.
 * Along a row, it indicates what addresses that task accessed
 * Along a column, it indicates which tasks accessed that address
 *
 * Addresses-->   0    1   2   3   4  ... 2^63  2^64
 * Tasks   t1     x            x
 *   |     t2          x           x       x
 *   |     t3          x
 *  \./    t4              x   x
 *  ...
 */

class AddressAccessorTable {
  public:
    class AddressSet {
      public:
        virtual ~AddressSet() {}
        virtual void insert(Address lineAddr) = 0;
        virtual bool contains(Address lineAddr) const = 0;
        virtual void clear() = 0;
        virtual void insertAsRow(AddressAccessorTable& table,
                                 const Task* accessor) const = 0;
    };

    // Update a cell
    virtual void update(const Task* task, Address lineAddr) = 0;
    // Insert a row
    virtual void insert(const Task* task, const AddressSet& addrSet) = 0;
    // Delete a row
    virtual void clear(const Task* task) = 0;
    // Query a cell
    virtual bool test(const Task* task, Address lineAddr) const = 0;
    // Query whether there is at least one address in the row
    virtual bool anyAddresses(const Task* task) const = 0;
    // Query all non-zero tasks along a column
    virtual Accessors allAccessors(Address lineAddr) const = 0;
    // Query whether there is at least one non-zero task in a column
    virtual bool anyAccessors(Address lineAddr) const = 0;
    // Factory method
    virtual AddressSet* createAddressSet() const = 0;

    virtual ~AddressAccessorTable() {}
};
