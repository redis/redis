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
#include "..\zmalloc.h"
#include <mswsock.h>
#include <Guiddef.h>
#include "win32_wsiocp.h"
#include "Win32_FDAPI.h"
#include <errno.h>


static void *iocpState;
static HANDLE iocph;
static fnGetSockState * aeGetSockState;
static fnDelSockState * aeDelSockState;

#define SUCCEEDED_WITH_IOCP(result)                        \
  ((result) || (GetLastError() == ERROR_IO_PENDING))

/* for zero length reads use shared buf */
static DWORD wsarecvflags;
static char zreadchar[1];


int aeWinQueueAccept(int listenfd) {
    aeSockState *sockstate;
    aeSockState *accsockstate;
    DWORD result, bytes;
    int acceptfd;
    aacceptreq * areq;

    if ((sockstate = aeGetSockState(iocpState, listenfd)) == NULL) {
        errno = WSAEINVAL;
        return -1;
    }

    acceptfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (acceptfd == -1) {
        errno = WSAEINVAL;
        return -1;
    }

    accsockstate = aeGetSockState(iocpState, acceptfd);
    if (accsockstate == NULL) {
        errno = WSAEINVAL;
        return -1;
    }

    accsockstate->masks = SOCKET_ATTACHED;
    /* keep accept socket in buf len until accepted */
    areq = (aacceptreq *)zmalloc(sizeof(aacceptreq));
    memset(areq, 0, sizeof(aacceptreq));
    areq->buf = (char *)zmalloc(sizeof(struct sockaddr_storage) * 2 + 64);
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
        close(acceptfd);
        accsockstate->masks = 0;
        zfree(areq);
        return -1;
    }

    return TRUE;
}

/* listen using extension function to get faster accepts */
int aeWinListen(int rfd, int backlog) {
    aeSockState *sockstate;
    const GUID wsaid_acceptex = WSAID_ACCEPTEX;
    const GUID wsaid_acceptexaddrs = WSAID_GETACCEPTEXSOCKADDRS;

    if ((sockstate = aeGetSockState(iocpState, rfd)) == NULL) {
        errno = WSAEINVAL;
        return SOCKET_ERROR;
    }

    aeWinSocketAttach(rfd);
    sockstate->masks |= LISTEN_SOCK;

    if (listen(rfd, backlog) == 0) {
        if (aeWinQueueAccept(rfd) == -1) {
            errno = WSAGetLastError();
            return SOCKET_ERROR;
        }
    } else {
        errno = WSAGetLastError();
    }

    return 0;
}

/* return the queued accept socket */
int aeWinAccept(int fd, struct sockaddr *sa, socklen_t *len) {
    aeSockState *sockstate;
    int acceptsock;
    int result;
    SOCKADDR *plocalsa;
    SOCKADDR *premotesa;
    int locallen, remotelen;
    aacceptreq * areq;

    if ((sockstate = aeGetSockState(iocpState, fd)) == NULL) {
        errno = WSAEINVAL;
        return SOCKET_ERROR;
    }


    areq = sockstate->reqs;
    if (areq == NULL) {
        errno = EWOULDBLOCK;
        return SOCKET_ERROR;
    }

    sockstate->reqs = areq->next;

    acceptsock = (int)areq->accept;

    result = FDAPI_UpdateAcceptContext(acceptsock);
    if (result == SOCKET_ERROR) {
        errno = WSAGetLastError();
        return SOCKET_ERROR;
    }

    locallen = *len;
    FDAPI_GetAcceptExSockaddrs(
                    acceptsock,
                    areq->buf,
                    0,
                    sizeof(struct sockaddr_storage),
                    sizeof(struct sockaddr_storage),
                    &plocalsa, &locallen,
                    &premotesa, &remotelen);

    locallen = remotelen < *len ? remotelen : *len;
    memcpy(sa, premotesa, locallen);
    *len = locallen;

    aeWinSocketAttach(acceptsock);

    zfree(areq->buf);
    zfree(areq);

    /* queue another accept */
    if (aeWinQueueAccept(fd) == -1) {
        return SOCKET_ERROR;
    }

    return acceptsock;
}


/* after doing read caller needs to call done
 * so that we can continue to check for read events.
 * This is not necessary if caller will delete read events */
int aeWinReceiveDone(int fd) {
    aeSockState *sockstate;
    int result;
    WSABUF zreadbuf;
    DWORD bytesReceived = 0;

    if ((sockstate = aeGetSockState(iocpState, fd)) == NULL) {
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
    result = WSARecv(fd,
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

/* wrapper for send
 * enables use of WSA Send to get IOCP notification of completion.
 * returns -1  with errno = WSA_IO_PENDING if callback will be invoked later */
int aeWinSocketSend(int fd, char *buf, int len, 
                    void *eventLoop, void *client, void *data, void *proc) {
    aeSockState *sockstate;
    int result;
    asendreq *areq;
    DWORD bytesSent = 0;

    sockstate = aeGetSockState(iocpState, fd);

    if (sockstate != NULL &&
        (sockstate->masks & CONNECT_PENDING)) {
        aeWait(fd, AE_WRITABLE, 50);
    }

    /* if not an async socket, do normal send */
    if (sockstate == NULL ||
        (sockstate->masks & SOCKET_ATTACHED) == 0 ||
        proc == NULL) {
        result = write(fd, buf, len);
        if (result == SOCKET_ERROR) {
            errno = WSAGetLastError();
        }
        return result;
    }

    /* use overlapped structure to send using IOCP */
    areq = (asendreq *)zmalloc(sizeof(asendreq));
    memset(areq, 0, sizeof(asendreq));
    areq->wbuf.len = len;
    areq->wbuf.buf = buf;
    areq->eventLoop = (aeEventLoop *)eventLoop;
    areq->req.client = client;
    areq->req.data = data;
    areq->req.len = len;
    areq->req.buf = buf;
    areq->proc = (aeFileProc *)proc;

    result = WSASend(fd,
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
        zfree(areq);
    }
    return SOCKET_ERROR;
}




/* for non-blocking connect with IOCP */
int aeWinSocketConnect(int fd, const SOCKADDR_STORAGE *ss) {
    const GUID wsaid_connectex = WSAID_CONNECTEX;
    DWORD result;
    aeSockState *sockstate;

    if ((sockstate = aeGetSockState(iocpState, fd)) == NULL) {
        errno = WSAEINVAL;
        return SOCKET_ERROR;
    }

    if (aeWinSocketAttach(fd) != 0) {
        return SOCKET_ERROR;
    }

    memset(&sockstate->ov_read, 0, sizeof(sockstate->ov_read));
    
    /* need to bind sock before connectex */
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
            DebugBreak();
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

int aeWinSocketConnectBind(int fd, const SOCKADDR_STORAGE *ss, const char* source_addr) {
    const GUID wsaid_connectex = WSAID_CONNECTEX;
    DWORD result;
    aeSockState *sockstate;

    if ((sockstate = aeGetSockState(iocpState, fd)) == NULL) {
        errno = WSAEINVAL;
        return SOCKET_ERROR;
    }

    if (aeWinSocketAttach(fd) != 0) {
        return SOCKET_ERROR;
    }

    memset(&sockstate->ov_read, 0, sizeof(sockstate->ov_read));

    /* need to bind sock before connectex */
    switch (ss->ss_family) {
        case AF_INET:
        {
            SOCKADDR_IN addr;
            memset(&addr, 0, sizeof(SOCKADDR_IN));
            addr.sin_family = ss->ss_family;
            addr.sin_addr.S_un.S_addr = INADDR_ANY;
            addr.sin_port = 0;
            result = bind(fd, (SOCKADDR*)&addr, sizeof(addr));
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
            break;
        }
        default:
        {
                   DebugBreak();
        }
    }

    result = FDAPI_ConnectEx(fd, (const LPSOCKADDR)ss, StorageSize(ss), NULL, 0, NULL, &sockstate->ov_read);
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

/* for each asynch socket, need to associate completion port */
int aeWinSocketAttach(int fd) {
    DWORD yes = 1;
    aeSockState *sockstate;

    if ((sockstate = aeGetSockState(iocpState, fd)) == NULL) {
        errno = WSAEINVAL;
        return -1;
    }

    /* Set the socket to nonblocking mode */
    if (ioctlsocket(fd, FIONBIO, &yes) == SOCKET_ERROR) {
        errno = WSAGetLastError();
        return -1;
    }

    /* Make the socket non-inheritable */
    if (!SetFDInformation(fd, HANDLE_FLAG_INHERIT, 0)) {
        errno = WSAGetLastError();
        return -1;
    }

    /* Associate it with the I/O completion port. */
    /* Use FD as completion key. */
    if (FDAPI_CreateIoCompletionPortOnFD(fd,
                                         iocph,
                                         (ULONG_PTR)fd,
                                         0) == NULL) {
        errno = WSAGetLastError();
        return -1;
    }
    sockstate->masks = SOCKET_ATTACHED;
    sockstate->wreqs = 0;

    return 0;
}

void aeShutdown(int fd) {
    char rbuf[100];
    long long waitmsecs = 50;      /* wait up to 50 millisecs */
    long long endms;
    long long nowms;

    /* wait for last item to complete up to tosecs seconds*/
    endms = GetHighResRelativeTime(1000) + waitmsecs;

    if (shutdown(fd, SD_SEND) != SOCKET_ERROR) {
        /* read data until no more or error to ensure shutdown completed */
        while (1) {
            int rc = read(fd, rbuf, 100);
            if (rc == 0 || rc == SOCKET_ERROR)
                break;
            else {
                nowms = GetHighResRelativeTime(1000);
                if (nowms > endms)
                    break;
            }
        }
    }
}

/* when closing socket, need to unassociate completion port */
int aeWinCloseSocket(int fd) {
    aeSockState *sockstate;
    BOOL closed = FALSE;

    if ((sockstate = aeGetSockState(iocpState, fd)) == NULL) {
        close(fd);
        return 0;
    }

    aeShutdown(fd);
    sockstate->masks &= ~(SOCKET_ATTACHED | AE_WRITABLE | AE_READABLE);

    if (sockstate->wreqs == 0 &&
        (sockstate->masks & (READ_QUEUED | CONNECT_PENDING | SOCKET_ATTACHED)) == 0) {
        close(fd);
        closed = TRUE;
    } else {
        sockstate->masks |= CLOSE_PENDING;
    }
    aeDelSockState(iocpState, sockstate);

    return 0;
}

void aeWinInit(void *state, HANDLE iocp, fnGetSockState *getSockState,
                                        fnDelSockState *delSockState) {
    iocpState = state;
    iocph = iocp;
    aeGetSockState = getSockState;
    aeDelSockState = delSockState;
}

void aeWinCleanup() {
    iocpState = NULL;
}
