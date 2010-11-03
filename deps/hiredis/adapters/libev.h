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

void redisLibevReadEvent(struct ev_loop *loop, ev_io *watcher, int revents) {
    ((void)loop); ((void)revents);
    redisLibevEvents *e = watcher->data;
    redisAsyncHandleRead(e->context);
}

void redisLibevWriteEvent(struct ev_loop *loop, ev_io *watcher, int revents) {
    ((void)loop); ((void)revents);
    redisLibevEvents *e = watcher->data;
    redisAsyncHandleWrite(e->context);
}

void redisLibevAddRead(void *privdata) {
    redisLibevEvents *e = privdata;
    if (!e->reading) {
        e->reading = 1;
        ev_io_start(e->loop,&e->rev);
    }
}

void redisLibevDelRead(void *privdata) {
    redisLibevEvents *e = privdata;
    if (e->reading) {
        e->reading = 0;
        ev_io_stop(e->loop,&e->rev);
    }
}

void redisLibevAddWrite(void *privdata) {
    redisLibevEvents *e = privdata;
    if (!e->writing) {
        e->writing = 1;
        ev_io_start(e->loop,&e->wev);
    }
}

void redisLibevDelWrite(void *privdata) {
    redisLibevEvents *e = privdata;
    if (e->writing) {
        e->writing = 0;
        ev_io_stop(e->loop,&e->wev);
    }
}

void redisLibevCleanup(void *privdata) {
    redisLibevEvents *e = privdata;
    redisLibevDelRead(privdata);
    redisLibevDelWrite(privdata);
    free(e);
}

int redisLibevAttach(redisAsyncContext *ac, struct ev_loop *loop) {
    redisContext *c = &(ac->c);
    redisLibevEvents *e;

    /* Nothing should be attached when something is already attached */
    if (ac->data != NULL)
        return REDIS_ERR;

    /* Create container for context and r/w events */
    e = malloc(sizeof(*e));
    e->context = ac;
    e->loop = loop;
    e->reading = e->writing = 0;
    e->rev.data = e;
    e->wev.data = e;

    /* Register functions to start/stop listening for events */
    ac->evAddRead = redisLibevAddRead;
    ac->evDelRead = redisLibevDelRead;
    ac->evAddWrite = redisLibevAddWrite;
    ac->evDelWrite = redisLibevDelWrite;
    ac->evCleanup = redisLibevCleanup;
    ac->data = e;

    /* Initialize read/write events */
    ev_io_init(&e->rev,redisLibevReadEvent,c->fd,EV_READ);
    ev_io_init(&e->wev,redisLibevWriteEvent,c->fd,EV_WRITE);
    return REDIS_OK;
}
