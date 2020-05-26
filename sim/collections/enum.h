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

#include <boost/range/iterator_range.hpp>
#include <iterator>
#include "sim/assert.h"  // dsm: boost includes generic C++ assert; override

// Helper functions: Get value of enum
template <typename Enumeration>
auto asValue(Enumeration const value) ->
    typename std::underlying_type<Enumeration>::type {
    return static_cast<typename std::underlying_type<Enumeration>::type>(value);
}

template <typename Enumeration> uint32_t asUInt(Enumeration const value) {
    return static_cast<uint32_t>(asValue(value));
}

// Iterator for enum class; hacky, not particularly robust
// Source:
// http://coliru.stacked-crooked.com/view?id=bcee5da83cd5c0738a17962bc00ee82d
template <class enum_type> class enum_iterator {
  private:
    enum_type value;
    typedef typename std::underlying_type<enum_type>::type under;

  public:
    typedef std::size_t size_type;
    typedef std::ptrdiff_t difference_type;
    typedef enum_type value_type;
    typedef enum_type reference;
    typedef enum_type* pointer;
    typedef std::random_access_iterator_tag iterator_category;

    constexpr enum_iterator() : value() {}
    constexpr enum_iterator(const enum_iterator& rhs) noexcept(true)
        : value(rhs.value) {}
    constexpr explicit enum_iterator(enum_type value_) noexcept(true)
        : value(value_) {}
    ~enum_iterator() noexcept(true) {}
    enum_iterator& operator=(const enum_iterator& rhs) noexcept(true) {
        value = rhs.valud;
        return *this;
    }
    enum_iterator& operator++() noexcept(true) {
        value = (enum_type)(under(value) + 1);
        return *this;
    }
    enum_iterator operator++(int) noexcept(true) {
        enum_iterator r(*this);
        ++*this;
        return r;
    }
    enum_iterator& operator+=(size_type o) noexcept(true) {
        value = (enum_type)(under(value) + o);
        return *this;
    }
    friend constexpr enum_iterator operator+(const enum_iterator& it,
                                             size_type o) noexcept(true) {
        return enum_iterator((enum_type)(under(it) + o));
    }
    friend constexpr enum_iterator operator+(
        size_type o, const enum_iterator& it) noexcept(true) {
        return enum_iterator((enum_type)(under(it) + o));
    }
    enum_iterator& operator--() noexcept(true) {
        value = (enum_type)(under(value) - 1);
        return *this;
    }
    enum_iterator operator--(int) noexcept(true) {
        enum_iterator r(*this);
        --*this;
        return r;
    }
    enum_iterator& operator-=(size_type o) noexcept(true) {
        value = (enum_type)(under(value) + o);
        return *this;
    }
    friend constexpr enum_iterator operator-(const enum_iterator& it,
                                             size_type o) noexcept(true) {
        return enum_iterator((enum_type)(under(it) - o));
    }
    friend constexpr difference_type operator-(
        enum_iterator lhs, enum_iterator rhs) noexcept(true) {
        return under(lhs.value) - under(rhs.value);
    }
    constexpr reference operator*() const noexcept(true) { return value; }
    constexpr reference operator[](size_type o) const noexcept(true) {
        return (enum_type)(under(value) + o);
    }
    constexpr const enum_type* operator->() const noexcept(true) {
        return &value;
    }
    constexpr friend bool operator==(const enum_iterator& lhs,
                                     const enum_iterator& rhs) noexcept(true) {
        return lhs.value == rhs.value;
    }
    constexpr friend bool operator!=(const enum_iterator& lhs,
                                     const enum_iterator& rhs) noexcept(true) {
        return lhs.value != rhs.value;
    }
    constexpr friend bool operator<(const enum_iterator& lhs,
                                    const enum_iterator& rhs) noexcept(true) {
        return lhs.value < rhs.value;
    }
    constexpr friend bool operator>(const enum_iterator& lhs,
                                    const enum_iterator& rhs) noexcept(true) {
        return lhs.value > rhs.value;
    }
    constexpr friend bool operator<=(const enum_iterator& lhs,
                                     const enum_iterator& rhs) noexcept(true) {
        return lhs.value <= rhs.value;
    }
    constexpr friend bool operator>=(const enum_iterator& lhs,
                                     const enum_iterator& rhs) noexcept(true) {
        return lhs.value >= rhs.value;
    }
    friend void swap(const enum_iterator& lhs,
                     const enum_iterator& rhs) noexcept(true) {
        std::swap(lhs.value, rhs.value);
    }
};

template <class enum_type>
constexpr boost::iterator_range<enum_iterator<enum_type>> get_range() noexcept(
    true) {
    return boost::make_iterator_range(
        enum_iterator<enum_type>(enum_type::FIRST),
        enum_iterator<enum_type>(enum_type::LAST));
}

