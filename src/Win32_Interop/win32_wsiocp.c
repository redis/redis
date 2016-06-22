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

#include "win32fixes.h"
#include "..\ae.h"
#include "..\adlist.h"
#include <mswsock.h>
#include "win32_wsiocp.h"
#include "Win32_FDAPI.h"
#include "Win32_Assert.h"
#include <errno.h>

static HANDLE iocph;

#define SUCCEEDED_WITH_IOCP(result)  ((result) || (GetLastError() == ERROR_IO_PENDING))

/* For zero length reads use shared buf */
static DWORD wsarecvflags;
static char zreadchar[1];

aeSockState* WSIOCP_GetExistingSocketState(int rfd) {
    aeSockState** socketState = (aeSockState**) FDAPI_GetSocketStatePtr(rfd);
    if (socketState == NULL) {
        return NULL;
    } else {
        return *socketState;
    }
}

/* Get the socket state. Create if not found. */
aeSockState* WSIOCP_GetSocketState(int rfd) {
    aeSockState** socketState = (aeSockState**) FDAPI_GetSocketStatePtr(rfd);
    if (socketState == NULL) {
        return NULL;
    } else {
        if (*socketState == NULL) {
            // Not found. Do lazy create of socket state.
            *socketState = (aeSockState *) CallocMemoryNoCOW(sizeof(aeSockState));
            if (*socketState != NULL) {
                (*socketState)->rfd = rfd;
            }
        }
        return *socketState;
    }
}

/* Closes the socket state or sets the CLOSE_PENDING mask bit.
   Returns TRUE if closed, FALSE if pending. */
BOOL WSIOCP_CloseSocketState(aeSockState* socketState) {
    socketState->masks &= ~(SOCKET_ATTACHED | AE_WRITABLE | AE_READABLE);
    if (socketState->wreqs == 0 &&
        (socketState->masks & (READ_QUEUED | CONNECT_PENDING)) == 0) {
        FreeMemoryNoCOW(socketState);
        return TRUE;
    } else {
        socketState->masks |= CLOSE_PENDING;
        return FALSE;
    }
}

BOOL WSIOCP_CloseSocketStateRFD(int rfd) {
    aeSockState* socketState = WSIOCP_GetExistingSocketState(rfd);
    if (socketState != NULL) {
        return WSIOCP_CloseSocketState(socketState);
    } else {
        return FALSE;
    }
}

int WSIOCP_QueueAccept(int listenfd) {
    aeSockState *sockstate;
    aeSockState *accsockstate;
    DWORD result, bytes;
    int acceptfd;
    aacceptreq * areq;

    if ((sockstate = WSIOCP_GetSocketState(listenfd)) == NULL) {
        errno = WSAEINVAL;
        return -1;
    }

    acceptfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (acceptfd == -1) {
        errno = WSAEINVAL;
        return -1;
    }

    accsockstate = WSIOCP_GetSocketState(acceptfd);
    if (accsockstate == NULL) {
        errno = WSAEINVAL;
        return -1;
    }

    accsockstate->masks = SOCKET_ATTACHED;
    // Keep accept socket in buf len until accepted
    areq = (aacceptreq *) CallocMemoryNoCOW(sizeof(aacceptreq));
    areq->buf = CallocMemoryNoCOW(sizeof(struct sockaddr_storage) * 2 + 64);
    areq->accept = acceptfd;
    areq->next = NULL;

    result = FDAPI_AcceptEx(listenfd, acceptfd,
                            areq->buf, 0,
                            sizeof(struct sockaddr_storage),
                            sizeof(struct sockaddr_storage),
                            &bytes, &areq->ov);
    if (SUCCEEDED_WITH_IOCP(result)){
        sockstate->masks |= ACCEPT_PENDING;
    } else {
        errno = WSAGetLastError();
        sockstate->masks &= ~ACCEPT_PENDING;
        accsockstate->masks = 0;
        close(acceptfd);
        FreeMemoryNoCOW(areq->buf);
        FreeMemoryNoCOW(areq);
        return -1;
    }

    return 0;
}

/* For each asynch socket, need to associate completion port */
int WSIOCP_SocketAttach(int fd) {
    if (iocph == NULL) {
        return -1;
    }

    aeSockState *sockstate;
    if ((sockstate = WSIOCP_GetSocketState(fd)) == NULL) {
        errno = WSAEINVAL;
        return -1;
    }

    // Set the socket to nonblocking mode
    DWORD yes = 1;
    if (FDAPI_ioctlsocket(fd, FIONBIO, &yes) == SOCKET_ERROR) {
        errno = WSAGetLastError();
        return -1;
    }

    // Make the socket non-inheritable
    if (!FDAPI_SetFDInformation(fd, HANDLE_FLAG_INHERIT, 0)) {
        errno = WSAGetLastError();
        return -1;
    }

    // Associate it with the I/O completion port.
    // Use fd as completion key.
    if (FDAPI_CreateIoCompletionPort(fd, iocph, (ULONG_PTR) fd, 0) == NULL) {
        errno = WSAGetLastError();
        return -1;
    }
    sockstate->masks = SOCKET_ATTACHED;
    sockstate->wreqs = 0;

    return 0;
}

/* Listen using extension function to get faster accepts */
int WSIOCP_Listen(int rfd, int backlog) {
    aeSockState *sockstate = WSIOCP_GetSocketState(rfd);
    if (sockstate == NULL) {
        errno = WSAEINVAL;
        return SOCKET_ERROR;
    }

    if (WSIOCP_SocketAttach(rfd) != 0) {
        return SOCKET_ERROR;
    }

    sockstate->masks |= LISTEN_SOCK;

    if (listen(rfd, backlog) != 0) {
        return SOCKET_ERROR;
    }

    if (WSIOCP_QueueAccept(rfd) != 0) {
        return SOCKET_ERROR;
    }

    return 0;
}

/* Return the queued accept socket */
int WSIOCP_Accept(int fd, struct sockaddr *sa, socklen_t *len) {
    aeSockState *sockstate;
    int acceptfd;
    int result;
    SOCKADDR *plocalsa = NULL;
    SOCKADDR *premotesa = NULL;
    int locallen = 0;
    int remotelen = 0;
    aacceptreq * areq;

    if ((sockstate = WSIOCP_GetSocketState(fd)) == NULL) {
        errno = WSAEINVAL;
        return SOCKET_ERROR;
    }

    areq = sockstate->reqs;
    if (areq == NULL) {
        errno = EWOULDBLOCK;
        return SOCKET_ERROR;
    }

    sockstate->reqs = areq->next;

    acceptfd = (int)areq->accept;

    result = FDAPI_UpdateAcceptContext(acceptfd);
    if (result == SOCKET_ERROR) {
        errno = WSAGetLastError();
        FreeMemoryNoCOW(areq->buf);
        FreeMemoryNoCOW(areq);
        return SOCKET_ERROR;
    }

    FDAPI_GetAcceptExSockaddrs(
                    acceptfd,
                    areq->buf,
                    0,
                    sizeof(struct sockaddr_storage),
                    sizeof(struct sockaddr_storage),
                    &plocalsa, &locallen,
                    &premotesa, &remotelen);

    if (sa != NULL) {
        if (remotelen > 0) {
            if (remotelen < *len) {
                *len = remotelen;
            }
            memcpy(sa, premotesa, *len);
        } else {
            *len = 0;
        }
    }

    WSIOCP_SocketAttach(acceptfd);

    FreeMemoryNoCOW(areq->buf);
    FreeMemoryNoCOW(areq);

    // Queue another accept
    if (WSIOCP_QueueAccept(fd) == -1) {
        return SOCKET_ERROR;
    }

    return acceptfd;
}

/* After doing a read, the caller needs to call this method in
 * order to continue to check for read events.
 * This is not necessary if the caller will delete read events */
int WSIOCP_QueueNextRead(int fd) {
    aeSockState *sockstate;
    int result;
    WSABUF zreadbuf;
    DWORD bytesReceived = 0;

    if ((sockstate = WSIOCP_GetSocketState(fd)) == NULL) {
        errno = WSAEINVAL;
        return -1;
    }
    if ((sockstate->masks & SOCKET_ATTACHED) == 0) {
        return 0;
    }

    /* use zero length read with overlapped to get notification
     of when data is available */
    memset(&sockstate->ov_read, 0, sizeof(sockstate->ov_read));
    wsarecvflags = 0;

    zreadbuf.buf = zreadchar;
    zreadbuf.len = 0;
    result = FDAPI_WSARecv(fd,
                           &zreadbuf,
                           1,
                           &bytesReceived,
                           &wsarecvflags,
                           &sockstate->ov_read,
                           NULL);
    if (SUCCEEDED_WITH_IOCP(result == 0)){
        sockstate->masks |= READ_QUEUED;
    } else {
        errno = WSAGetLastError();
        sockstate->masks &= ~READ_QUEUED;
        return -1;
    }
    return 0;
}

/* Wrapper for send.
 * Enables use of WSASend to get IOCP notification of completion.
 * Returns -1 with errno = WSA_IO_PENDING if callback will be invoked later */
int WSIOCP_SocketSend(int fd, char *buf, int len, void *eventLoop,
                      void *client, void *data, void *proc) {
    aeSockState *sockstate;
    int result;
    asendreq *areq;
    DWORD bytesSent = 0;

    sockstate = WSIOCP_GetSocketState(fd);

    if (sockstate != NULL &&
        (sockstate->masks & CONNECT_PENDING)) {
        aeWait(fd, AE_WRITABLE, 50);
    }

    // If not an async socket, do normal send
    if (sockstate == NULL ||
        (sockstate->masks & SOCKET_ATTACHED) == 0 ||
        proc == NULL) {
        result = (int) write(fd, buf, len);
        if (result == SOCKET_ERROR) {
            errno = WSAGetLastError();
        }
        return result;
    }

    // Use overlapped structure to send using IOCP
    areq = (asendreq *) CallocMemoryNoCOW(sizeof(asendreq));
    areq->wbuf.len = len;
    areq->wbuf.buf = buf;
    areq->eventLoop = (aeEventLoop *)eventLoop;
    areq->req.client = client;
    areq->req.data = data;
    areq->req.len = len;
    areq->req.buf = buf;
    areq->proc = (aeFileProc *)proc;

    result = FDAPI_WSASend(fd,
                           &areq->wbuf,
                           1,
                           &bytesSent,
                           0,
                           &areq->ov,
                           NULL);

    if (SUCCEEDED_WITH_IOCP(result == 0)){
        errno = WSA_IO_PENDING;
        sockstate->wreqs++;
        listAddNodeTail(&sockstate->wreqlist, areq);
    } else {
        errno = WSAGetLastError();
        FreeMemoryNoCOW(areq);
    }
    return SOCKET_ERROR;
}

/* For non-blocking connect with IOCP */
int WSIOCP_SocketConnect(int fd, const SOCKADDR_STORAGE *ss) {
    const GUID wsaid_connectex = WSAID_CONNECTEX;
    DWORD result;
    aeSockState *sockstate;

    if ((sockstate = WSIOCP_GetSocketState(fd)) == NULL) {
        errno = WSAEINVAL;
        return SOCKET_ERROR;
    }

    if (WSIOCP_SocketAttach(fd) != 0) {
        return SOCKET_ERROR;
    }

    memset(&sockstate->ov_read, 0, sizeof(sockstate->ov_read));
    
    // Need to bind sock before connectex
    switch (ss->ss_family) {
        case AF_INET:
        {
            SOCKADDR_IN addr;
            memset(&addr, 0, sizeof(SOCKADDR_IN));
            addr.sin_family = ss->ss_family;
            addr.sin_addr.S_un.S_addr = INADDR_ANY;
            addr.sin_port = 0;
            result = bind(fd, (SOCKADDR*)&addr, sizeof(addr));

            result = FDAPI_ConnectEx(fd, (SOCKADDR*)ss, sizeof(SOCKADDR_IN), NULL, 0, NULL, &sockstate->ov_read);
            break;
        }
        case AF_INET6:
        {
            SOCKADDR_IN6 addr;
            memset(&addr, 0, sizeof(SOCKADDR_IN6));
            addr.sin6_family = ss->ss_family;
            memset(&(addr.sin6_addr.u.Byte), 0, 16);
            addr.sin6_port = 0;
            result = bind(fd, (SOCKADDR*)&addr, sizeof(addr));

            result = FDAPI_ConnectEx(fd, (SOCKADDR*)ss, sizeof(SOCKADDR_IN6), NULL, 0, NULL, &sockstate->ov_read);
            break;
        }
        default:
        {
            ASSERT(ss->ss_family == AF_INET || ss->ss_family == AF_INET6);
            errno = WSAEINVAL;
            return SOCKET_ERROR;
        }
    }

    if (result != TRUE) {
        result = WSAGetLastError();
        if (result == ERROR_IO_PENDING) {
            errno = WSA_IO_PENDING;
            sockstate->masks |= CONNECT_PENDING;
        } else {
            errno = result;
            return SOCKET_ERROR;
        }
    }
    return 0;
}

int WSIOCP_SocketConnectBind(int fd, const SOCKADDR_STORAGE *ss, const char* source_addr) {
    const GUID wsaid_connectex = WSAID_CONNECTEX;
    DWORD result;
    aeSockState *sockstate;

    if ((sockstate = WSIOCP_GetSocketState(fd)) == NULL) {
        errno = WSAEINVAL;
        return SOCKET_ERROR;
    }

    if (WSIOCP_SocketAttach(fd) != 0) {
        return SOCKET_ERROR;
    }

    memset(&sockstate->ov_read, 0, sizeof(sockstate->ov_read));

    // Need to bind sock before connectex
    int storageSize = 0;
    switch (ss->ss_family) {
        case AF_INET:
        {
            storageSize = sizeof(SOCKADDR_IN);
            SOCKADDR_IN addr;
            memset(&addr, 0, storageSize);
            addr.sin_family = ss->ss_family;
            addr.sin_addr.S_un.S_addr = INADDR_ANY;
            addr.sin_port = 0;
            result = bind(fd, (SOCKADDR*)&addr, sizeof(addr));
            break;
        }
        case AF_INET6:
        {
            storageSize = sizeof(SOCKADDR_IN6);
            SOCKADDR_IN6 addr;
            memset(&addr, 0, storageSize);
            addr.sin6_family = ss->ss_family;
            memset(&(addr.sin6_addr.u.Byte), 0, 16);
            addr.sin6_port = 0;
            result = bind(fd, (SOCKADDR*)&addr, sizeof(addr));
            break;
        }
        default:
        {
            ASSERT(ss->ss_family == AF_INET || ss->ss_family == AF_INET6);
            errno = WSAEINVAL;
            return SOCKET_ERROR;
        }
    }

    result = FDAPI_ConnectEx(fd, (const LPSOCKADDR) ss, storageSize, NULL, 0, NULL, &sockstate->ov_read);
    if (result != TRUE) {
        result = WSAGetLastError();
        if (result == ERROR_IO_PENDING) {
            errno = WSA_IO_PENDING;
            sockstate->masks |= CONNECT_PENDING;
        } else {
            errno = result;
            return SOCKET_ERROR;
        }
    }
    return 0;
}

void WSIOCP_Init(HANDLE iocp) {
    iocph = iocp;
    FDAPI_SetCloseSocketState(WSIOCP_CloseSocketStateRFD);
}

void WSIOCP_Cleanup() {
    iocph = NULL;
}

static HANDLE privateheap;

void* CallocMemoryNoCOW(size_t size) {
    if (!privateheap) {
        privateheap = HeapCreate(HEAP_GENERATE_EXCEPTIONS | HEAP_NO_SERIALIZE, 0, 0);
    }
    return HeapAlloc(privateheap, HEAP_ZERO_MEMORY, size);
}

void FreeMemoryNoCOW(void * ptr) {
    HeapFree(privateheap, 0, ptr);
}