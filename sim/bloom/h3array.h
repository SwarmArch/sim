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

// Modified from:
//     http://www.nersc.gov/~scottc/misc/docs/bro.1.0.3/H3_8h-source.html

// H3 hash function family
// C++ template implementation by Ken Keys (kkeys@caida.org)
//
// Usage:
//    #include <h3.h>
//    const H3<T, N> h;
//    T hashval = h(data);
// (T) is the type to be returned by the hash function; must be an integral
//     type, e.g. uint32_t.
// (M) is the maximum return value of the operator()
// (N) is the size of the data in bytes (assumed to be 4 for RingSTM)
// (K) is the size of this hash function array
// The hash function assumes 4 byte size of key, and hashes the key directly
// Any number of hash functions can be created by creating new instances of H3,
//     with the same or different template parameters.  The hash function is
//     randomly generated using random(); you must call srandom() before the
//     H3 constructor if you wish to seed it.

#ifndef H3ARRAY_H
#define H3ARRAY_H

#if defined(__SSE2__)
#include <xmmintrin.h>
#endif
#include <stdlib.h>  // for rand()

#define H3ARRAY_N (sizeof(const void* const))

// Fix the data size to be 4 bytes
template<typename T, int BOUND, int K>
class H3Array
{
    T byte_lookup[K][H3ARRAY_N][256];
public:
    H3Array();
    ~H3Array() { /*free(byte_lookup); */}

    __attribute__((always_inline))
    T operator()(const void* const key , const unsigned i) const
    {
        const unsigned char * const p =
            (const unsigned char* const)(&key);

        T result1 = byte_lookup[i][0][p[0]] ^ byte_lookup[i][1][p[1]];
        T result2 = byte_lookup[i][2][p[2]] ^ byte_lookup[i][3][p[3]];
        return (result1 ^ result2);
    }
};

template<typename T, int BOUND, int K>
H3Array<T,BOUND,K>::H3Array()
{
    T bit_lookup[H3ARRAY_N * 8];

    for (size_t hidx = 0; hidx < K; hidx++)
    {
        for (size_t bit = 0; bit < H3ARRAY_N * 8; bit++) {
            bit_lookup[bit] = 0;
            for (size_t i = 0; i < sizeof(T)/2; i++) {
                // assume random() returns at least 16 random bits
                bit_lookup[bit] = (bit_lookup[bit] << 16) | (rand() & 0xFFFF);
            }
            bit_lookup[bit] = bit_lookup[bit] % BOUND;
        }

        for (size_t byte = 0; byte < H3ARRAY_N; byte++) {
            for (unsigned val = 0; val < 256; val++) {
                byte_lookup[hidx][byte][val] = 0;
                for (size_t bit = 0; bit < 8; bit++) {
                    if (val & (1 << bit))
                        byte_lookup[hidx][byte][val] ^= bit_lookup[byte*8+bit];
                }
            }
        }
    }
}

#endif //H3_H
