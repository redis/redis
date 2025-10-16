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

#include "xsum_output.h"
#include "xsum_os_specific.h"  /* XSUM_API */

int XSUM_logLevel = 2;

XSUM_ATTRIBUTE((__format__(__printf__, 1, 2)))
XSUM_API int XSUM_log(const char* format, ...)
{
    int ret;
    va_list ap;
    va_start(ap, format);
    ret = XSUM_vfprintf(stderr, format, ap);
    va_end(ap);
    return ret;
}


XSUM_ATTRIBUTE((__format__(__printf__, 1, 2)))
XSUM_API int XSUM_output(const char* format, ...)
{
    int ret;
    va_list ap;
    va_start(ap, format);
    ret = XSUM_vfprintf(stdout, format, ap);
    va_end(ap);
    return ret;
}

XSUM_ATTRIBUTE((__format__(__printf__, 2, 3)))
XSUM_API int XSUM_logVerbose(int minLevel, const char* format, ...)
{
    if (XSUM_logLevel >= minLevel) {
        int ret;
        va_list ap;
        va_start(ap, format);
        ret = XSUM_vfprintf(stderr, format, ap);
        va_end(ap);
        return ret;
    }
    return 0;
}
