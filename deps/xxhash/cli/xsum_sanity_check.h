/*
 * xxhsum - Command line interface for xxhash algorithms
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

#ifndef XSUM_SANITY_CHECK_H
#define XSUM_SANITY_CHECK_H

#include "xsum_config.h"  /* XSUM_API, XSUM_U8 */

#include <stddef.h>   /* size_t */


#ifdef __cplusplus
extern "C" {
#endif

/*
 * Runs a series of self-tests.
 *
 * Exits if any of these tests fail, printing a message to stderr.
 *
 * If XSUM_NO_TESTS is defined to non-zero,
 * this will instead print a warning if this is called (e.g. via xxhsum -b).
 */
XSUM_API void XSUM_sanityCheck(void);

/*
 * Fills a test buffer with pseudorandom data.
 *
 * This is used in the sanity check and the benchmarks.
 * Its values must not be changed.
 */
XSUM_API void XSUM_fillTestBuffer(XSUM_U8* buffer, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* XSUM_SANITY_CHECK_H */
