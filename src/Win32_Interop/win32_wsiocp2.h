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

#ifndef WIN32_INTEROP_WSIOCP2_H
#define WIN32_INTEROP_WSIOCP2_H

#include <WinSock2.h>   // For SOCKADDR_STORAGE
#include "WS2tcpip.h"   // For socklen_t

/* need callback on write complete. WSIOCP_Request is used to pass parameters */
typedef struct WSIOCP_Request {
    void *client;
    void *data;
    char *buf;
    int len;
} WSIOCP_Request;

int WSIOCP_QueueNextRead(int rfd);
int WSIOCP_SocketSend(int rfd, char *buf, int len, void *eventLoop, void *client, void *data, void *proc);
int WSIOCP_Listen(int rfd, int backlog);
int WSIOCP_Accept(int rfd, struct sockaddr *sa, socklen_t *len);
int WSIOCP_SocketConnect(int rfd, const SOCKADDR_STORAGE *ss);
int WSIOCP_SocketConnectBind(int rfd, const SOCKADDR_STORAGE *ss, const char* source_addr);

#endif