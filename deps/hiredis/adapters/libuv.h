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


static void on_close(uv_handle_t* handle) {
  redisLibuvEvents* p = (redisLibuvEvents*)handle->data;

  hi_free(p);
}


static void redisLibuvCleanup(void *privdata) {
  redisLibuvEvents* p = (redisLibuvEvents*)privdata;

  p->context = NULL; // indicate that context might no longer exist
  uv_close((uv_handle_t*)&p->handle, on_close);
}


static int redisLibuvAttach(redisAsyncContext* ac, uv_loop_t* loop) {
  redisContext *c = &(ac->c);

  if (ac->ev.data != NULL) {
    return REDIS_ERR;
  }

  ac->ev.addRead  = redisLibuvAddRead;
  ac->ev.delRead  = redisLibuvDelRead;
  ac->ev.addWrite = redisLibuvAddWrite;
  ac->ev.delWrite = redisLibuvDelWrite;
  ac->ev.cleanup  = redisLibuvCleanup;

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
