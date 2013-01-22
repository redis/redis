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

#include "redis.h"
#include "win32_wsiocp.h"

#ifdef _WIN32

DWORD WINAPI BkgdSaveThreadProc(LPVOID param);
void bkgdsave_cleanup();


/* start a background save using a windows thread.
 * used for rdb save and aof save */
int bkgdsave_start(const char *filename, int (*bkgdfsave_serialize)(char *)) {
    if (server.rdbbkgdfsave.state != BKSAVE_IDLE) {
        /* only one background activity at a time is allowed */
        errno = EINVAL;
        return -1;
    }
    server.rdbbkgdfsave.state = BKSAVE_WRITING;
    cowBkgdSaveStart();

    if (server.rdbbkgdfsave.thread == NULL) {
        server.rdbbkgdfsave.dosaveevent = CreateEvent(NULL, FALSE, FALSE, NULL);
        if (server.rdbbkgdfsave.dosaveevent == NULL) {
            goto failed;
        }

        server.rdbbkgdfsave.terminateevent = CreateEvent(NULL, FALSE, FALSE, NULL);
        if (server.rdbbkgdfsave.terminateevent == NULL) {
            goto failed;
        }

        server.rdbbkgdfsave.thread = CreateThread(NULL, 0, BkgdSaveThreadProc, &server.rdbbkgdfsave, 0, NULL);
        if (server.rdbbkgdfsave.thread == NULL) {
            goto failed;
        }
    }

    server.rdbbkgdfsave.filename = (char*)zmalloc(strlen(filename) + 1);
    strcpy(server.rdbbkgdfsave.filename, filename);
    server.rdbbkgdfsave.bkgdfsave_serialize = bkgdfsave_serialize;

    /* signal background thread to run */
    SetEvent(server.rdbbkgdfsave.dosaveevent);
    return REDIS_OK;

failed:
    bkgdsave_cleanup();
    errno = EINVAL;
    return -1;
}

/* terminate the background save thread */
int bkgdsave_termthread() {
    if (server.rdbbkgdfsave.terminateevent != NULL && server.rdbbkgdfsave.thread != NULL) {
        SetEvent(server.rdbbkgdfsave.terminateevent);
        WaitForSingleObject(server.rdbbkgdfsave.thread, INFINITE);
    }
    bkgdsave_cleanup();
    return 0;
}


/* cleanup state for thread termination */
void bkgdsave_cleanup() {
    if (server.rdbbkgdfsave.dosaveevent != NULL) {
        CloseHandle(server.rdbbkgdfsave.dosaveevent);
        server.rdbbkgdfsave.dosaveevent = NULL;
    }
    if (server.rdbbkgdfsave.terminateevent != NULL) {
        CloseHandle(server.rdbbkgdfsave.terminateevent);
        server.rdbbkgdfsave.terminateevent = NULL;
    }
    if (server.rdbbkgdfsave.thread != NULL) {
        CloseHandle(server.rdbbkgdfsave.thread);
        server.rdbbkgdfsave.thread = NULL;
    }
    if (server.rdbbkgdfsave.filename != NULL) {
        zfree(server.rdbbkgdfsave.filename);
        server.rdbbkgdfsave.filename = NULL;
    }
    if (server.rdbbkgdfsave.tmpname != NULL) {
        zfree(server.rdbbkgdfsave.tmpname);
        server.rdbbkgdfsave.tmpname = NULL;
    }
}

/* initialize the background save state */
void bkgdsave_init() {
    server.rdbbkgdfsave.dosaveevent = NULL;
    server.rdbbkgdfsave.terminateevent = NULL;
    server.rdbbkgdfsave.thread = NULL;
    server.rdbbkgdfsave.state = BKSAVE_IDLE;
    server.rdbbkgdfsave.filename = NULL;
    server.rdbbkgdfsave.tmpname = NULL;
}


/* background thread to write buffers to disk */
DWORD WINAPI BkgdSaveThreadProc(LPVOID param) {
    HANDLE workorterm[2];
    int rc = REDIS_OK;

    workorterm[0] = server.rdbbkgdfsave.terminateevent;
    workorterm[1] = server.rdbbkgdfsave.dosaveevent;

    while (1) {

        DWORD ev = WaitForMultipleObjects(2, workorterm, FALSE, INFINITE);
        if (ev != (WAIT_OBJECT_0 + 1)) {
            /* terminate or unexpected return, do exit */
            bkgdsave_cleanup();
            return 0;
        }

        /* start saving data into buffers */
        server.rdbbkgdfsave.background = 1;
        rc = server.rdbbkgdfsave.bkgdfsave_serialize(server.rdbbkgdfsave.filename);
        server.rdbbkgdfsave.background = 0;

        if (rc == REDIS_OK)
            server.rdbbkgdfsave.state = BKSAVE_SUCCESS;
        else
            server.rdbbkgdfsave.state = BKSAVE_FAILED;
    }

    return 0;
}

#endif
