#ifndef __HIREDIS_LIBUV_H__
#define __HIREDIS_LIBUV_H__
#include <stdlib.h>
#include <uv.h>
#include "../hiredis.h"
#include "../async.h"
#include <string.h>

typedef struct redisLibuvEvents {
    redisAsyncContext* context;
    uv_poll_t          handle;
    uv_timer_t         timer;
    int                events;
} redisLibuvEvents;


static void redisLibuvPoll(uv_poll_t* handle, int status, int events) {
    redisLibuvEvents* p = (redisLibuvEvents*)handle->data;
    int ev = (status ? p->events : events);

    if (p->context != NULL && (ev & UV_READABLE)) {
        redisAsyncHandleRead(p->context);
    }
    if (p->context != NULL && (ev & UV_WRITABLE)) {
        redisAsyncHandleWrite(p->context);
    }
}


static void redisLibuvAddRead(void *privdata) {
    redisLibuvEvents* p = (redisLibuvEvents*)privdata;

    if (p->events & UV_READABLE) {
        return;
    }

    p->events |= UV_READABLE;

    uv_poll_start(&p->handle, p->events, redisLibuvPoll);
}


static void redisLibuvDelRead(void *privdata) {
    redisLibuvEvents* p = (redisLibuvEvents*)privdata;

    p->events &= ~UV_READABLE;

    if (p->events) {
        uv_poll_start(&p->handle, p->events, redisLibuvPoll);
    } else {
        uv_poll_stop(&p->handle);
    }
}


static void redisLibuvAddWrite(void *privdata) {
    redisLibuvEvents* p = (redisLibuvEvents*)privdata;

    if (p->events & UV_WRITABLE) {
        return;
    }

    p->events |= UV_WRITABLE;

    uv_poll_start(&p->handle, p->events, redisLibuvPoll);
}


static void redisLibuvDelWrite(void *privdata) {
    redisLibuvEvents* p = (redisLibuvEvents*)privdata;

    p->events &= ~UV_WRITABLE;

    if (p->events) {
        uv_poll_start(&p->handle, p->events, redisLibuvPoll);
    } else {
        uv_poll_stop(&p->handle);
    }
}

static void on_timer_close(uv_handle_t *handle) {
    redisLibuvEvents* p = (redisLibuvEvents*)handle->data;
    p->timer.data = NULL;
    if (!p->handle.data) {
        // both timer and handle are closed
        hi_free(p);
    }
    // else, wait for `on_handle_close`
}

static void on_handle_close(uv_handle_t *handle) {
    redisLibuvEvents* p = (redisLibuvEvents*)handle->data;
    p->handle.data = NULL;
    if (!p->timer.data) {
        // timer never started, or timer already destroyed
        hi_free(p);
    }
    // else, wait for `on_timer_close`
}

// libuv removed `status` parameter since v0.11.23
// see: https://github.com/libuv/libuv/blob/v0.11.23/include/uv.h
#if (UV_VERSION_MAJOR == 0 && UV_VERSION_MINOR < 11) || \
    (UV_VERSION_MAJOR == 0 && UV_VERSION_MINOR == 11 && UV_VERSION_PATCH < 23)
static void redisLibuvTimeout(uv_timer_t *timer, int status) {
    (void)status; // unused
#else
static void redisLibuvTimeout(uv_timer_t *timer) {
#endif
    redisLibuvEvents *e = (redisLibuvEvents*)timer->data;
    redisAsyncHandleTimeout(e->context);
}

static void redisLibuvSetTimeout(void *privdata, struct timeval tv) {
    redisLibuvEvents* p = (redisLibuvEvents*)privdata;

    uint64_t millsec = tv.tv_sec * 1000 + tv.tv_usec / 1000.0;
    if (!p->timer.data) {
        // timer is uninitialized
        if (uv_timer_init(p->handle.loop, &p->timer) != 0) {
            return;
        }
        p->timer.data = p;
    }
    // updates the timeout if the timer has already started
    // or start the timer
    uv_timer_start(&p->timer, redisLibuvTimeout, millsec, 0);
}

static void redisLibuvCleanup(void *privdata) {
    redisLibuvEvents* p = (redisLibuvEvents*)privdata;

    p->context = NULL; // indicate that context might no longer exist
    if (p->timer.data) {
        uv_close((uv_handle_t*)&p->timer, on_timer_close);
    }
    uv_close((uv_handle_t*)&p->handle, on_handle_close);
}


static int redisLibuvAttach(redisAsyncContext* ac, uv_loop_t* loop) {
    redisContext *c = &(ac->c);

    if (ac->ev.data != NULL) {
        return REDIS_ERR;
    }

    ac->ev.addRead        = redisLibuvAddRead;
    ac->ev.delRead        = redisLibuvDelRead;
    ac->ev.addWrite       = redisLibuvAddWrite;
    ac->ev.delWrite       = redisLibuvDelWrite;
    ac->ev.cleanup        = redisLibuvCleanup;
    ac->ev.scheduleTimer  = redisLibuvSetTimeout;

    redisLibuvEvents* p = (redisLibuvEvents*)hi_malloc(sizeof(*p));
    if (p == NULL)
        return REDIS_ERR;

    memset(p, 0, sizeof(*p));

    if (uv_poll_init_socket(loop, &p->handle, c->fd) != 0) {
        return REDIS_ERR;
    }

    ac->ev.data    = p;
    p->handle.data = p;
    p->context     = ac;

    return REDIS_OK;
}
#endif
