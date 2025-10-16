/*
 * xsum_bench - Benchmark functions for xxhsum
 * Copyright (C) 2013-2021 Yann Collet
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

#ifndef XSUM_BENCH_H
#define XSUM_BENCH_H

#include <stddef.h>  /* size_t */

#define NBLOOPS_DEFAULT    3    /* Default number of benchmark iterations */

extern int const g_nbTestFunctions;
extern char g_testIDs[];  /* size : g_nbTestFunctions */
extern const char k_testIDs_default[];
extern int g_nbIterations;

int XSUM_benchInternal(size_t keySize);
int XSUM_benchFiles(const char* fileNamesTable[], int nbFiles);


#ifdef __cplusplus
extern "C" {
#endif


#ifdef __cplusplus
}
#endif

#endif /* XSUM_BENCH_H */
