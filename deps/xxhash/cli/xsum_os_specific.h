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

#ifndef XSUM_OS_SPECIFIC_H
#define XSUM_OS_SPECIFIC_H

#include "xsum_config.h"
#include <stdio.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Declared here to be implemented in user code.
 *
 * Functions like main(), but is passed UTF-8 arguments even on Windows.
 */
XSUM_API int XSUM_main(int argc, const char* argv[]);

/*
 * Returns whether stream is a console.
 *
 * Functionally equivalent to isatty(fileno(stream)).
 */
XSUM_API int XSUM_isConsole(FILE* stream);

/*
 * Sets stream to pure binary mode (a.k.a. no CRLF conversions).
 */
XSUM_API void XSUM_setBinaryMode(FILE* stream);

/*
 * Returns whether the file at filename is a directory.
 */
XSUM_API int XSUM_isDirectory(const char* filename);

/*
 * Returns the file size of the file at filename.
 */
XSUM_API XSUM_U64 XSUM_getFileSize(const char* filename);

/*
 * UTF-8 stdio wrappers primarily for Windows
 */

/*
 * fopen() wrapper. Accepts UTF-8 filenames on Windows.
 *
 * Specifically, on Windows, the arguments will be converted to UTF-16
 * and passed to _wfopen().
 */
XSUM_API FILE* XSUM_fopen(const char* filename, const char* mode);

/*
 * vfprintf() wrapper which prints UTF-8 strings to Windows consoles
 * if applicable.
 */
XSUM_ATTRIBUTE((__format__(__printf__, 2, 0)))
XSUM_API int XSUM_vfprintf(FILE* stream, const char* format, va_list ap);

#ifdef __cplusplus
}
#endif

#endif /* XSUM_OS_SPECIFIC_H */
