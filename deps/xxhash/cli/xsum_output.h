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

#ifndef XSUM_OUTPUT_H
#define XSUM_OUTPUT_H

#include "xsum_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * How verbose the output is.
 */
extern int XSUM_logLevel;

/*
 * Same as fprintf(stderr, format, ...)
 */
XSUM_ATTRIBUTE((__format__(__printf__, 1, 2)))
XSUM_API int XSUM_log(const char *format, ...);

/*
 * Like XSUM_log, but only outputs if XSUM_logLevel >= minLevel.
 */
XSUM_ATTRIBUTE((__format__(__printf__, 2, 3)))
XSUM_API int XSUM_logVerbose(int minLevel, const char *format, ...);

/*
 * Same as printf(format, ...)
 */
XSUM_ATTRIBUTE((__format__(__printf__, 1, 2)))
XSUM_API int XSUM_output(const char *format, ...);

#ifdef __cplusplus
}
#endif

#endif /* XSUM_OUTPUT_H */
