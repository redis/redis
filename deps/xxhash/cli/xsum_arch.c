/*
 * xxhsum - Command line interface for xxhash algorithms
 * Copyright (C) 2013-2024 Yann Collet
 *
 * GPL v2 License
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * You can contact the author at:
 *   - xxHash homepage: https://www.xxhash.com
 *   - xxHash source repository: https://github.com/Cyan4973/xxHash
 */

int g_xsumarch_avoid_empty_unit = 0;

#if ((defined(__x86_64__) || defined(_M_X64)) && !defined(_M_ARM64EC)) || defined(__i386__) || defined(_M_IX86) || defined(_M_IX86_FP)
#if defined(XXHSUM_DISPATCH)

#include "../xxh_x86dispatch.h"

const char* XSUM_autox86(void)
{
    int vecVersion = XXH_featureTest();
    switch(vecVersion) {
        case XXH_SCALAR:
            return "x86 autoVec (scalar: no vector extension detected)";
        case XXH_SSE2:
            return "x86 autoVec (SSE2 detected)";
        case XXH_AVX2:
            return "x86 autoVec (AVX2 detected)";
        case XXH_AVX512:
            return "x86 autoVec (AVX512 detected)";
        default:;
    }
    return " autoVec (error detecting vector extension)";
}

#endif /* XXHSUM_DISPATCH */
#endif /* x86 */
