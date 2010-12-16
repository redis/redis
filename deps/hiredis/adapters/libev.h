#include <sys/types.h>
#include <ev.h>
#include "../hiredis.h"
#include "../async.h"

typedef struct redisLibevEvents {
    redisAsyncContext *context;
    struct ev_loop *loop;
    int reading, writing;
    ev_io rev, wev;
} redisLibevEvents;

void redisLibevReadEvent(EV_P_ ev_io *watcher, int revents) {
#if EV_MULTIPLICITY
    ((void)loop);
#endif
    ((void)revents);

    redisLibevEvents *e = (redisLibevEvents*)watcher->data;
    redisAsyncHandleRead(e->context);
}

void redisLibevWriteEvent(EV_P_ ev_io *watcher, int revents) {
#if EV_MULTIPLICITY
    ((void)loop);
#endif
    ((void)revents);

    redisLibevEvents *e = (redisLibevEvents*)watcher->data;
    redisAsyncHandleWrite(e->context);
}

void redisLibevAddRead(void *privdata) {
    redisLibevEvents *e = (redisLibevEvents*)privdata;
    struct ev_loop *loop = e->loop;
    ((void)loop);
    if (!e->reading) {
        e->reading = 1;
        ev_io_start(EV_A_ &e->rev);
    }
}

void redisLibevDelRead(void *privdata) {
    redisLibevEvents *e = (redisLibevEvents*)privdata;
    struct ev_loop *loop = e->loop;
    ((void)loop);
    if (e->reading) {
        e->reading = 0;
        ev_io_stop(EV_A_ &e->rev);
    }
}

void redisLibevAddWrite(void *privdata) {
    redisLibevEvents *e = (redisLibevEvents*)privdata;
    struct ev_loop *loop = e->loop;
    ((void)loop);
    if (!e->writing) {
        e->writing = 1;
        ev_io_start(EV_A_ &e->wev);
    }
}

void redisLibevDelWrite(void *privdata) {
    redisLibevEvents *e = (redisLibevEvents*)privdata;
    struct ev_loop *loop = e->loop;
    ((void)loop);
    if (e->writing) {
        e->writing = 0;
        ev_io_stop(EV_A_ &e->wev);
    }
}

void redisLibevCleanup(void *privdata) {
    redisLibevEvents *e = (redisLibevEvents*)privdata;
    redisLibevDelRead(privdata);
    redisLibevDelWrite(privdata);
    free(e);
}

int redisLibevAttach(EV_P_ redisAsyncContext *ac) {
    redisContext *c = &(ac->c);
    redisLibevEvents *e;

    /* Nothing should be attached when something is already attached */
    if (ac->_adapter_data != NULL)
        return REDIS_ERR;

    /* Create container for context and r/w events */
    e = (redisLibevEvents*)malloc(sizeof(*e));
    e->context = ac;
#if EV_MULTIPLICITY
    e->loop = loop;
#else
    e->loop = NULL;
#endif
    e->reading = e->writing = 0;
    e->rev.data = e;
    e->wev.data = e;

    /* Register functions to start/stop listening for events */
    ac->evAddRead = redisLibevAddRead;
    ac->evDelRead = redisLibevDelRead;
    ac->evAddWrite = redisLibevAddWrite;
    ac->evDelWrite = redisLibevDelWrite;
    ac->evCleanup = redisLibevCleanup;
    ac->_adapter_data = e;

    /* Initialize read/write events */
    ev_io_init(&e->rev,redisLibevReadEvent,c->fd,EV_READ);
    ev_io_init(&e->wev,redisLibevWriteEvent,c->fd,EV_WRITE);
    return REDIS_OK;
}

