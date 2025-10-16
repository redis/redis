/*
 * Multi-include test program
 * ensure that pixel, bool and vector are not redefined
 *
 * Copyright (C) 2020 Yann Collet
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

/* gcc's altivec.h, included for the VSX code path,
 * may, in some circumstances, redefine
 * bool, vector and pixel keywords.
 *
 * This unit checks if it happens.
 * It's a compile test.
 * The test is mostly meaningful for PPC target using altivec.h
 * hence XXH_VECTOR == XXH_VSX
 */

#define BOOL_VALUE 32123456
#define bool BOOL_VALUE

#define VECTOR_VALUE 374464784
#define vector VECTOR_VALUE

#define PIXEL_VALUE 5846841
#define pixel PIXEL_VALUE

#define XXH_INLINE_ALL
#include "../xxhash.h"

#if (bool != BOOL_VALUE)
#  error "bool macro was redefined !"
#endif

#if (vector != VECTOR_VALUE)
#  error "vector macro was redefined !"
#endif

#if (pixel != PIXEL_VALUE)
#  error "pixel macro was redefined !"
#endif

int g_nonEmptyUnit = 0;
