#ifndef HIREDIS_LIBSDEVENT_H
#define HIREDIS_LIBSDEVENT_H
#include <systemd/sd-event.h>
#include "../hiredis.h"
#include "../async.h"

#define REDIS_LIBSDEVENT_DELETED 0x01
#define REDIS_LIBSDEVENT_ENTERED 0x02

typedef struct redisLibsdeventEvents {
    redisAsyncContext *context;
    struct sd_event *event;
    struct sd_event_source *fdSource;
    struct sd_event_source *timerSource;
    int fd;
    short flags;
    short state;
} redisLibsdeventEvents;

static void redisLibsdeventDestroy(redisLibsdeventEvents *e) {
    if (e->fdSource) {
        e->fdSource = sd_event_source_disable_unref(e->fdSource);
    }
    if (e->timerSource) {
        e->timerSource = sd_event_source_disable_unref(e->timerSource);
    }
    sd_event_unref(e->event);
    hi_free(e);
}

static int redisLibsdeventTimeoutHandler(sd_event_source *s, uint64_t usec, void *userdata) {
    ((void)s);
    ((void)usec);
    redisLibsdeventEvents *e = (redisLibsdeventEvents*)userdata;
    redisAsyncHandleTimeout(e->context);
    return 0;
}

static int redisLibsdeventHandler(sd_event_source *s, int fd, uint32_t event, void *userdata) {
    ((void)s);
    ((void)fd);
    redisLibsdeventEvents *e = (redisLibsdeventEvents*)userdata;
    e->state |= REDIS_LIBSDEVENT_ENTERED;

#define CHECK_DELETED() if (e->state & REDIS_LIBSDEVENT_DELETED) {\
        redisLibsdeventDestroy(e);\
        return 0; \
    }

    if ((event & EPOLLIN) && e->context && (e->state & REDIS_LIBSDEVENT_DELETED) == 0) {
        redisAsyncHandleRead(e->context);
        CHECK_DELETED();
    }

    if ((event & EPOLLOUT) && e->context && (e->state & REDIS_LIBSDEVENT_DELETED) == 0) {
        redisAsyncHandleWrite(e->context);
        CHECK_DELETED();
    }

    e->state &= ~REDIS_LIBSDEVENT_ENTERED;
#undef CHECK_DELETED

    return 0;
}

static void redisLibsdeventAddRead(void *userdata) {
    redisLibsdeventEvents *e = (redisLibsdeventEvents*)userdata;

    if (e->flags & EPOLLIN) {
        return;
    }

    e->flags |= EPOLLIN;

    if (e->flags & EPOLLOUT) {
        sd_event_source_set_io_events(e->fdSource, e->flags);
    } else {
        sd_event_add_io(e->event, &e->fdSource, e->fd, e->flags, redisLibsdeventHandler, e);
    }
}

static void redisLibsdeventDelRead(void *userdata) {
    redisLibsdeventEvents *e = (redisLibsdeventEvents*)userdata;

    e->flags &= ~EPOLLIN;

    if (e->flags) {
        sd_event_source_set_io_events(e->fdSource, e->flags);
    } else {
        e->fdSource = sd_event_source_disable_unref(e->fdSource);
    }
}

static void redisLibsdeventAddWrite(void *userdata) {
    redisLibsdeventEvents *e = (redisLibsdeventEvents*)userdata;

    if (e->flags & EPOLLOUT) {
        return;
    }

    e->flags |= EPOLLOUT;

    if (e->flags & EPOLLIN) {
        sd_event_source_set_io_events(e->fdSource, e->flags);
    } else {
        sd_event_add_io(e->event, &e->fdSource, e->fd, e->flags, redisLibsdeventHandler, e);
    }
}

static void redisLibsdeventDelWrite(void *userdata) {
    redisLibsdeventEvents *e = (redisLibsdeventEvents*)userdata;

    e->flags &= ~EPOLLOUT;

    if (e->flags) {
        sd_event_source_set_io_events(e->fdSource, e->flags);
    } else {
        e->fdSource = sd_event_source_disable_unref(e->fdSource);
    }
}

static void redisLibsdeventCleanup(void *userdata) {
    redisLibsdeventEvents *e = (redisLibsdeventEvents*)userdata;

    if (!e) {
        return;
    }

    if (e->state & REDIS_LIBSDEVENT_ENTERED) {
        e->state |= REDIS_LIBSDEVENT_DELETED;
    } else {
        redisLibsdeventDestroy(e);
    }
}

static void redisLibsdeventSetTimeout(void *userdata, struct timeval tv) {
    redisLibsdeventEvents *e = (redisLibsdeventEvents *)userdata;

    uint64_t usec = tv.tv_sec * 1000000 + tv.tv_usec;
    if (!e->timerSource) {
        sd_event_add_time_relative(e->event, &e->timerSource, CLOCK_MONOTONIC, usec, 1, redisLibsdeventTimeoutHandler, e);
    } else {
        sd_event_source_set_time_relative(e->timerSource, usec);
    }
}

static int redisLibsdeventAttach(redisAsyncContext *ac, struct sd_event *event) {
    redisContext *c = &(ac->c);
    redisLibsdeventEvents *e;

    /* Nothing should be attached when something is already attached */
    if (ac->ev.data != NULL)
        return REDIS_ERR;

    /* Create container for context and r/w events */
    e = (redisLibsdeventEvents*)hi_calloc(1, sizeof(*e));
    if (e == NULL)
        return REDIS_ERR;

    /* Initialize and increase event refcount */
    e->context = ac;
    e->event = event;
    e->fd = c->fd;
    sd_event_ref(event);

    /* Register functions to start/stop listening for events */
    ac->ev.addRead = redisLibsdeventAddRead;
    ac->ev.delRead = redisLibsdeventDelRead;
    ac->ev.addWrite = redisLibsdeventAddWrite;
    ac->ev.delWrite = redisLibsdeventDelWrite;
    ac->ev.cleanup = redisLibsdeventCleanup;
    ac->ev.scheduleTimer = redisLibsdeventSetTimeout;
    ac->ev.data = e;

    return REDIS_OK;
}
#endif
