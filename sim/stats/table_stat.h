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

#ifndef _TABLE_STAT_H_
#define _TABLE_STAT_H_

#include <cstring>
#include <string>
#include <vector>
#include "sim/stats/stats.h"

class TableStat : public AggregateStat {
  public:
    class TableRowReference {
      public:
        TableRowReference(uint64_t* base, size_t size)
          : _values(base), _size(size) {}

        inline TableRowReference& operator+=(const VectorCounter& that) {
            assert(that.size() == _size);
            for (uint32_t i = 0; i < _size; i++) {
                _values[i] += that.count(i);
            }
            return *this;
        }

        inline TableRowReference& operator-=(const VectorCounter& that) {
            assert(that.size() == _size);
            for (uint32_t i = 0; i < _size; i++) {
                _values[i] -= that.count(i);
            }
            return *this;
        }

      private:
        // Mutable slice of the TableStat values that represents a row.
        uint64_t* _values;
        size_t _size;
    };

    TableStat() : _values(nullptr) {}

    ~TableStat() override {
        for (auto& row : _rowStats) {
            free(const_cast<char*>(row.name()));
        }
        if (_values) delete [] _values;
    }

    void init(const char* name, const char* desc,
              std::vector<std::string> rowNames,
              std::vector<std::string> columnNames);

    inline TableRowReference row(uint32_t r) {
        return TableRowReference(&_values[r * _ncols], _ncols);
    }

    inline VectorCounter row(uint32_t r) const {
        VectorCounter vc;
        // FIXME(mcj) is it necessary that the names match here?
        vc.init("", "", _ncols);
        for (uint32_t c = 0; c < _ncols; c++) {
            vc.set(c, _values[r * _ncols + c]);
        }
        return vc;
    }

    inline uint64_t element(uint32_t r, uint32_t c) const {
        return _values[r * _ncols + c];
    }

    inline uint64_t total() const {
        return std::accumulate(_values, _values + _nrows * _ncols, 0ul);
    }

    inline void inc(uint32_t r, uint32_t c, uint64_t value) {
        _values[r * _ncols + c] += value;
    }

    inline void inc(uint32_t r, uint32_t c) {
        _values[r * _ncols + c]++;
    }

    inline TableStat& operator+=(const TableStat& that) {
        std::transform(_values,
                       _values + _nrows * _ncols,
                       that._values, _values, std::plus<uint64_t>());
        return *this;
    }

    inline TableStat& operator-=(const TableStat& that) {
        std::transform(_values,
                       _values + _nrows * _ncols,
                       that._values, _values, std::minus<uint64_t>());
        return *this;
    }

  private:
    uint64_t* _values;
    size_t _nrows;
    size_t _ncols;

    class TableRowStat : public VectorStat {
      public:
        TableRowStat() : _values(nullptr) {}

        void init(const char* name,
                  const vector<std::string>& counterNames,
                  const uint64_t* values) {
            initStat(name, "");
            _counterNames = counterNames;
            _values = values;
        }

        uint64_t count(uint32_t idx) const override { return _values[idx]; }

        uint32_t size() const override {
            assert(hasCounterNames());
            return _counterNames.size();
        }

      private:
        // Read-only slice of the TableStat values that represents a row.
        const uint64_t* _values;
    };

    std::vector<TableRowStat> _rowStats;
};

TableStat operator+(const TableStat&, const TableStat&);
TableStat operator-(const TableStat&, const TableStat&);


#endif  // _TABLE_STAT_H_
