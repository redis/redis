/* This module contains four tests :
 * 1- test.sanity    : Basic tests for argument validation mostly.
 * 2- test.sendbytes : Creates a pipe and registers its fds to the event loop,
 *                     one end of the pipe for read events and the other end for
 *                     the write events. On writable event, data is written. On
 *                     readable event data is read. Repeated until all data is
 *                     received.
 * 3- test.iteration : A test for BEFORE_SLEEP and AFTER_SLEEP callbacks.
 *                     Counters are incremented each time these events are
 *                     fired. They should be equal and increment monotonically.
 * 4- test.oneshot   : Test for oneshot API
 */

#include "redismodule.h"
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <memory.h>
#include <errno.h>

int fds[2];
long long buf_size;
char *src;
long long src_offset;
char *dst;
long long dst_offset;

RedisModuleBlockedClient *bc;
RedisModuleCtx *reply_ctx;

void onReadable(int fd, void *user_data, int mask) {
    REDISMODULE_NOT_USED(mask);

    RedisModule_Assert(strcmp(user_data, "userdataread") == 0);

    while (1) {
        int rd = read(fd, dst + dst_offset, buf_size - dst_offset);
        if (rd <= 0)
            return;
        dst_offset += rd;

        /* Received all bytes */
        if (dst_offset == buf_size) {
            if (memcmp(src, dst, buf_size) == 0)
                RedisModule_ReplyWithSimpleString(reply_ctx, "OK");
            else
                RedisModule_ReplyWithError(reply_ctx, "ERR bytes mismatch");

            RedisModule_EventLoopDel(fds[0], REDISMODULE_EVENTLOOP_READABLE);
            RedisModule_EventLoopDel(fds[1], REDISMODULE_EVENTLOOP_WRITABLE);
            RedisModule_Free(src);
            RedisModule_Free(dst);
            close(fds[0]);
            close(fds[1]);

            RedisModule_FreeThreadSafeContext(reply_ctx);
            RedisModule_UnblockClient(bc, NULL);
            return;
        }
    };
}

void onWritable(int fd, void *user_data, int mask) {
    REDISMODULE_NOT_USED(user_data);
    REDISMODULE_NOT_USED(mask);

    RedisModule_Assert(strcmp(user_data, "userdatawrite") == 0);

    while (1) {
        /* Check if we sent all data */
        if (src_offset >= buf_size)
            return;
        int written = write(fd, src + src_offset, buf_size - src_offset);
        if (written <= 0) {
            return;
        }

        src_offset += written;
    };
}

/* Create a pipe(), register pipe fds to the event loop and send/receive data
 * using them. */
int sendbytes(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    if (RedisModule_StringToLongLong(argv[1], &buf_size) != REDISMODULE_OK ||
        buf_size == 0) {
        RedisModule_ReplyWithError(ctx, "Invalid integer value");
        return REDISMODULE_OK;
    }

    bc = RedisModule_BlockClient(ctx, NULL, NULL, NULL, 0);
    reply_ctx = RedisModule_GetThreadSafeContext(bc);

    /* Allocate source buffer and write some random data */
    src = RedisModule_Calloc(1,buf_size);
    src_offset = 0;
    memset(src, rand() % 0xFF, buf_size);
    memcpy(src, "randomtestdata", strlen("randomtestdata"));

    dst = RedisModule_Calloc(1,buf_size);
    dst_offset = 0;

    /* Create a pipe and register it to the event loop. */
    if (pipe(fds) < 0) return REDISMODULE_ERR;
    if (fcntl(fds[0], F_SETFL, O_NONBLOCK) < 0) return REDISMODULE_ERR;
    if (fcntl(fds[1], F_SETFL, O_NONBLOCK) < 0) return REDISMODULE_ERR;

    if (RedisModule_EventLoopAdd(fds[0], REDISMODULE_EVENTLOOP_READABLE,
        onReadable, "userdataread") != REDISMODULE_OK) return REDISMODULE_ERR;
    if (RedisModule_EventLoopAdd(fds[1], REDISMODULE_EVENTLOOP_WRITABLE,
        onWritable, "userdatawrite") != REDISMODULE_OK) return REDISMODULE_ERR;
    return REDISMODULE_OK;
}

int sanity(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (pipe(fds) < 0) return REDISMODULE_ERR;

    if (RedisModule_EventLoopAdd(fds[0], 9999999, onReadable, NULL)
        == REDISMODULE_OK || errno != EINVAL) {
        RedisModule_ReplyWithError(ctx, "ERR non-existing event type should fail");
        goto out;
    }
    if (RedisModule_EventLoopAdd(-1, REDISMODULE_EVENTLOOP_READABLE, onReadable, NULL)
        == REDISMODULE_OK || errno != ERANGE) {
        RedisModule_ReplyWithError(ctx, "ERR out of range fd should fail");
        goto out;
    }
    if (RedisModule_EventLoopAdd(99999999, REDISMODULE_EVENTLOOP_READABLE, onReadable, NULL)
        == REDISMODULE_OK || errno != ERANGE) {
        RedisModule_ReplyWithError(ctx, "ERR out of range fd should fail");
        goto out;
    }
    if (RedisModule_EventLoopAdd(fds[0], REDISMODULE_EVENTLOOP_READABLE, NULL, NULL)
        == REDISMODULE_OK || errno != EINVAL) {
        RedisModule_ReplyWithError(ctx, "ERR null callback should fail");
        goto out;
    }
    if (RedisModule_EventLoopAdd(fds[0], 9999999, onReadable, NULL)
        == REDISMODULE_OK || errno != EINVAL) {
        RedisModule_ReplyWithError(ctx, "ERR non-existing event type should fail");
        goto out;
    }
    if (RedisModule_EventLoopDel(fds[0], REDISMODULE_EVENTLOOP_READABLE)
        != REDISMODULE_OK || errno != 0) {
        RedisModule_ReplyWithError(ctx, "ERR del on non-registered fd should not fail");
        goto out;
    }
    if (RedisModule_EventLoopDel(fds[0], 9999999) == REDISMODULE_OK ||
        errno != EINVAL) {
        RedisModule_ReplyWithError(ctx, "ERR non-existing event type should fail");
        goto out;
    }
    if (RedisModule_EventLoopDel(-1, REDISMODULE_EVENTLOOP_READABLE)
        == REDISMODULE_OK || errno != ERANGE) {
        RedisModule_ReplyWithError(ctx, "ERR out of range fd should fail");
        goto out;
    }
    if (RedisModule_EventLoopDel(99999999, REDISMODULE_EVENTLOOP_READABLE)
        == REDISMODULE_OK || errno != ERANGE) {
        RedisModule_ReplyWithError(ctx, "ERR out of range fd should fail");
        goto out;
    }
    if (RedisModule_EventLoopAdd(fds[0], REDISMODULE_EVENTLOOP_READABLE, onReadable, NULL)
        != REDISMODULE_OK || errno != 0) {
        RedisModule_ReplyWithError(ctx, "ERR Add failed");
        goto out;
    }
    if (RedisModule_EventLoopAdd(fds[0], REDISMODULE_EVENTLOOP_READABLE, onReadable, NULL)
        != REDISMODULE_OK || errno != 0) {
        RedisModule_ReplyWithError(ctx, "ERR Adding same fd twice failed");
        goto out;
    }
    if (RedisModule_EventLoopDel(fds[0], REDISMODULE_EVENTLOOP_READABLE)
        != REDISMODULE_OK || errno != 0) {
        RedisModule_ReplyWithError(ctx, "ERR Del failed");
        goto out;
    }
    if (RedisModule_EventLoopAddOneShot(NULL, NULL) == REDISMODULE_OK || errno != EINVAL) {
        RedisModule_ReplyWithError(ctx, "ERR null callback should fail");
        goto out;
    }

    RedisModule_ReplyWithSimpleString(ctx, "OK");
out:
    close(fds[0]);
    close(fds[1]);
    return REDISMODULE_OK;
}

static long long beforeSleepCount;
static long long afterSleepCount;

int iteration(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    /* On each event loop iteration, eventloopCallback() is called. We increment
     * beforeSleepCount and afterSleepCount, so these two should be equal.
     * We reply with iteration count, caller can test if iteration count
     * increments monotonically */
    RedisModule_Assert(beforeSleepCount == afterSleepCount);
    RedisModule_ReplyWithLongLong(ctx, beforeSleepCount);
    return REDISMODULE_OK;
}

void oneshotCallback(void* arg)
{
    RedisModule_Assert(strcmp(arg, "userdata") == 0);
    RedisModule_ReplyWithSimpleString(reply_ctx, "OK");
    RedisModule_FreeThreadSafeContext(reply_ctx);
    RedisModule_UnblockClient(bc, NULL);
}

int oneshot(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    bc = RedisModule_BlockClient(ctx, NULL, NULL, NULL, 0);
    reply_ctx = RedisModule_GetThreadSafeContext(bc);

    if (RedisModule_EventLoopAddOneShot(oneshotCallback, "userdata") != REDISMODULE_OK) {
        RedisModule_ReplyWithError(ctx, "ERR oneshot failed");
        RedisModule_FreeThreadSafeContext(reply_ctx);
        RedisModule_UnblockClient(bc, NULL);
    }
    return REDISMODULE_OK;
}

void eventloopCallback(struct RedisModuleCtx *ctx, RedisModuleEvent eid, uint64_t subevent, void *data) {
    REDISMODULE_NOT_USED(ctx);
    REDISMODULE_NOT_USED(eid);
    REDISMODULE_NOT_USED(subevent);
    REDISMODULE_NOT_USED(data);

    RedisModule_Assert(eid.id == REDISMODULE_EVENT_EVENTLOOP);
    if (subevent == REDISMODULE_SUBEVENT_EVENTLOOP_BEFORE_SLEEP)
        beforeSleepCount++;
    else if (subevent == REDISMODULE_SUBEVENT_EVENTLOOP_AFTER_SLEEP)
        afterSleepCount++;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx,"eventloop",1,REDISMODULE_APIVER_1)
        == REDISMODULE_ERR) return REDISMODULE_ERR;

    /* Test basics. */
    if (RedisModule_CreateCommand(ctx, "test.sanity", sanity, "", 0, 0, 0)
        == REDISMODULE_ERR) return REDISMODULE_ERR;

    /* Register a command to create a pipe() and send data through it by using
     * event loop API. */
    if (RedisModule_CreateCommand(ctx, "test.sendbytes", sendbytes, "", 0, 0, 0)
        == REDISMODULE_ERR) return REDISMODULE_ERR;

    /* Register a command to return event loop iteration count. */
    if (RedisModule_CreateCommand(ctx, "test.iteration", iteration, "", 0, 0, 0)
        == REDISMODULE_ERR) return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "test.oneshot", oneshot, "", 0, 0, 0)
        == REDISMODULE_ERR) return REDISMODULE_ERR;

    if (RedisModule_SubscribeToServerEvent(ctx, RedisModuleEvent_EventLoop,
        eventloopCallback) != REDISMODULE_OK) return REDISMODULE_ERR;

    return REDISMODULE_OK;
}
