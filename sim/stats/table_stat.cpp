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

#include "table_stat.h"

void TableStat::init(const char* name, const char* desc,
          std::vector<std::string> rowNames,
          std::vector<std::string> columnNames) {
    initStat(name, desc);
    _nrows = rowNames.size();
    _ncols = columnNames.size();
    _rowStats.resize(_nrows);
    _values = new uint64_t[_nrows * _ncols];
    for (size_t r = 0; r < _nrows; r++) {
        _rowStats[r].init(strdup(rowNames[r].c_str()), columnNames,
                          &_values[r * _ncols]);
        this->append(&_rowStats[r]);
    }
    std::fill(_values, _values + _nrows * _ncols, 0ul);
}


TableStat operator+(const TableStat& lhs, const TableStat& rhs) {
    TableStat result(lhs);
    result += rhs;
    return result;
}

TableStat operator-(const TableStat& lhs, const TableStat& rhs) {
    TableStat result(lhs);
    result -= rhs;
    return result;
}

