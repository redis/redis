/* ==========================================================================
 * rdma.c - Support RDMA protocol for transport layer. Instead of IB verbs
 *          low-level API, Use rsocket which is implemented by rdma-core to
 *          make this module easy to implement/maintain.
 * --------------------------------------------------------------------------
 * Copyright (C) 2021  zhenwei pi
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to permit
 * persons to whom the Software is furnished to do so, subject to the
 * following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
 * NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 * ==========================================================================
 */

#include "fmacros.h"

#include "async.h"
#include "async_private.h"
#include "hiredis.h"
#include "rdma.h"
#include <errno.h>

#define UNUSED(x) (void)(x)

void __redisSetError(redisContext *c, int type, const char *str);

#ifdef USE_RDMA
#ifdef __linux__    /* currently RDMA is only supported on Linux */
#define __USE_MISC
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <rdma/rsocket.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/types.h>

/* TODO: defined in net.c, should declear it as public function */
int redisContextTimeoutMsec(redisContext *c, long *result);
int redisContextUpdateConnectTimeout(redisContext *c, const struct timeval *timeout);

ssize_t redisRdmaRead(redisContext *c, char *buf, size_t bufcap) {
    ssize_t nread = rrecv(c->fd, buf, bufcap, 0);

    if (nread == -1) {
        if ((errno == EWOULDBLOCK && !(c->flags & REDIS_BLOCK)) || (errno == EINTR)) {
            return 0;
        } else {
            __redisSetError(c, REDIS_ERR_IO, "RDMA: recv failed");
            return REDIS_ERR;
        }
    } else if (nread == 0) {
        __redisSetError(c, REDIS_ERR_EOF, "RDMA: Server closed the connection");
        return REDIS_ERR;
    }

    return nread;
}

ssize_t redisRdmaWrite(redisContext *c) {
    ssize_t nwritten = rsend(c->fd, c->obuf, hi_sdslen(c->obuf), 0);

    if (nwritten < 0) {
        if ((errno == EWOULDBLOCK && !(c->flags & REDIS_BLOCK)) || (errno == EINTR)) {
             return 0;
        } else {
            __redisSetError(c, REDIS_ERR_IO, NULL);
            return REDIS_ERR;
        }
    }

    return nwritten;
}

void redisRdmaAsyncRead(redisAsyncContext *ac) {
    redisContext *c = &(ac->c);

    if (redisBufferRead(c) == REDIS_ERR) {
        __redisAsyncDisconnect(ac);
    } else {
        /* Always re-schedule reads */
        _EL_ADD_READ(ac);
        redisProcessCallbacks(ac);
    }
}

void redisRdmaAsyncWrite(redisAsyncContext *ac) {
    redisContext *c = &(ac->c);
    int done = 0;

    if (redisBufferWrite(c,&done) == REDIS_ERR) {
        __redisAsyncDisconnect(ac);
    } else {
        /* Continue writing when not done, stop writing otherwise */
        if (!done)
            _EL_ADD_WRITE(ac);
        else
            _EL_DEL_WRITE(ac);

        /* Always schedule reads after writes */
        _EL_ADD_READ(ac);
    }
}

static void redisRdmaClose(redisContext *c) {
    if (c && c->fd != REDIS_INVALID_FD) {
        rclose(c->fd);
        c->fd = REDIS_INVALID_FD;
    }
}

static void redisRdmaFree(void *privctx) {
    UNUSED(privctx);
}

redisContextFuncs redisContextRdmaFuncs = {
    .close = redisRdmaClose,
    .free_privctx = redisRdmaFree,
    .async_read = redisRdmaAsyncRead,
    .async_write = redisRdmaAsyncWrite,
    .read = redisRdmaRead,
    .write = redisRdmaWrite,
};

static int rsocketSetBlock(redisContext *c, int fd, int block) {
    int flags;

    if ((flags = rfcntl(fd, F_GETFL)) == -1) {
        __redisSetError(c, REDIS_ERR_OTHER, "RDMA: rfcntl failed");
        return REDIS_ERR;
    }

    if (block)
        flags &= ~O_NONBLOCK;
    else
        flags |= O_NONBLOCK;

    if (rfcntl(fd, F_SETFL, flags) == -1) {
        __redisSetError(c, REDIS_ERR_OTHER, "RDMA: rfcntl failed");
        return REDIS_ERR;
    }

    return REDIS_OK;
}

static int redisRdmaWait(int fd, int events, long long timeoutms) {
    struct pollfd pfd = {0};
    int retval;

    pfd.fd = fd;
    pfd.events = events;

    retval = rpoll(&pfd, 1, timeoutms);
    if (retval < 0) {
        return REDIS_ERR;
    }

    return pfd.revents;
}

static int _redisContextConnectBindRdma(redisContext *c, const char *addr,
                                       int port, const struct timeval *timeout,
                                       const char *source_addr) {
    struct addrinfo hints, *servinfo = NULL, *cliinfo = NULL, *psrv, *pcli;
    char buf[128];
    char _port[6];
    long timeoutms = -1;
    int blocking = (c->flags & REDIS_BLOCK);
    int ret;

    c->connection_type = REDIS_CONN_RDMA;
    c->funcs = &redisContextRdmaFuncs;
    c->tcp.port = port;
    c->fd = REDIS_INVALID_FD;

    if (c->tcp.host != addr) {
        hi_free(c->tcp.host);

        c->tcp.host = hi_strdup(addr);
        if (c->tcp.host == NULL) {
            __redisSetError(c, REDIS_ERR_OOM, "RDMA: Out of memory");
            return REDIS_ERR;
        }
    }

    if (source_addr == NULL) {
        hi_free(c->tcp.source_addr);
        c->tcp.source_addr = NULL;
    } else if (c->tcp.source_addr != source_addr) {
        hi_free(c->tcp.source_addr);
        c->tcp.source_addr = hi_strdup(source_addr);
    }

    if (timeout) {
        if (redisContextUpdateConnectTimeout(c, timeout) == REDIS_ERR) {
            __redisSetError(c, REDIS_ERR_OOM, "RDMA: Out of memory");
            return REDIS_ERR;
        }
    } else {
        hi_free(c->connect_timeout);
        c->connect_timeout = NULL;
    }

    if (redisContextTimeoutMsec(c, &timeoutms) != REDIS_OK) {
        __redisSetError(c, REDIS_ERR_IO, "RDMA: Invalid timeout specified");
        return REDIS_ERR;
    } else if (timeoutms == -1) {
        timeoutms = INT_MAX;
    }

    /* getaddrinfo for server side */
    snprintf(_port, 6, "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if ((ret = getaddrinfo(c->tcp.host, _port, &hints, &servinfo)) != 0) {
        hints.ai_family = AF_INET6;
        if ((ret = getaddrinfo(addr, _port, &hints, &servinfo)) != 0) {
            __redisSetError(c, REDIS_ERR_OTHER, gai_strerror(ret));
            goto err;
        }
    }

    /* getaddrinfo for client side */
    if (c->tcp.source_addr) {
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(c->tcp.source_addr, NULL, &hints, &cliinfo) != 0) {
            hints.ai_family = AF_INET6;
            if (getaddrinfo(c->tcp.source_addr, NULL, &hints, &servinfo) != 0) {
                __redisSetError(c, REDIS_ERR_OTHER, gai_strerror(ret));
                goto err;
            }
        }
    }

    for (psrv = servinfo; psrv != NULL; psrv = psrv->ai_next) {
        c->fd = rsocket(psrv->ai_family, psrv->ai_socktype, psrv->ai_protocol);
        if (c->fd == REDIS_INVALID_FD) {
            continue;
        }

        for (pcli = cliinfo; pcli != NULL; pcli = pcli->ai_next) {
            if (rbind(c->fd, pcli->ai_addr, pcli->ai_addrlen) != -1) {
                break;
            }
        }

        if (rsocketSetBlock(c, c->fd, 0) != REDIS_OK) {
            goto err;
        }

        hi_free(c->saddr);
        c->addrlen = psrv->ai_addrlen;
        c->saddr = hi_malloc(psrv->ai_addrlen);
        memcpy(c->saddr, psrv->ai_addr, psrv->ai_addrlen);
        ret = rconnect(c->fd, psrv->ai_addr, psrv->ai_addrlen);
        if (ret == -1) {
            if (errno != EINPROGRESS) {
                snprintf(buf, sizeof(buf), "RDMA: rconnect failed: %s", strerror(errno));
                __redisSetError(c, REDIS_ERR_OTHER, buf);
	        goto err;
            }

            if (redisRdmaWait(c->fd, POLLOUT, timeoutms) == REDIS_ERR) {
	        goto err;
            }
        }

        if (blocking && rsocketSetBlock(c, c->fd, 1) != REDIS_OK) {
            goto err;
        }

        break;
    }

    c->flags |= REDIS_CONNECTED;
    ret = REDIS_OK;
    goto out;

err:
    ret = REDIS_ERR;
    if (c->fd != REDIS_INVALID_FD) {
        rclose(c->fd);
        c->fd = REDIS_INVALID_FD;
    }

out:
    if(servinfo) {
        freeaddrinfo(servinfo);
    }

    if(cliinfo) {
        freeaddrinfo(cliinfo);
    }

    return ret;
}

int redisContextConnectBindRdma(redisContext *c, const char *addr, int port,
                                const struct timeval *timeout,
                                const char *source_addr) {
    return _redisContextConnectBindRdma(c, addr, port, timeout, source_addr);
}
#else    /* __linux__ */

"BUILD ERROR: RDMA is only supported on linux"

#endif   /* __linux__ */
#else    /* USE_RDMA */

int redisContextConnectBindRdma(redisContext *c, const char *addr, int port,
                                const struct timeval *timeout,
                                const char *source_addr) {
    UNUSED(c);
    UNUSED(addr);
    UNUSED(port);
    UNUSED(timeout);
    UNUSED(source_addr);
    __redisSetError(c, REDIS_ERR_PROTOCOL, "RDMA: disabled, please rebuild with BUILD_RDMA");
    return -EPROTONOSUPPORT;
}

#endif
