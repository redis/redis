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

#ifndef WIN32_INTEROP_TYPES_H
#define WIN32_INTEROP_TYPES_H

#include "win32_types_hiredis.h"

/* The Posix version of Redis defines off_t as 64-bit integers, so we do the same.
 * On Windows, these types are defined as 32-bit in sys/types.h under and #ifndef _OFF_T_DEFINED
 * So we define _OFF_T_DEFINED at the project level, to make sure that that definition is never included.
 * If you get an error about re-definition, make sure to include this file before sys/types.h, or any other
 * file that include it (eg wchar.h).
 * _off_t is also defined #ifndef _OFF_T_DEFINED, so we need to define it here.
 * It is used by the CRT internally (but not by Redis), so we leave it as 32-bit.
 */

typedef __int64     off_t;
typedef long        _off_t;

#ifndef _OFF_T_DEFINED
#define _OFF_T_DEFINED
#endif

#endif
