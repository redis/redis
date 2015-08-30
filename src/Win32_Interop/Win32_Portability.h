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

#ifndef WIN32_INTEROPA_PORTABILITY_H
#define WIN32_INTEROPA_PORTABILITY_H


#ifdef __cplusplus
extern "C"
{
#endif

/*  Sometimes in the Windows port we make changes from:
        antirez_redis_statement();
    to:
        #ifdef _WIN32
            windows_redis_statement();
        #else
            antirez_redis_statement();
        #endif

    If subsequently antirez changed that code, we might not detect the change during the next merge.
    The INDUCE_MERGE_CONFLICT macro expands to nothing, but it is used to make sure that the original line
    is modified with respect to the antirez version, so that any subsequent modifications will trigger a conflict
    during the next merge.

    Sample usage:
        #ifdef _WIN32
            windows_redis_statement();
        #else
            antirez_redis_statement();          INDUCE_MERGE_CONFLICT
        #endif

    Don't use any parenthesis or semi-colon after INDUCE_MERGE_CONFLICT.
    Use it at the end of a line to preserve the original indentation.
*/
#define INDUCE_MERGE_CONFLICT

/*  Use WIN_PORT_FIX at the end of a line to mark places where we make changes to the code
    without using #ifdefs. Useful to keep the code more legible. Mainly intended for replacing
    the use of long (which is 64-bit on 64-bit Unix and 32-bit on 64-bit Windows) to portable types.
    In order to be eligible for an inline fix (without #ifdef), the change should be portable back to the Posix version.
*/
#define WIN_PORT_FIX

#ifdef _WIN32
#define IF_WIN32(x, y) x
#define WIN32_ONLY(x) x
#define POSIX_ONLY(x)
#define inline __inline
#else
#define IF_WIN32(x, y) y
#define WIN32_ONLY(x)
#define POSIX_ONLY(x) x
#endif

#ifdef __cplusplus
}
#endif

#endif
