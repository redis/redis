/* Module designed to test the Redis modules subsystem.
 *
 * -----------------------------------------------------------------------------
 *
 * Copyright (c) 2016, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#define REDISMODULE_EXPERIMENTAL_API
#include "../redismodule.h"
#include <string.h>

/* --------------------------------- Helpers -------------------------------- */

/* Return true if the reply and the C null term string matches. */
int TestMatchReply(RedisModuleCallReply *reply, char *str) {
    RedisModuleString *mystr;
    mystr = RedisModule_CreateStringFromCallReply(reply);
    if (!mystr) return 0;
    const char *ptr = RedisModule_StringPtrLen(mystr,NULL);
    return strcmp(ptr,str) == 0;
}

/* ------------------------------- Test units ------------------------------- */

/* TEST.CALL -- Test Call() API. */
int TestCall(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    RedisModule_AutoMemory(ctx);
    RedisModuleCallReply *reply;

    RedisModule_Call(ctx,"DEL","c","mylist");
    RedisModuleString *mystr = RedisModule_CreateString(ctx,"foo",3);
    RedisModule_Call(ctx,"RPUSH","csl","mylist",mystr,(long long)1234);
    reply = RedisModule_Call(ctx,"LRANGE","ccc","mylist","0","-1");
    long long items = RedisModule_CallReplyLength(reply);
    if (items != 2) goto fail;

    RedisModuleCallReply *item0, *item1;

    item0 = RedisModule_CallReplyArrayElement(reply,0);
    item1 = RedisModule_CallReplyArrayElement(reply,1);
    if (!TestMatchReply(item0,"foo")) goto fail;
    if (!TestMatchReply(item1,"1234")) goto fail;

    RedisModule_ReplyWithSimpleString(ctx,"OK");
    return REDISMODULE_OK;

fail:
    RedisModule_ReplyWithSimpleString(ctx,"ERR");
    return REDISMODULE_OK;
}

/* TEST.STRING.APPEND -- Test appending to an existing string object. */
int TestStringAppend(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    RedisModuleString *s = RedisModule_CreateString(ctx,"foo",3);
    RedisModule_StringAppendBuffer(ctx,s,"bar",3);
    RedisModule_ReplyWithString(ctx,s);
    RedisModule_FreeString(ctx,s);
    return REDISMODULE_OK;
}

/* TEST.STRING.APPEND.AM -- Test append with retain when auto memory is on. */
int TestStringAppendAM(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    RedisModule_AutoMemory(ctx);
    RedisModuleString *s = RedisModule_CreateString(ctx,"foo",3);
    RedisModule_RetainString(ctx,s);
    RedisModule_StringAppendBuffer(ctx,s,"bar",3);
    RedisModule_ReplyWithString(ctx,s);
    RedisModule_FreeString(ctx,s);
    return REDISMODULE_OK;
}

/* TEST.STRING.PRINTF -- Test string formatting. */
int TestStringPrintf(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);
    if (argc < 3) {
        return RedisModule_WrongArity(ctx);
    }
    RedisModuleString *s = RedisModule_CreateStringPrintf(ctx,
        "Got %d args. argv[1]: %s, argv[2]: %s",
        argc,
        RedisModule_StringPtrLen(argv[1], NULL),
        RedisModule_StringPtrLen(argv[2], NULL)
    );

    RedisModule_ReplyWithString(ctx,s);

    return REDISMODULE_OK;
}

int failTest(RedisModuleCtx *ctx, const char *msg) {
    RedisModule_ReplyWithError(ctx, msg);
    return REDISMODULE_ERR;
}

int TestUnlink(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    RedisModuleKey *k = RedisModule_OpenKey(ctx, RedisModule_CreateStringPrintf(ctx, "unlinked"), REDISMODULE_WRITE | REDISMODULE_READ);
    if (!k) return failTest(ctx, "Could not create key");

    if (REDISMODULE_ERR == RedisModule_StringSet(k, RedisModule_CreateStringPrintf(ctx, "Foobar"))) {
        return failTest(ctx, "Could not set string value");
    }

    RedisModuleCallReply *rep = RedisModule_Call(ctx, "EXISTS", "c", "unlinked");
    if (!rep || RedisModule_CallReplyInteger(rep) != 1) {
        return failTest(ctx, "Key does not exist before unlink");
    }

    if (REDISMODULE_ERR == RedisModule_UnlinkKey(k)) {
        return failTest(ctx, "Could not unlink key");
    }

    rep = RedisModule_Call(ctx, "EXISTS", "c", "unlinked");
    if (!rep || RedisModule_CallReplyInteger(rep) != 0) {
        return failTest(ctx, "Could not verify key to be unlinked");
    }
    return RedisModule_ReplyWithSimpleString(ctx, "OK");

}

int NotifyCallback(RedisModuleCtx *ctx, int type, const char *event,
                   RedisModuleString *key) {
  /* Increment a counter on the notifications: for each key notified we
   * increment a counter */
  RedisModule_Log(ctx, "notice", "Got event type %d, event %s, key %s", type,
                  event, RedisModule_StringPtrLen(key, NULL));

  RedisModule_Call(ctx, "HINCRBY", "csc", "notifications", key, "1");
  return REDISMODULE_OK;
}

/* TEST.NOTIFICATIONS -- Test Keyspace Notifications. */
int TestNotifications(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

#define FAIL(msg, ...)                                                                       \
    {                                                                                        \
        RedisModule_Log(ctx, "warning", "Failed NOTIFY Test. Reason: " #msg, ##__VA_ARGS__); \
        goto err;                                                                            \
    }
    RedisModule_Call(ctx, "FLUSHDB", "");

    RedisModule_Call(ctx, "SET", "cc", "foo", "bar");
    RedisModule_Call(ctx, "SET", "cc", "foo", "baz");
    RedisModule_Call(ctx, "SADD", "cc", "bar", "x");
    RedisModule_Call(ctx, "SADD", "cc", "bar", "y");

    RedisModule_Call(ctx, "HSET", "ccc", "baz", "x", "y");
    /* LPUSH should be ignored and not increment any counters */
    RedisModule_Call(ctx, "LPUSH", "cc", "l", "y");
    RedisModule_Call(ctx, "LPUSH", "cc", "l", "y");

    /* Miss some keys intentionally so we will get a "keymiss" notification. */
    RedisModule_Call(ctx, "GET", "c", "nosuchkey");
    RedisModule_Call(ctx, "SMEMBERS", "c", "nosuchkey");

    size_t sz;
    const char *rep;
    RedisModuleCallReply *r = RedisModule_Call(ctx, "HGET", "cc", "notifications", "foo");
    if (r == NULL || RedisModule_CallReplyType(r) != REDISMODULE_REPLY_STRING) {
        FAIL("Wrong or no reply for foo");
    } else {
        rep = RedisModule_CallReplyStringPtr(r, &sz);
        if (sz != 1 || *rep != '2') {
            FAIL("Got reply '%s'. expected '2'", RedisModule_CallReplyStringPtr(r, NULL));
        }
    }

    r = RedisModule_Call(ctx, "HGET", "cc", "notifications", "bar");
    if (r == NULL || RedisModule_CallReplyType(r) != REDISMODULE_REPLY_STRING) {
        FAIL("Wrong or no reply for bar");
    } else {
        rep = RedisModule_CallReplyStringPtr(r, &sz);
        if (sz != 1 || *rep != '2') {
            FAIL("Got reply '%s'. expected '2'", rep);
        }
    }

    r = RedisModule_Call(ctx, "HGET", "cc", "notifications", "baz");
    if (r == NULL || RedisModule_CallReplyType(r) != REDISMODULE_REPLY_STRING) {
        FAIL("Wrong or no reply for baz");
    } else {
        rep = RedisModule_CallReplyStringPtr(r, &sz);
        if (sz != 1 || *rep != '1') {
            FAIL("Got reply '%.*s'. expected '1'", (int)sz, rep);
        }
    }
    /* For l we expect nothing since we didn't subscribe to list events */
    r = RedisModule_Call(ctx, "HGET", "cc", "notifications", "l");
    if (r == NULL || RedisModule_CallReplyType(r) != REDISMODULE_REPLY_NULL) {
        FAIL("Wrong reply for l");
    }

    r = RedisModule_Call(ctx, "HGET", "cc", "notifications", "nosuchkey");
    if (r == NULL || RedisModule_CallReplyType(r) != REDISMODULE_REPLY_STRING) {
        FAIL("Wrong or no reply for nosuchkey");
    } else {
        rep = RedisModule_CallReplyStringPtr(r, &sz);
        if (sz != 1 || *rep != '2') {
            FAIL("Got reply '%.*s'. expected '2'", (int)sz, rep);
        }
    }

    RedisModule_Call(ctx, "FLUSHDB", "");

    return RedisModule_ReplyWithSimpleString(ctx, "OK");
err:
    RedisModule_Call(ctx, "FLUSHDB", "");

    return RedisModule_ReplyWithSimpleString(ctx, "ERR");
}

/* TEST.CTXFLAGS -- Test GetContextFlags. */
int TestCtxFlags(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argc);
    REDISMODULE_NOT_USED(argv);

    RedisModule_AutoMemory(ctx);

    int ok = 1;
    const char *errString = NULL;
#undef FAIL
#define FAIL(msg)        \
    {                    \
        ok = 0;          \
        errString = msg; \
        goto end;        \
    }

    int flags = RedisModule_GetContextFlags(ctx);
    if (flags == 0) {
        FAIL("Got no flags");
    }

    if (flags & REDISMODULE_CTX_FLAGS_LUA) FAIL("Lua flag was set");
    if (flags & REDISMODULE_CTX_FLAGS_MULTI) FAIL("Multi flag was set");

    if (flags & REDISMODULE_CTX_FLAGS_AOF) FAIL("AOF Flag was set")
    /* Enable AOF to test AOF flags */
    RedisModule_Call(ctx, "config", "ccc", "set", "appendonly", "yes");
    flags = RedisModule_GetContextFlags(ctx);
    if (!(flags & REDISMODULE_CTX_FLAGS_AOF)) FAIL("AOF Flag not set after config set");

    if (flags & REDISMODULE_CTX_FLAGS_RDB) FAIL("RDB Flag was set");
    /* Enable RDB to test RDB flags */
    RedisModule_Call(ctx, "config", "ccc", "set", "save", "900 1");
    flags = RedisModule_GetContextFlags(ctx);
    if (!(flags & REDISMODULE_CTX_FLAGS_RDB)) FAIL("RDB Flag was not set after config set");

    if (!(flags & REDISMODULE_CTX_FLAGS_MASTER)) FAIL("Master flag was not set");
    if (flags & REDISMODULE_CTX_FLAGS_SLAVE) FAIL("Slave flag was set");
    if (flags & REDISMODULE_CTX_FLAGS_READONLY) FAIL("Read-only flag was set");
    if (flags & REDISMODULE_CTX_FLAGS_CLUSTER) FAIL("Cluster flag was set");

    if (flags & REDISMODULE_CTX_FLAGS_MAXMEMORY) FAIL("Maxmemory flag was set");

    RedisModule_Call(ctx, "config", "ccc", "set", "maxmemory", "100000000");
    flags = RedisModule_GetContextFlags(ctx);
    if (!(flags & REDISMODULE_CTX_FLAGS_MAXMEMORY))
        FAIL("Maxmemory flag was not set after config set");

    if (flags & REDISMODULE_CTX_FLAGS_EVICT) FAIL("Eviction flag was set");
    RedisModule_Call(ctx, "config", "ccc", "set", "maxmemory-policy", "allkeys-lru");
    flags = RedisModule_GetContextFlags(ctx);
    if (!(flags & REDISMODULE_CTX_FLAGS_EVICT)) FAIL("Eviction flag was not set after config set");

end:
    /* Revert config changes */
    RedisModule_Call(ctx, "config", "ccc", "set", "appendonly", "no");
    RedisModule_Call(ctx, "config", "ccc", "set", "save", "");
    RedisModule_Call(ctx, "config", "ccc", "set", "maxmemory", "0");
    RedisModule_Call(ctx, "config", "ccc", "set", "maxmemory-policy", "noeviction");

    if (!ok) {
        RedisModule_Log(ctx, "warning", "Failed CTXFLAGS Test. Reason: %s", errString);
        return RedisModule_ReplyWithSimpleString(ctx, "ERR");
    }

    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

/* ----------------------------- Test framework ----------------------------- */

/* Return 1 if the reply matches the specified string, otherwise log errors
 * in the server log and return 0. */
int TestAssertStringReply(RedisModuleCtx *ctx, RedisModuleCallReply *reply, char *str, size_t len) {
    RedisModuleString *mystr, *expected;

    if (RedisModule_CallReplyType(reply) != REDISMODULE_REPLY_STRING) {
        RedisModule_Log(ctx,"warning","Unexpected reply type %d",
            RedisModule_CallReplyType(reply));
        return 0;
    }
    mystr = RedisModule_CreateStringFromCallReply(reply);
    expected = RedisModule_CreateString(ctx,str,len);
    if (RedisModule_StringCompare(mystr,expected) != 0) {
        const char *mystr_ptr = RedisModule_StringPtrLen(mystr,NULL);
        const char *expected_ptr = RedisModule_StringPtrLen(expected,NULL);
        RedisModule_Log(ctx,"warning",
            "Unexpected string reply '%s' (instead of '%s')",
            mystr_ptr, expected_ptr);
        return 0;
    }
    return 1;
}

/* Return 1 if the reply matches the specified integer, otherwise log errors
 * in the server log and return 0. */
int TestAssertIntegerReply(RedisModuleCtx *ctx, RedisModuleCallReply *reply, long long expected) {
    if (RedisModule_CallReplyType(reply) != REDISMODULE_REPLY_INTEGER) {
        RedisModule_Log(ctx,"warning","Unexpected reply type %d",
            RedisModule_CallReplyType(reply));
        return 0;
    }
    long long val = RedisModule_CallReplyInteger(reply);
    if (val != expected) {
        RedisModule_Log(ctx,"warning",
            "Unexpected integer reply '%lld' (instead of '%lld')",
            val, expected);
        return 0;
    }
    return 1;
}

#define T(name,...) \
    do { \
        RedisModule_Log(ctx,"warning","Testing %s", name); \
        reply = RedisModule_Call(ctx,name,__VA_ARGS__); \
    } while (0)

/* TEST.IT -- Run all the tests. */
int TestIt(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    RedisModule_AutoMemory(ctx);
    RedisModuleCallReply *reply;

    /* Make sure the DB is empty before to proceed. */
    T("dbsize","");
    if (!TestAssertIntegerReply(ctx,reply,0)) goto fail;

    T("ping","");
    if (!TestAssertStringReply(ctx,reply,"PONG",4)) goto fail;

    T("test.call","");
    if (!TestAssertStringReply(ctx,reply,"OK",2)) goto fail;

    T("test.ctxflags","");
    if (!TestAssertStringReply(ctx,reply,"OK",2)) goto fail;

    T("test.string.append","");
    if (!TestAssertStringReply(ctx,reply,"foobar",6)) goto fail;

    T("test.unlink","");
    if (!TestAssertStringReply(ctx,reply,"OK",2)) goto fail;

    T("test.string.append.am","");
    if (!TestAssertStringReply(ctx,reply,"foobar",6)) goto fail;

    T("test.string.printf", "cc", "foo", "bar");
    if (!TestAssertStringReply(ctx,reply,"Got 3 args. argv[1]: foo, argv[2]: bar",38)) goto fail;

    T("test.notify", "");
    if (!TestAssertStringReply(ctx,reply,"OK",2)) goto fail;

    RedisModule_ReplyWithSimpleString(ctx,"ALL TESTS PASSED");
    return REDISMODULE_OK;

fail:
    RedisModule_ReplyWithSimpleString(ctx,
        "SOME TEST NOT PASSED! Check server logs");
    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx,"test",1,REDISMODULE_APIVER_1)
        == REDISMODULE_ERR) return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"test.call",
        TestCall,"write deny-oom",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"test.string.append",
        TestStringAppend,"write deny-oom",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"test.string.append.am",
        TestStringAppendAM,"write deny-oom",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"test.string.printf",
        TestStringPrintf,"write deny-oom",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"test.ctxflags",
        TestCtxFlags,"readonly",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"test.unlink",
        TestUnlink,"write deny-oom",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"test.it",
        TestIt,"readonly",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    RedisModule_SubscribeToKeyspaceEvents(ctx,
                                            REDISMODULE_NOTIFY_HASH |
                                            REDISMODULE_NOTIFY_SET |
                                            REDISMODULE_NOTIFY_STRING |
                                            REDISMODULE_NOTIFY_KEY_MISS,
                                        NotifyCallback);
    if (RedisModule_CreateCommand(ctx,"test.notify",
        TestNotifications,"write deny-oom",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}
