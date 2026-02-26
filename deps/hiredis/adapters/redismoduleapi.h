#ifndef __HIREDIS_REDISMODULEAPI_H__
#define __HIREDIS_REDISMODULEAPI_H__

#include "redismodule.h"

#include "../async.h"
#include "../hiredis.h"

#include <sys/types.h>

typedef struct redisModuleEvents {
    redisAsyncContext *context;
    RedisModuleCtx *module_ctx;
    int fd;
    int reading, writing;
    int timer_active;
    RedisModuleTimerID timer_id;
} redisModuleEvents;

static inline void redisModuleReadEvent(int fd, void *privdata, int mask) {
    (void) fd;
    (void) mask;

    redisModuleEvents *e = (redisModuleEvents*)privdata;
    redisAsyncHandleRead(e->context);
}

static inline void redisModuleWriteEvent(int fd, void *privdata, int mask) {
    (void) fd;
    (void) mask;

    redisModuleEvents *e = (redisModuleEvents*)privdata;
    redisAsyncHandleWrite(e->context);
}

static inline void redisModuleAddRead(void *privdata) {
    redisModuleEvents *e = (redisModuleEvents*)privdata;
    if (!e->reading) {
        e->reading = 1;
        RedisModule_EventLoopAdd(e->fd, REDISMODULE_EVENTLOOP_READABLE, redisModuleReadEvent, e);
    }
}

static inline void redisModuleDelRead(void *privdata) {
    redisModuleEvents *e = (redisModuleEvents*)privdata;
    if (e->reading) {
        e->reading = 0;
        RedisModule_EventLoopDel(e->fd, REDISMODULE_EVENTLOOP_READABLE);
    }
}

static inline void redisModuleAddWrite(void *privdata) {
    redisModuleEvents *e = (redisModuleEvents*)privdata;
    if (!e->writing) {
        e->writing = 1;
        RedisModule_EventLoopAdd(e->fd, REDISMODULE_EVENTLOOP_WRITABLE, redisModuleWriteEvent, e);
    }
}

static inline void redisModuleDelWrite(void *privdata) {
    redisModuleEvents *e = (redisModuleEvents*)privdata;
    if (e->writing) {
        e->writing = 0;
        RedisModule_EventLoopDel(e->fd, REDISMODULE_EVENTLOOP_WRITABLE);
    }
}

static inline void redisModuleStopTimer(void *privdata) {
    redisModuleEvents *e = (redisModuleEvents*)privdata;
    if (e->timer_active) {
        RedisModule_StopTimer(e->module_ctx, e->timer_id, NULL);
    }
    e->timer_active = 0;
}

static inline void redisModuleCleanup(void *privdata) {
    redisModuleEvents *e = (redisModuleEvents*)privdata;
    redisModuleDelRead(privdata);
    redisModuleDelWrite(privdata);
    redisModuleStopTimer(privdata);
    hi_free(e);
}

static inline void redisModuleTimeout(RedisModuleCtx *ctx, void *privdata) {
    (void) ctx;

    redisModuleEvents *e = (redisModuleEvents*)privdata;
    e->timer_active = 0;
    redisAsyncHandleTimeout(e->context);
}

static inline void redisModuleSetTimeout(void *privdata, struct timeval tv) {
    redisModuleEvents* e = (redisModuleEvents*)privdata;

    redisModuleStopTimer(privdata);

    mstime_t millis = tv.tv_sec * 1000 + tv.tv_usec / 1000.0;
    e->timer_id = RedisModule_CreateTimer(e->module_ctx, millis, redisModuleTimeout, e);
    e->timer_active = 1;
}

/* Check if Redis version is compatible with the adapter. */
static inline int redisModuleCompatibilityCheck(void) {
    if (!RedisModule_EventLoopAdd ||
        !RedisModule_EventLoopDel ||
        !RedisModule_CreateTimer ||
        !RedisModule_StopTimer) {
        return REDIS_ERR;
    }
    return REDIS_OK;
}

static inline int redisModuleAttach(redisAsyncContext *ac, RedisModuleCtx *module_ctx) {
    redisContext *c = &(ac->c);
    redisModuleEvents *e;

    /* Nothing should be attached when something is already attached */
    if (ac->ev.data != NULL)
        return REDIS_ERR;

    /* Create container for context and r/w events */
    e = (redisModuleEvents*)hi_malloc(sizeof(*e));
    if (e == NULL)
        return REDIS_ERR;

    e->context = ac;
    e->module_ctx = module_ctx;
    e->fd = c->fd;
    e->reading = e->writing = 0;
    e->timer_active = 0;

    /* Register functions to start/stop listening for events */
    ac->ev.addRead = redisModuleAddRead;
    ac->ev.delRead = redisModuleDelRead;
    ac->ev.addWrite = redisModuleAddWrite;
    ac->ev.delWrite = redisModuleDelWrite;
    ac->ev.cleanup = redisModuleCleanup;
    ac->ev.scheduleTimer = redisModuleSetTimeout;
    ac->ev.data = e;

    return REDIS_OK;
}

#endif
