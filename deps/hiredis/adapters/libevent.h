/*
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

#ifndef __HIREDIS_LIBEVENT_H__
#define __HIREDIS_LIBEVENT_H__
#include <event2/event.h>
#include "../hiredis.h"
#include "../async.h"

#define REDIS_LIBEVENT_DELETED 0x01
#define REDIS_LIBEVENT_ENTERED 0x02

typedef struct redisLibeventEvents {
    redisAsyncContext *context;
    struct event *ev;
    struct event_base *base;
    struct timeval tv;
    short flags;
    short state;
} redisLibeventEvents;

static void redisLibeventDestroy(redisLibeventEvents *e) {
    hi_free(e);
}

static void redisLibeventHandler(evutil_socket_t fd, short event, void *arg) {
    ((void)fd);
    redisLibeventEvents *e = (redisLibeventEvents*)arg;
    e->state |= REDIS_LIBEVENT_ENTERED;

    #define CHECK_DELETED() if (e->state & REDIS_LIBEVENT_DELETED) {\
        redisLibeventDestroy(e);\
        return; \
    }

    if ((event & EV_TIMEOUT) && (e->state & REDIS_LIBEVENT_DELETED) == 0) {
        redisAsyncHandleTimeout(e->context);
        CHECK_DELETED();
    }

    if ((event & EV_READ) && e->context && (e->state & REDIS_LIBEVENT_DELETED) == 0) {
        redisAsyncHandleRead(e->context);
        CHECK_DELETED();
    }

    if ((event & EV_WRITE) && e->context && (e->state & REDIS_LIBEVENT_DELETED) == 0) {
        redisAsyncHandleWrite(e->context);
        CHECK_DELETED();
    }

    e->state &= ~REDIS_LIBEVENT_ENTERED;
    #undef CHECK_DELETED
}

static void redisLibeventUpdate(void *privdata, short flag, int isRemove) {
    redisLibeventEvents *e = (redisLibeventEvents *)privdata;
    const struct timeval *tv = e->tv.tv_sec || e->tv.tv_usec ? &e->tv : NULL;

    if (isRemove) {
        if ((e->flags & flag) == 0) {
            return;
        } else {
            e->flags &= ~flag;
        }
    } else {
        if (e->flags & flag) {
            return;
        } else {
            e->flags |= flag;
        }
    }

    event_del(e->ev);
    event_assign(e->ev, e->base, e->context->c.fd, e->flags | EV_PERSIST,
                 redisLibeventHandler, privdata);
    event_add(e->ev, tv);
}

static void redisLibeventAddRead(void *privdata) {
    redisLibeventUpdate(privdata, EV_READ, 0);
}

static void redisLibeventDelRead(void *privdata) {
    redisLibeventUpdate(privdata, EV_READ, 1);
}

static void redisLibeventAddWrite(void *privdata) {
    redisLibeventUpdate(privdata, EV_WRITE, 0);
}

static void redisLibeventDelWrite(void *privdata) {
    redisLibeventUpdate(privdata, EV_WRITE, 1);
}

static void redisLibeventCleanup(void *privdata) {
    redisLibeventEvents *e = (redisLibeventEvents*)privdata;
    if (!e) {
        return;
    }
    event_del(e->ev);
    event_free(e->ev);
    e->ev = NULL;

    if (e->state & REDIS_LIBEVENT_ENTERED) {
        e->state |= REDIS_LIBEVENT_DELETED;
    } else {
        redisLibeventDestroy(e);
    }
}

static void redisLibeventSetTimeout(void *privdata, struct timeval tv) {
    redisLibeventEvents *e = (redisLibeventEvents *)privdata;
    short flags = e->flags;
    e->flags = 0;
    e->tv = tv;
    redisLibeventUpdate(e, flags, 0);
}

static int redisLibeventAttach(redisAsyncContext *ac, struct event_base *base) {
    redisContext *c = &(ac->c);
    redisLibeventEvents *e;

    /* Nothing should be attached when something is already attached */
    if (ac->ev.data != NULL)
        return REDIS_ERR;

    /* Create container for context and r/w events */
    e = (redisLibeventEvents*)hi_calloc(1, sizeof(*e));
    if (e == NULL)
        return REDIS_ERR;

    e->context = ac;

    /* Register functions to start/stop listening for events */
    ac->ev.addRead = redisLibeventAddRead;
    ac->ev.delRead = redisLibeventDelRead;
    ac->ev.addWrite = redisLibeventAddWrite;
    ac->ev.delWrite = redisLibeventDelWrite;
    ac->ev.cleanup = redisLibeventCleanup;
    ac->ev.scheduleTimer = redisLibeventSetTimeout;
    ac->ev.data = e;

    /* Initialize and install read/write events */
    e->ev = event_new(base, c->fd, EV_READ | EV_WRITE, redisLibeventHandler, e);
    e->base = base;
    return REDIS_OK;
}
#endif
