/** $lic$
 * Copyright (C) 2012-2021 by Massachusetts Institute of Technology
 * Copyright (C) 2010-2012 by The Board of Trustees of Stanford University
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

/* This file was adapted from zsim. */

#ifndef CONFIG_H_
#define CONFIG_H_

/* Thin wrapper around libconfig to:
 * - Reduce and simplify init code (tailored interface, not type BS, ...)
 * - Strict config: type errors, warnings on unused variables, panic on different defaults
 * - Produce a full configuration file with all the variables, including defaults (for config parsing, comparison, etc.)
 */

#include <stdint.h>
#include <string>
#include <vector>
#include "sim/assert.h"
#include "sim/log.h"

namespace libconfig {
    class Config;
    class Setting;
};


class Config {
    private:
        libconfig::Config* inCfg;
        libconfig::Config* outCfg;

    public:
        explicit Config(const char* inFile);
        ~Config();

        //Called when initialization ends. Writes output config, and emits warnings for unused input settings
        void writeAndClose(const char* outFile, bool strictCheck);

        bool exists(const char* key) const;
        bool exists(const std::string& key) const {return exists(key.c_str());}

        //Access interface
        //T can be uint32_t, uint64_t, bool, or const char*. Instantiations are in the cpp file

        // Mandatory values (no default, panics if setting does not exist)
        template<typename T> T get(const char* key) const;
        template<typename T> T get(const std::string& key) const {
            return get<T>(key.c_str());
        }

        // Optional values (default)
        template<typename T> T get(const char* key, T def) const;
        template<typename T> T get(const std::string& key, T def) const {
            return get<T>(key.c_str(), def);
        }

        // Get optional values (with default) with an alternate name.
        // (This is useful for backwards compatibility when changing a config name)
        template <typename T>
        T get(const char* canonicalName, const char* altName, T def) const {
            if (exists(canonicalName)) return get<T>(canonicalName);
            else if (exists(altName)) return get<T>(altName);
            else return get<T>(canonicalName, def);
        }
        template <typename T>
        T get(const std::string& canonicalName, const std::string& altName, T def) const {
            return get<T>(canonicalName.c_str(), altName.c_str(), def);
        }

        //Get subgroups in a specific key
        void subgroups(const char* key, std::vector<const char*>& grps) const;
        void subgroups(const std::string& key,
                std::vector<const char*>& grps) const {
            subgroups(key.c_str(), grps);
        }

    private:
        template<typename T> T genericGet(const char* key) const;
        template<typename T> T genericGet(const char* key, T def) const;
};


/* Parsing functions used for configuration */

std::vector<bool> ParseMask(const std::string& maskStr, uint32_t maskSize);

/* Parses a space-separated list of T's (typically ints, see/add specializtions in .cpp)
 * 0-elem lists are OK
 * panics on parsing and size-violation errors
 */
template <typename T> std::vector<T> ParseList(const std::string& listStr);

// fills remaining elems till maxSize with fillValue
template <typename T> std::vector<T> ParseList(const std::string& listStr, uint32_t maxSize, uint32_t fillValue) {
    std::vector<T> res = ParseList<T>(listStr);
    if (res.size() > maxSize) panic("ParseList: Too many elements, max %d, got %ld", maxSize, res.size());
    while (res.size() < maxSize) res.push_back(fillValue);
    return res;
}

void Tokenize(const std::string& str, std::vector<std::string>& tokens, const std::string& delimiters);

#endif  // CONFIG_H_
