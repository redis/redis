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

#ifndef WIN32WSIOCP_H
#define WIN32WSIOCP_H

#ifdef _WIN32
/* structs and functions for using IOCP with windows sockets */

/* structure used for async write requests.
 * contains overlapped, WSABuf, and callback info
 * NOTE: OVERLAPPED must be first member */
typedef struct asendreq {
    OVERLAPPED ov;
    WSABUF wbuf;
    aeWinSendReq req;
    aeFileProc *proc;
    aeEventLoop *eventLoop;
} asendreq;

/* structure used for async accept requests.
 * contains overlapped, accept socket, accept buffer
 * NOTE: OVERLAPPED must be first member */
typedef struct aacceptreq {
    OVERLAPPED ov;
    SOCKET accept;
    void *buf;
    struct aacceptreq *next;
} aacceptreq;


/* per socket information */
typedef struct aeSockState {
    int masks;
    int fd;
    aacceptreq *reqs;
    int wreqs;
    OVERLAPPED ov_read;
    list wreqlist;
} aeSockState;

typedef aeSockState * fnGetSockState(void *apistate, int fd);
typedef void fnDelSockState(void *apistate, aeSockState *sockState);

#define READ_QUEUED         0x000100
#define SOCKET_ATTACHED     0x000400
#define ACCEPT_PENDING      0x000800
#define LISTEN_SOCK         0x001000
#define CONNECT_PENDING     0x002000
#define CLOSE_PENDING       0x004000

void aeWinInit(void *state, HANDLE iocp, fnGetSockState *getSockState, fnDelSockState *delSockState);
void aeWinCleanup();

#endif
#endif
