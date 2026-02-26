#ifndef __HIREDIS_LIBHV_H__
#define __HIREDIS_LIBHV_H__

#include <hv/hloop.h>
#include "../hiredis.h"
#include "../async.h"

typedef struct redisLibhvEvents {
    hio_t *io;
    htimer_t *timer;
} redisLibhvEvents;

static void redisLibhvHandleEvents(hio_t* io) {
    redisAsyncContext* context = (redisAsyncContext*)hevent_userdata(io);
    int events = hio_events(io);
    int revents = hio_revents(io);
    if (context && (events & HV_READ) && (revents & HV_READ)) {
        redisAsyncHandleRead(context);
    }
    if (context && (events & HV_WRITE) && (revents & HV_WRITE)) {
        redisAsyncHandleWrite(context);
    }
}

static void redisLibhvAddRead(void *privdata) {
    redisLibhvEvents* events = (redisLibhvEvents*)privdata;
    hio_add(events->io, redisLibhvHandleEvents, HV_READ);
}

static void redisLibhvDelRead(void *privdata) {
    redisLibhvEvents* events = (redisLibhvEvents*)privdata;
    hio_del(events->io, HV_READ);
}

static void redisLibhvAddWrite(void *privdata) {
    redisLibhvEvents* events = (redisLibhvEvents*)privdata;
    hio_add(events->io, redisLibhvHandleEvents, HV_WRITE);
}

static void redisLibhvDelWrite(void *privdata) {
    redisLibhvEvents* events = (redisLibhvEvents*)privdata;
    hio_del(events->io, HV_WRITE);
}

static void redisLibhvCleanup(void *privdata) {
    redisLibhvEvents* events = (redisLibhvEvents*)privdata;

    if (events->timer)
        htimer_del(events->timer);

    hio_close(events->io);
    hevent_set_userdata(events->io, NULL);

    hi_free(events);
}

static void redisLibhvTimeout(htimer_t* timer) {
    hio_t* io = (hio_t*)hevent_userdata(timer);
    redisAsyncHandleTimeout((redisAsyncContext*)hevent_userdata(io));
}

static void redisLibhvSetTimeout(void *privdata, struct timeval tv) {
    redisLibhvEvents* events;
    uint32_t millis;
    hloop_t* loop;

    events = (redisLibhvEvents*)privdata;
    millis = tv.tv_sec * 1000 + tv.tv_usec / 1000;

    if (millis == 0) {
        /* Libhv disallows zero'd timers so treat this as a delete or NO OP */
        if (events->timer) {
            htimer_del(events->timer);
            events->timer = NULL;
        }
    } else if (events->timer == NULL) {
        /* Add new timer */
        loop = hevent_loop(events->io);
        events->timer = htimer_add(loop, redisLibhvTimeout, millis, 1);
        hevent_set_userdata(events->timer, events->io);
    } else {
        /* Update existing timer */
        htimer_reset(events->timer, millis);
    }
}

static int redisLibhvAttach(redisAsyncContext* ac, hloop_t* loop) {
    redisContext *c = &(ac->c);
    redisLibhvEvents *events;
    hio_t* io = NULL;

    if (ac->ev.data != NULL) {
        return REDIS_ERR;
    }

    /* Create container struct to keep track of our io and any timer */
    events = (redisLibhvEvents*)hi_malloc(sizeof(*events));
    if (events == NULL) {
        return REDIS_ERR;
    }

    io = hio_get(loop, c->fd);
    if (io == NULL) {
        hi_free(events);
        return REDIS_ERR;
    }

    hevent_set_userdata(io, ac);

    events->io = io;
    events->timer = NULL;

    ac->ev.addRead  = redisLibhvAddRead;
    ac->ev.delRead  = redisLibhvDelRead;
    ac->ev.addWrite = redisLibhvAddWrite;
    ac->ev.delWrite = redisLibhvDelWrite;
    ac->ev.cleanup  = redisLibhvCleanup;
    ac->ev.scheduleTimer = redisLibhvSetTimeout;
    ac->ev.data = events;

    return REDIS_OK;
}
#endif
