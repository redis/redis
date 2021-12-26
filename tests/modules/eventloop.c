#define REDISMODULE_EXPERIMENTAL_API
#include "redismodule.h"
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <memory.h>

int fds[2];
long long buf_size;
char *src;
long long src_offset;
char *dst;
long long dst_offset;
long long slow; /* If not zero, do a single read/write and return */

RedisModuleBlockedClient *bc;
RedisModuleCtx *reply_ctx;

void onReadable(int fd, void *user_data, int mask) {
    REDISMODULE_NOT_USED(user_data);
    REDISMODULE_NOT_USED(mask);

    RedisModule_EventLoopDel(fd, REDISMODULE_EVENTLOOP_READABLE);
    do {
        /* Read 1024 bytes max at once to give a chance to exit loop if 'slow'
         * is set */
        int bytes = buf_size - dst_offset < 1024 ? buf_size - dst_offset : 1024;
        int rd = read(fd, dst + dst_offset, bytes);
        if (rd <= 0)
            goto out;
        dst_offset += rd;

        /* Received all bytes */
        if (dst_offset == buf_size) {
            if (memcmp(src, dst, buf_size) == 0)
                RedisModule_ReplyWithSimpleString(reply_ctx, "OK");
            else
                RedisModule_ReplyWithError(reply_ctx, "ERR bytes mismatch");

            RedisModule_FreeThreadSafeContext(reply_ctx);
            RedisModule_UnblockClient(bc, NULL);
            RedisModule_Free(src);
            RedisModule_Free(dst);
            close(fds[0]);
            close(fds[1]);
            return;
        }
    } while (!slow);

out:
    if (RedisModule_EventLoopAdd(fd, REDISMODULE_EVENTLOOP_READABLE,
        onReadable, NULL) != REDISMODULE_OK) abort();
}

void onWritable(int fd, void *user_data, int mask) {
    REDISMODULE_NOT_USED(user_data);
    REDISMODULE_NOT_USED(mask);

    RedisModule_EventLoopDel(fd, REDISMODULE_EVENTLOOP_WRITABLE);
    do {
        if (src_offset >= buf_size)
            return;
        /* Write 2048 bytes max at once to give a chance to exit loop if 'slow'
         * is set */
        int bytes = buf_size - src_offset < 2048 ? buf_size - src_offset : 2048;
        int written = write(fd, src + src_offset, bytes);
        if (written <= 0)
            goto out;

        src_offset += written;
    } while (!slow);
out:
    if (RedisModule_EventLoopAdd(fd, REDISMODULE_EVENTLOOP_WRITABLE,
        onWritable, NULL) != REDISMODULE_OK) abort();
    RedisModule_EventLoopWakeup();
}

int sendbytes(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 3) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    if (RedisModule_StringToLongLong(argv[1], &slow) != REDISMODULE_OK) {
        RedisModule_ReplyWithError(ctx, "Invalid integer value");
        return REDISMODULE_OK;
    }

    if (RedisModule_StringToLongLong(argv[2], &buf_size) != REDISMODULE_OK ||
        buf_size == 0) {
        RedisModule_ReplyWithError(ctx, "Invalid integer value");
        return REDISMODULE_OK;
    }

    bc = RedisModule_BlockClient(ctx, NULL, NULL, NULL, 0);
    reply_ctx = RedisModule_GetThreadSafeContext(bc);

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
        onReadable, NULL) != REDISMODULE_OK) return REDISMODULE_ERR;
    if (RedisModule_EventLoopAdd(fds[1], REDISMODULE_EVENTLOOP_WRITABLE,
        onWritable, NULL) != REDISMODULE_OK) return REDISMODULE_ERR;
    return REDISMODULE_OK;
}

static long long beforeSleepCount;
static long long afterSleepCount;

void beforeSleepCallback() {
    beforeSleepCount++;
}

void afterSleepCallback() {
    afterSleepCount++;
}

int iteration(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    RedisModule_Assert(beforeSleepCount == afterSleepCount);
    RedisModule_ReplyWithLongLong(ctx, beforeSleepCount);
    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx,"eventloop",1,REDISMODULE_APIVER_1)
        == REDISMODULE_ERR) return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "test.sendbytes", sendbytes, "", 0, 0, 0)
        == REDISMODULE_ERR) return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "test.iteration", iteration, "", 0, 0, 0)
        == REDISMODULE_ERR) return REDISMODULE_ERR;

    if (RedisModule_SubscribeToServerEvent(ctx, RedisModuleEvent_BeforeSleep,
        beforeSleepCallback) != REDISMODULE_OK) return REDISMODULE_ERR;

    if (RedisModule_SubscribeToServerEvent(ctx, RedisModuleEvent_AfterSleep,
        afterSleepCallback) != REDISMODULE_OK) return REDISMODULE_ERR;

    return REDISMODULE_OK;
}
