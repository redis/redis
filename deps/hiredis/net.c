/* Extracted from anet.c to work properly with Hiredis error reporting.
 *
 * Copyright (c) 2006-2011, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2010-2011, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "fmacros.h"
#include <sys/types.h>
#ifdef _WIN32
  #ifndef FD_SETSIZE
    #define FD_SETSIZE 16000
  #endif
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#endif
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#ifndef _WIN32
#include <poll.h>
#endif
#include <limits.h>

#include "net.h"
#include "sds.h"
#ifdef _WIN32
  #include "../../src/win32fixes.h"
#endif

/* Defined in hiredis.c */
void __redisSetError(redisContext *c, int type, const char *str);

static void __redisSetErrorFromErrno(redisContext *c, int type, const char *prefix) {
    char buf[128];
    size_t len = 0;

    if (prefix != NULL)
        len = snprintf(buf,sizeof(buf),"%s: ",prefix);
    strerror_r(errno,buf+len,sizeof(buf)-len);
    __redisSetError(c,type,buf);
}

static int redisSetReuseAddr(redisContext *c, int fd) {
    int on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) == -1) {
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,NULL);
        close(fd);
        return REDIS_ERR;
    }
    return REDIS_OK;
}

#ifdef _WIN32
static int redisCreateSocket(redisContext *c, int type) {
    SOCKET s;
    int on=1;

    s = socket(type, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        __redisSetError(c,REDIS_ERR_IO,sdscatprintf(sdsempty(), "socket error: %d\n", WSAGetLastError()));
        return REDIS_ERR;
    }
    if (type == AF_INET) {
        LINGER l;
        l.l_onoff = 1;
        l.l_linger = 2;
        setsockopt((int)s, SOL_SOCKET, SO_LINGER, (const char *) &l, sizeof(l));

        if (setsockopt((int)s, SOL_SOCKET, SO_REUSEADDR, (const char *) &on, sizeof(on)) == -1) {
            __redisSetError(c,REDIS_ERR_IO,NULL);
            closesocket(s);
            return REDIS_ERR;
        }
    }
    return (int)s;
}
#else
static int redisCreateSocket(redisContext *c, int type) {
    int s;
    if ((s = socket(type, SOCK_STREAM, 0)) == -1) {
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,NULL);
        return REDIS_ERR;
    }
    if (type == AF_INET) {
        if (redisSetReuseAddr(c,s) == REDIS_ERR) {
            return REDIS_ERR;
        }
    }
    return s;
}
}
#endif


#ifdef _WIN32
static int redisSetBlocking(redisContext *c, int fd, int blocking) {
    /* If iMode = 0, blocking is enabled; */
    /* If iMode != 0, non-blocking mode is enabled. */
    u_long flags;

    if (blocking)
        flags = (u_long)0;
    else
        flags = (u_long)1;

    if (ioctlsocket((SOCKET)fd, FIONBIO, &flags) == SOCKET_ERROR) {
        errno = WSAGetLastError();
        __redisSetError(c,REDIS_ERR_IO,
            sdscatprintf(sdsempty(), "ioctlsocket(FIONBIO): %d\n", errno));
        closesocket(fd);
        return REDIS_ERR;
    };

    return REDIS_OK;
}

#else
static int redisSetBlocking(redisContext *c, int fd, int blocking) {
    int flags;

    /* Set the socket nonblocking.
     * Note that fcntl(2) for F_GETFL and F_SETFL can't be
     * interrupted by a signal. */
    if ((flags = fcntl(fd, F_GETFL)) == -1) {
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,"fcntl(F_GETFL)");
        close(fd);
        return REDIS_ERR;
    }

    if (blocking)
        flags &= ~O_NONBLOCK;
    else
        flags |= O_NONBLOCK;

    if (fcntl(fd, F_SETFL, flags) == -1) {
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,"fcntl(F_SETFL)");
        close(fd);
        return REDIS_ERR;
    }
    return REDIS_OK;
}
#endif

#ifdef _WIN32
static int redisSetTcpNoDelay(redisContext *c, int fd) {
    int yes = 1;
    if (setsockopt((SOCKET)fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&yes, sizeof(yes)) == -1) {
        __redisSetError(c,REDIS_ERR_IO,
            sdscatprintf(sdsempty(), "setsockopt(TCP_NODELAY): %d", (int)GetLastError()));
        closesocket(fd);
        return REDIS_ERR;
    }
    return REDIS_OK;
}
#else
static int redisSetTcpNoDelay(redisContext *c, int fd) {
    int yes = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) == -1) {
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,"setsockopt(TCP_NODELAY)");
        close(fd);
        return REDIS_ERR;
    }
    return REDIS_OK;
}
#endif

#ifdef _WIN32
static int redisContextWaitReady(redisContext *c, int fd, const struct timeval *timeout) {
    struct timeval to;
    struct timeval *toptr = NULL;
    fd_set wfd;

    /* Only use timeout when not NULL. */
    if (timeout != NULL) {
        to = *timeout;
        toptr = &to;
    }

    if (errno == EINPROGRESS) {
        FD_ZERO(&wfd);
        FD_SET((SOCKET)fd, &wfd);

        if (select(FD_SETSIZE, NULL, &wfd, NULL, toptr) == -1) {
            __redisSetErrorFromErrno(c,REDIS_ERR_IO,"select(2)");
            closesocket(fd);
            return REDIS_ERR;
        }

        if (!FD_ISSET(fd, &wfd)) {
            errno = WSAGetLastError();
            __redisSetErrorFromErrno(c,REDIS_ERR_IO,NULL);
            closesocket(fd);
            return REDIS_ERR;
        }

        if (redisCheckSocketError(c, fd) != REDIS_OK)
            return REDIS_ERR;

        return REDIS_OK;
    }

    __redisSetErrorFromErrno(c,REDIS_ERR_IO,NULL);
    closesocket(fd);
    return REDIS_ERR;
}
#else
#define __MAX_MSEC (((LONG_MAX) - 999) / 1000)

static int redisContextWaitReady(redisContext *c, int fd, const struct timeval *timeout) {
    struct pollfd   wfd[1];
    long msec;

    msec          = -1;
    wfd[0].fd     = fd;
    wfd[0].events = POLLOUT;

    /* Only use timeout when not NULL. */
    if (timeout != NULL) {
        if (timeout->tv_usec > 1000000 || timeout->tv_sec > __MAX_MSEC) {
            close(fd);
            return REDIS_ERR;
        }

        msec = (timeout->tv_sec * 1000) + ((timeout->tv_usec + 999) / 1000);

        if (msec < 0 || msec > INT_MAX) {
            msec = INT_MAX;
        }
    }

    if (errno == EINPROGRESS) {
        int res;

        if ((res = poll(wfd, 1, msec)) == -1) {
            __redisSetErrorFromErrno(c, REDIS_ERR_IO, "poll(2)");
            close(fd);
            return REDIS_ERR;
        } else if (res == 0) {
            errno = ETIMEDOUT;
            __redisSetErrorFromErrno(c,REDIS_ERR_IO,NULL);
            close(fd);
            return REDIS_ERR;
        }

        if (redisCheckSocketError(c, fd) != REDIS_OK)
            return REDIS_ERR;

        return REDIS_OK;
    }

    __redisSetErrorFromErrno(c,REDIS_ERR_IO,NULL);
    close(fd);
    return REDIS_ERR;
}
#endif

int redisCheckSocketError(redisContext *c, int fd) {
    int err = 0;
    socklen_t errlen = sizeof(err);

#ifdef _WIN32
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&err, &errlen) == SOCKET_ERROR) {
        errno = WSAGetLastError();
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,"getsockopt(SO_ERROR)");
        closesocket(fd);
        return REDIS_ERR;
    }
#else
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen) == -1) {
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,"getsockopt(SO_ERROR)");
        close(fd);
        return REDIS_ERR;
    }
#endif

    if (err) {
        errno = err;
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,NULL);
#ifdef _WIN32
        closesocket(fd);
#else
        close(fd);
#endif
        return REDIS_ERR;
    }

    return REDIS_OK;
}


#ifdef _WIN32
int redisContextSetTimeout(redisContext *c, struct timeval tv) {
    DWORD ms = (tv.tv_sec * 1000) + (tv.tv_usec / 1000);
    if (setsockopt((int)c->fd,SOL_SOCKET,SO_RCVTIMEO,(const char *)&ms,sizeof(ms)) == SOCKET_ERROR ) {
        errno = WSAGetLastError();
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,"setsockopt(SO_RCVTIMEO)");
        return REDIS_ERR;
    }
    if (setsockopt((int)c->fd,SOL_SOCKET,SO_SNDTIMEO,(const char *)&ms,sizeof(ms)) == SOCKET_ERROR ) {
        errno = WSAGetLastError();
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,"setsockopt(SO_SNDTIMEO)");
        return REDIS_ERR;
    }
    return REDIS_OK;
}
#else
int redisContextSetTimeout(redisContext *c, struct timeval tv) {
    if (setsockopt(c->fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv)) == -1) {
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,"setsockopt(SO_RCVTIMEO)");
        return REDIS_ERR;
    }
    if (setsockopt(c->fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv)) == -1) {
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,"setsockopt(SO_SNDTIMEO)");
        return REDIS_ERR;
    }
    return REDIS_OK;
}
#endif

#ifdef _WIN32
int redisContextPreConnectTcp(redisContext *c, const char *addr, int port,
                            struct timeval *timeout, struct sockaddr_in *sa) {
    int s;
    int blocking = (c->flags & REDIS_BLOCK);
    unsigned long inAddress;

    if ((s = redisCreateSocket(c,AF_INET)) < 0)
        return REDIS_ERR;

    sa->sin_family = AF_INET;
    sa->sin_port = htons(port);

    inAddress = inet_addr(addr);
    if (inAddress == INADDR_NONE || inAddress == INADDR_ANY) {
        struct hostent *he;

        he = gethostbyname(addr);
        if (he == NULL) {
            __redisSetError(c,REDIS_ERR_OTHER,
                sdscatprintf(sdsempty(),"can't resolve: %s\n", addr));
            closesocket(s);
            return REDIS_ERR;
        }
        memcpy(&sa->sin_addr, he->h_addr, sizeof(struct in_addr));
    }
    else {
        sa->sin_addr.s_addr = inAddress;
    }

    if (redisSetTcpNoDelay(c,s) != REDIS_OK)
        return REDIS_ERR;

    if (blocking ==  0) {
        if (redisSetBlocking(c,s,0) != REDIS_OK)
            return REDIS_ERR;
    }

    c->fd = s;
    return REDIS_OK;
}

int redisContextConnectTcp(redisContext *c, const char *addr, int port, struct timeval *timeout) {
    int s;
    int blocking = (c->flags & REDIS_BLOCK);
    struct sockaddr_in sa;
    unsigned long inAddress;

    if ((s = redisCreateSocket(c,AF_INET)) < 0)
        return REDIS_ERR;

    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);

    inAddress = inet_addr(addr);
    if (inAddress == INADDR_NONE || inAddress == INADDR_ANY) {
        struct hostent *he;

        he = gethostbyname(addr);
        if (he == NULL) {
            __redisSetError(c,REDIS_ERR_OTHER,
                sdscatprintf(sdsempty(),"can't resolve: %s\n", addr));
            closesocket(s);
            return REDIS_ERR;
        }
        memcpy(&sa.sin_addr, he->h_addr, sizeof(struct in_addr));
    }
    else {
        sa.sin_addr.s_addr = inAddress;
    }

    if (redisSetTcpNoDelay(c,s) != REDIS_OK)
        return REDIS_ERR;

    if (blocking ==  0) {
        if (redisSetBlocking(c,s,0) != REDIS_OK)
            return REDIS_ERR;
    }
    if (connect((SOCKET)s, (struct sockaddr*)&sa, sizeof(sa)) == -1) {
        errno = WSAGetLastError();
        if ((errno == WSAEINVAL) || (errno == WSAEWOULDBLOCK))
            errno = EINPROGRESS;
        if (errno == EINPROGRESS && !blocking) {
            /* This is ok. */
        } else {
            if (redisContextWaitReady(c,s,timeout) != REDIS_OK)
                return REDIS_ERR;
        }
    }

    if (blocking) {
        if (redisSetBlocking(c,s,1) != REDIS_OK)
            return REDIS_ERR;
    }

    c->fd = s;
    c->flags |= REDIS_CONNECTED;
    return REDIS_OK;
}
#else
int redisContextConnectTcp(redisContext *c, const char *addr, int port, struct timeval *timeout) {
    int s, rv;
    char _port[6];  /* strlen("65535"); */
    struct addrinfo hints, *servinfo, *p;
    int blocking = (c->flags & REDIS_BLOCK);

    snprintf(_port, 6, "%d", port);
    memset(&hints,0,sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if ((rv = getaddrinfo(addr,_port,&hints,&servinfo)) != 0) {
        __redisSetError(c,REDIS_ERR_OTHER,gai_strerror(rv));
        return REDIS_ERR;
    }
    for (p = servinfo; p != NULL; p = p->ai_next) {
        if ((s = socket(p->ai_family,p->ai_socktype,p->ai_protocol)) == -1)
            continue;

        if (redisSetBlocking(c,s,0) != REDIS_OK)
            goto error;
        if (connect(s,p->ai_addr,p->ai_addrlen) == -1) {
            if (errno == EHOSTUNREACH) {
                close(s);
                continue;
            } else if (errno == EINPROGRESS && !blocking) {
                /* This is ok. */
            } else {
                if (redisContextWaitReady(c,s,timeout) != REDIS_OK)
                    goto error;
            }
        }
        if (blocking && redisSetBlocking(c,s,1) != REDIS_OK)
            goto error;
        if (redisSetTcpNoDelay(c,s) != REDIS_OK)
            goto error;

        c->fd = s;
        c->flags |= REDIS_CONNECTED;
        rv = REDIS_OK;
        goto end;
    }
    if (p == NULL) {
        char buf[128];
        snprintf(buf,sizeof(buf),"Can't create socket: %s",strerror(errno));
        __redisSetError(c,REDIS_ERR_OTHER,buf);
        goto error;
    }

error:
    rv = REDIS_ERR;
end:
    freeaddrinfo(servinfo);
    return rv;  // Need to return REDIS_OK if alright
}
#endif

#ifdef _WIN32
int redisContextConnectUnix(redisContext *c, const char *path, struct timeval *timeout) {
    (void) timeout;
    __redisSetError(c,REDIS_ERR_IO,
        sdscatprintf(sdsempty(),"Unix sockets are not suported on Windows platform. (%s)\n", path));

    return REDIS_ERR;
}
#else
int redisContextConnectUnix(redisContext *c, const char *path, struct timeval *timeout) {
    int s;
    int blocking = (c->flags & REDIS_BLOCK);
    struct sockaddr_un sa;

    if ((s = redisCreateSocket(c,AF_LOCAL)) < 0)
        return REDIS_ERR;
    if (redisSetBlocking(c,s,0) != REDIS_OK)
        return REDIS_ERR;

    sa.sun_family = AF_LOCAL;
    strncpy(sa.sun_path,path,sizeof(sa.sun_path)-1);
    if (connect(s, (struct sockaddr*)&sa, sizeof(sa)) == -1) {
        if (errno == EINPROGRESS && !blocking) {
            /* This is ok. */
        } else {
            if (redisContextWaitReady(c,s,timeout) != REDIS_OK)
                return REDIS_ERR;
        }
    }

    /* Reset socket to be blocking after connect(2). */
    if (blocking && redisSetBlocking(c,s,1) != REDIS_OK)
        return REDIS_ERR;

    c->fd = s;
    c->flags |= REDIS_CONNECTED;
    return REDIS_OK;
}
#endif
