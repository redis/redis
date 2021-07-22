/* RSOCKET(7)-implemented by rdma-core, based ae.c module
 *
 * Support RDMA protocol for transport layer. Instead of IB verbs
 * low-level API, Use rsocket which is implemented by rdma-core to
 * make this module easy to implement/maintain.
 *
 * Copyright (C) 2021 zhenwei pi
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


#include <sys/time.h>
#include <rdma/rsocket.h>

typedef struct aeApiState {
    struct pollfd *pfds;
    struct pollfd *polling;
    int nfds;
} aeApiState;

static int aeApiCreate(aeEventLoop *eventLoop) {
    aeApiState *state;
    struct pollfd *pfd;
    int i;

    state = zmalloc(sizeof(aeApiState));
    state->nfds = eventLoop->setsize;
    state->pfds = zmalloc(sizeof(struct pollfd) * eventLoop->setsize);
    state->polling = zmalloc(sizeof(struct pollfd) * eventLoop->setsize);

    for (i = 0; i < eventLoop->setsize; i++) {
        pfd = state->pfds + i;
        pfd->fd = -1;
        pfd->events = 0;
    }

    eventLoop->apidata = state;
    return 0;
}

static int aeApiResize(aeEventLoop *eventLoop, int setsize) {
    aeApiState *state = eventLoop->apidata;
    struct pollfd *pfd;
    int i;

    state->pfds = zrealloc(state->pfds, sizeof(struct pollfd) * setsize);
    state->polling = zrealloc(state->polling, sizeof(struct pollfd) * setsize);

    /* mark the new pollfd(s) fd as -1 */
    if (setsize > state->nfds) {
        for (i = state->nfds; i < setsize; i++) {
            pfd = state->pfds + i;
            pfd->fd = -1;
            pfd->events = 0;
        }
    }

    state->nfds = setsize;

    return 0;
}

static void aeApiFree(aeEventLoop *eventLoop) {
    aeApiState *state = eventLoop->apidata;

    zfree(state->pfds);
    zfree(state->polling);
    zfree(state);
}

static int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;
    struct pollfd *pfd;

    if (fd >= state->nfds) {
        return -1;
    }

    pfd = state->pfds + fd;
    pfd->fd = fd;
    pfd->revents = 0;
    if (mask & AE_READABLE) {
        pfd->events |= POLLIN;
    }

    if (mask & AE_WRITABLE) {
        pfd->events |= POLLOUT;
    }

    pfd->events |= (POLLERR | POLLHUP);

    return 0;
}

static void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;
    struct pollfd *pfd;

    if (fd >= state->nfds) {
        return;
    }

    pfd = state->pfds + fd;
    pfd->fd = fd;
    pfd->revents = 0;
    if (mask & AE_READABLE) {
        pfd->events &= ~POLLIN;
    }

    if (mask & AE_WRITABLE) {
        pfd->events &= ~POLLOUT;
    }

    /* no POLLIN/POLLOUT event should poll, set fd as invalid */
    if (!(pfd->events & (POLLIN | POLLOUT))) {
        pfd->fd = -1;
    }
}

static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp) {
    aeApiState *state = eventLoop->apidata;
    struct pollfd *pfd, *polling;
    aeFiredEvent *fired;
    int nevents, timeoutms, nfds = 0;
    int i, fires, mask;

    for (i = 0; i < state->nfds; i++) {
        pfd = state->pfds + i;
        if (pfd->fd < 0 || !pfd->events) {
            continue;
        }

        polling = state->polling + nfds;
        *polling = *pfd;
        nfds++;
    }

    timeoutms = tvp ? (tvp->tv_sec * 1000 + (tvp->tv_usec + 999) / 1000) : -1;
    nevents = rpoll(state->polling, nfds, timeoutms);
    if (nevents == -1) {
        if ( errno == EINTR) {
            return 0;
        }

        panic("aeApiPoll: Fatal error rpoll: %s", strerror(errno));
    }

    for (fires = 0, i = 0; i < nfds; i++) {
        polling = state->polling + i;

        if (!polling->revents) {
            continue;
        }

        mask = 0;
        if (polling->revents & POLLIN)
            mask |= AE_READABLE;
        if (polling->revents & POLLOUT)
            mask |= AE_WRITABLE;
        if (polling->revents & POLLERR)
            mask |= AE_WRITABLE | AE_READABLE;
        if (polling->revents & POLLHUP)
            mask |= AE_WRITABLE | AE_READABLE;

        fired = eventLoop->fired + fires;
        fired->fd = polling->fd;
        fired->mask = mask;
        fires++;
    }

    return nevents;
}

static char *aeApiName(void) {
    return "rpoll";
}
