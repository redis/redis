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

#ifndef __W32BKSAV_H__
#define __W32BKSAV_H__

#ifdef _WIN32

#include "adlist.h"

#define BKSAVE_IDLE         0
#define BKSAVE_BUFFERING    1
#define BKSAVE_WRITING      3
#define BKSAVE_SUCCESS      4
#define BKSAVE_FAILED       5


/* each buffer has a current postion and remaining space */
typedef struct bkgdfsavehdr {
    size_t pos;
    size_t rem;
} bkgdfsavehdr;

typedef struct bkgdfsave {
    int background;
    int state;
    HANDLE dosaveevent;
    HANDLE terminateevent;
    HANDLE thread;
    char *filename;
    char *tmpname;
    int (*bkgdfsave_serialize)(char *);
} bkgdfsave;

void bkgdsave_init();
int bkgdsave_start(const char *filename, int (*bkgdfsave_serialize)(char *));
int bkgdsave_termthread();

#endif

#endif
