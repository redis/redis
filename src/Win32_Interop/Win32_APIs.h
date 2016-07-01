/*
 * Copyright (c), Microsoft Open Technologies, Inc.
 * All rights reserved.
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *  - Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  - Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef WIN32_INTEROP_APIS_H
#define WIN32_INTEROP_APIS_H

#include "Win32_types.h"
#include <Windows.h>
#include <stdio.h>      // for rename

// API replacement for non-fd stdio functions
#define fseeko      _fseeki64
#define ftello      _ftelli64
#define snprintf    _snprintf
#define strcasecmp  _stricmp
#define strtoll     _strtoi64

#ifdef _WIN64
#define strtol      _strtoi64
#define strtoul     _strtoui64
#endif

#define sleep(x) Sleep((x)*1000)
/* Redis calls usleep(1) to give thread some time.
 * Sleep(0) should do the same on Windows.
 * In other cases, usleep is called with millisec resolution
 * which can be directly translated to WinAPI Sleep() */
#undef usleep
#define usleep(x) (x == 1) ? Sleep(0) : Sleep((int)((x)/1000))


/* following defined to choose little endian byte order */
#define __i386__ 1
#if !defined(va_copy)
#define va_copy(d,s)  d = (s)
#endif

#ifndef __RTL_GENRANDOM
#define __RTL_GENRANDOM 1
typedef BOOLEAN(_stdcall* RtlGenRandomFunc)(void * RandomBuffer, ULONG RandomBufferLength);
#endif
RtlGenRandomFunc RtlGenRandom;

#define random()    replace_random()
#define rand()      replace_random()
#define srandom     srand
int replace_random();

#define rename(a,b) replace_rename(a,b)
int replace_rename(const char *src, const char *dest);

int truncate(const char *path, PORT_LONGLONG length);

#define lseek lseek64

#endif