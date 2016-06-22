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

#pragma once

/* The Posix version of Redis defines off_t as 64-bit integers, so we do the same.
 * On Windows, these types are defined as 32-bit in sys/types.h under and #ifndef _OFF_T_DEFINED
 * So we define _OFF_T_DEFINED at the project level, to make sure that that definition is never included.
 * If you get an error about re-definition, make sure to include this file before sys/types.h, or any other
 * file that include it (eg wchar.h).
 * _off_t is also defined #ifndef _OFF_T_DEFINED, so we need to define it here.
 * It is used by the CRT internally (but not by Redis), so we leave it as 32-bit.
 */

typedef __int64             off_t;
typedef long                _off_t;

#ifndef _OFF_T_DEFINED
#define _OFF_T_DEFINED
#endif

/* On 64-bit *nix and Windows use different data type models: LP64 and LLP64 respectively.
 * The main difference is that 'long' is 64-bit on 64-bit *nix and 32-bit on 64-bit Windows. 
 * The Posix version of Redis makes many assumptions about long being 64-bit and the same size
 * as pointers.
 * To deal with this issue, we replace all occurrences of 'long' in antirez code with our own typedefs,
 * and make those definitions 64-bit to match antirez' assumptions.
 * This enables us to have merge check script to verify that no new instances of 'long' go unnoticed.
 */

typedef __int64                 PORT_LONGLONG;
typedef unsigned __int64        PORT_ULONGLONG;
typedef double                  PORT_LONGDOUBLE;

#ifdef _WIN64
  typedef __int64               ssize_t;
  typedef __int64               PORT_LONG;
  typedef unsigned __int64      PORT_ULONG;
#else
  typedef long                  ssize_t;
  typedef long                  PORT_LONG;
  typedef unsigned long         PORT_ULONG;
#endif

#ifdef _WIN64
  #define PORT_LONG_MAX     _I64_MAX
  #define PORT_LONG_MIN     _I64_MIN
  #define PORT_ULONG_MAX    _UI64_MAX
#else
  #define PORT_LONG_MAX     LONG_MAX
  #define PORT_LONG_MIN     LONG_MIN
  #define PORT_ULONG_MAX    ULONG_MAX
#endif

/* The maximum possible size_t value has all bits set */
#define MAX_SIZE_T          (~(size_t)0)

typedef int                 pid_t;

#ifdef _WIN64
  #define PORT_STRTOL       strtoll
#else
  #define PORT_STRTOL       strtol
#endif