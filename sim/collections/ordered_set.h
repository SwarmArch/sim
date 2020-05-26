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

#include <algorithm>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/identity.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index_container.hpp>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include "sim/assert.h"

using boost::multi_index_container;
using namespace boost::multi_index;

/**
 * An ordered set for pointers to objects that can be compared in a partial
 * order. Note that a std::set<int*,Compare> that Compares pointers based on
 * the value of the ints won't permit two distinct objects (pointers) with the
 * same integer value.
 */
template <typename T, typename OrderedIndex, typename Key = T>
class ordered_pointer_set {
  private:
    // To faciliate fast erase-by-pointer, while supporting some
    // non-pointer-based order, I opted to use the boost::multi_index_container.
    // http://www.boost.org/doc/libs/1_57_0/libs/multi_index/doc/tutorial/
    using ordered_ptr_impl = multi_index_container<T,
        boost::multi_index::indexed_by<
            // Different sets demand different key extractors, so the whole
            // ordered index is left as a template parameter
            OrderedIndex,
            // The pointers themselves are unique, and must be fast to delete
            hashed_unique<identity<T>>
        >
    >;
  protected:
    ordered_ptr_impl os_;
    typename ordered_ptr_impl::template nth_index<0>::type& oindex_;
    typename ordered_ptr_impl::template nth_index<1>::type& ptr_index_;

  public:
    // dsm: os_.template get<0>... is the correct expression, see:
    // http://stackoverflow.com/questions/14615328/boost-multi-index-container-of-template-dependent-struct-in-template-class
    // os_.get<0>... works in gcc, but not in clang
    ordered_pointer_set()
        : oindex_(os_.template get<0>()), ptr_index_(os_.template get<1>()) {}

    ~ordered_pointer_set() = default;

    using iterator = typename ordered_ptr_impl::template nth_index<0>::type::iterator;
    using reverse_iterator =
        typename ordered_ptr_impl::template nth_index<0>::type::reverse_iterator;
    using const_iterator =
        typename ordered_ptr_impl::template nth_index<0>::type::const_iterator;
    using const_reverse_iterator =
        typename ordered_ptr_impl::template nth_index<0>::type::const_reverse_iterator;

    bool empty() const { return oindex_.empty(); }
    size_t size() const { return oindex_.size(); }

    iterator begin() { return oindex_.begin(); }
    iterator end() { return oindex_.end(); }

    // dsm: Overloaded const_iterator begin/end() functions. Allows things like
    // range-based for loops inside const functions to work. This is how
    // libstdc++ implements it, see eg <bits/unordered_set.h>
    const_iterator begin() const { return oindex_.begin(); }
    const_iterator end() const { return oindex_.end(); }

    const_iterator cbegin() const { return oindex_.cbegin(); }
    const_iterator cend() const { return oindex_.cend(); }

    reverse_iterator rbegin() { return oindex_.rbegin(); }
    reverse_iterator rend() { return oindex_.rend(); }

    const_reverse_iterator crbegin() const { return oindex_.crbegin(); }
    const_reverse_iterator crend() const { return oindex_.crend(); }

    // Convenience function since we often want the minimum element
    T min() const { assert(!empty()); return *cbegin(); }
    T max() const { assert(!empty()); return *crbegin(); }

    void insert(const T& val) { ptr_index_.insert(val); }

    // dsm: Use iterator_to to switch among indexes in O(1) (old implementation
    // was pretty slow)
    iterator find(const T& val) {
        auto it = ptr_index_.find(val);
        return (it == ptr_index_.end()) ? end() : oindex_.iterator_to(*it);
    }

    const_iterator find(const T& val) const {
        auto it = ptr_index_.find(val);
        return (it == ptr_index_.end()) ? cend() : oindex_.iterator_to(*it);
    }

    iterator lower_bound(const Key& val) { return oindex_.lower_bound(val); }
    const_iterator lower_bound(const Key& val) const {
        return oindex_.lower_bound(val);
    }

    iterator upper_bound(const Key& val) { return oindex_.upper_bound(val); }
    const_iterator upper_bound(const Key& val) const {
        return oindex_.upper_bound(val);
    }

    size_t count(const T& val) const { return ptr_index_.count(val); }

    iterator erase(const_iterator it) {
        return oindex_.erase(it);
    }

    void erase(const T& val) {
        auto it = ptr_index_.find(val);
        if (it == ptr_index_.end()) {
            assert(it != ptr_index_.end());
        }
        ptr_index_.erase(it);
    }

    std::ostream& operator<<(std::ostream& os) const {
        os << "{";
        for (const T & t : oindex_) os << t << "," << std::endl;
        os << "}";
        return os;
    }

    std::string toString() const {
        std::stringstream ss;
        ss << *this;
        return ss.str();
    }
};

/**
 * An ordered pointer set with a maximum capacity.
 * Uses the Delegate/Decorator design pattern.
 */
template <typename T, typename OrderedIndex, typename Key = T>
class fixed_capacity_ordered_set {
    ordered_pointer_set<T, OrderedIndex, Key> set_;
    const size_t capacity_;

  public:
    using iterator =
        typename ordered_pointer_set<T, OrderedIndex, Key>::iterator;
    using reverse_iterator =
        typename ordered_pointer_set<T, OrderedIndex, Key>::reverse_iterator;
    using const_iterator =
        typename ordered_pointer_set<T, OrderedIndex, Key>::const_iterator;
    using const_reverse_iterator =
        typename ordered_pointer_set<T, OrderedIndex, Key>::const_reverse_iterator;

    fixed_capacity_ordered_set(size_t capacity) : capacity_(capacity) {}

    bool empty() const { return set_.empty(); }
    size_t size() const { return set_.size(); }
    size_t capacity() const { return capacity_; }
    bool full() const { return capacity() == size(); }

    iterator begin() { return set_.begin(); }
    iterator end() { return set_.end(); }

    const_iterator begin() const { return set_.begin(); }
    const_iterator end() const { return set_.end(); }

    const_iterator cbegin() const { return set_.cbegin(); }
    const_iterator cend() const { return set_.cend(); }

    reverse_iterator rbegin() { return set_.rbegin(); }
    reverse_iterator rend() { return set_.rend(); }

    const_reverse_iterator crbegin() const { return set_.crbegin(); }
    const_reverse_iterator crend() const { return set_.crend(); }

    void insert(const T& val) {
        if (full()) throw std::length_error("Insert would exceed capacity");
        set_.insert(val);
    }

    T min() const { return set_.min(); }
    T max() const { return set_.max(); }

    iterator find(const T& val) { return set_.find(val); }
    const_iterator find(const T& val) const { return set_.find(val); }

    size_t count(const T& val) const { return set_.count(val); }

    iterator erase(const_iterator it) { return set_.erase(it); }
    void erase(const T& val) { set_.erase(val); }

    std::ostream& operator<<(std::ostream& os) const {
        return set_.operator<<(os);
    }
};

template <typename T, typename OrderedIndex, typename Key>
static std::ostream&
operator<<(std::ostream& os, const ordered_pointer_set<T, OrderedIndex, Key>& ops) {
    ops.operator<<(os);
    return os;
}

// FIXME duplicating this method seems unnecessary... use an interface?
template <typename T, typename OrderedIndex, typename Key>
static std::ostream&
operator<<(std::ostream& os,
           const fixed_capacity_ordered_set<T, OrderedIndex, Key>& s) {
    s.operator<<(os);
    return os;
}
