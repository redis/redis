#include <sys/types.h>
#include <event.h>
#include "../hiredis.h"
#include "../async.h"

typedef struct redisLibeventEvents {
    redisAsyncContext *context;
    struct event rev, wev;
} redisLibeventEvents;

void redisLibeventReadEvent(int fd, short event, void *arg) {
    ((void)fd); ((void)event);
    redisLibeventEvents *e = arg;
    redisAsyncHandleRead(e->context);
}

void redisLibeventWriteEvent(int fd, short event, void *arg) {
    ((void)fd); ((void)event);
    redisLibeventEvents *e = arg;
    redisAsyncHandleWrite(e->context);
}

void redisLibeventAddRead(void *privdata) {
    redisLibeventEvents *e = privdata;
    event_add(&e->rev,NULL);
}

void redisLibeventDelRead(void *privdata) {
    redisLibeventEvents *e = privdata;
    event_del(&e->rev);
}

void redisLibeventAddWrite(void *privdata) {
    redisLibeventEvents *e = privdata;
    event_add(&e->wev,NULL);
}

void redisLibeventDelWrite(void *privdata) {
    redisLibeventEvents *e = privdata;
    event_del(&e->wev);
}

void redisLibeventCleanup(void *privdata) {
    redisLibeventEvents *e = privdata;
    event_del(&e->rev);
    event_del(&e->wev);
    free(e);
}

int redisLibeventAttach(redisAsyncContext *ac, struct event_base *base) {
    redisContext *c = &(ac->c);
    redisLibeventEvents *e;

    /* Nothing should be attached when something is already attached */
    if (ac->data != NULL)
        return REDIS_ERR;

    /* Create container for context and r/w events */
    e = malloc(sizeof(*e));
    e->context = ac;

    /* Register functions to start/stop listening for events */
    ac->evAddRead = redisLibeventAddRead;
    ac->evDelRead = redisLibeventDelRead;
    ac->evAddWrite = redisLibeventAddWrite;
    ac->evDelWrite = redisLibeventDelWrite;
    ac->evCleanup = redisLibeventCleanup;
    ac->data = e;

    /* Initialize and install read/write events */
    event_set(&e->rev,c->fd,EV_READ,redisLibeventReadEvent,e);
    event_set(&e->wev,c->fd,EV_WRITE,redisLibeventWriteEvent,e);
    event_base_set(base,&e->rev);
    event_base_set(base,&e->wev);
    return REDIS_OK;
}
