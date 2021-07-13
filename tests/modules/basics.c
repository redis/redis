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
#include "redismodule.h"
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

int TestCallResp3Attribute(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    RedisModule_AutoMemory(ctx);
    RedisModuleCallReply *reply;

    reply = RedisModule_Call(ctx,"DEBUG","3cc" ,"PROTOCOL", "attrib"); /* 3 stands for resp 3 reply */
    if (RedisModule_CallReplyType(reply) != REDISMODULE_REPLY_STRING) goto fail;

    /* make sure we can not reply to resp2 client with resp3 (it might be a string but it contains attribute) */
    if (RedisModule_ReplyWithCallReply(ctx, reply) != REDISMODULE_ERR) goto fail;

    if (!TestMatchReply(reply,"Some real reply following the attribute")) goto fail;

    reply = RedisModule_CallReplyAttribute(reply);
    if (!reply || RedisModule_CallReplyType(reply) != REDISMODULE_REPLY_ATTRIBUTE) goto fail;
    /* make sure we can not reply to resp2 client with resp3 attribute */
    if (RedisModule_ReplyWithCallReply(ctx, reply) != REDISMODULE_ERR) goto fail;
    if (RedisModule_CallReplyLength(reply) != 1) goto fail;

    RedisModuleCallReply *key, *val;
    if (RedisModule_CallReplyAttributeElement(reply,0,&key,&val) != REDISMODULE_OK) goto fail;
    if (!TestMatchReply(key,"key-popularity")) goto fail;
    if (RedisModule_CallReplyType(val) != REDISMODULE_REPLY_ARRAY) goto fail;
    if (RedisModule_CallReplyLength(val) != 2) goto fail;
    if (!TestMatchReply(RedisModule_CallReplyArrayElement(val, 0),"key:123")) goto fail;
    if (!TestMatchReply(RedisModule_CallReplyArrayElement(val, 1),"90")) goto fail;

    RedisModule_ReplyWithSimpleString(ctx,"OK");
    return REDISMODULE_OK;

fail:
    RedisModule_ReplyWithSimpleString(ctx,"ERR");
    return REDISMODULE_OK;
}

int TestGetResp(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    int flags = RedisModule_GetContextFlags(ctx);

    if (flags & REDISMODULE_CTX_FLAGS_RESP3) {
        RedisModule_ReplyWithLongLong(ctx, 3);
    } else {
        RedisModule_ReplyWithLongLong(ctx, 2);
    }

    return REDISMODULE_OK;
}

int TestCallRespAutoMode(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    RedisModule_AutoMemory(ctx);
    RedisModuleCallReply *reply;

    RedisModule_Call(ctx,"DEL","c","myhash");
    RedisModule_Call(ctx,"HSET","ccccc","myhash", "f1", "v1", "f2", "v2");
    /* 0 stands for auto mode, we will get the reply in the same format as the client */
    reply = RedisModule_Call(ctx,"HGETALL","0c" ,"myhash");
    RedisModule_ReplyWithCallReply(ctx, reply);
    return REDISMODULE_OK;
}

int TestCallResp3Map(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    RedisModule_AutoMemory(ctx);
    RedisModuleCallReply *reply;

    RedisModule_Call(ctx,"DEL","c","myhash");
    RedisModule_Call(ctx,"HSET","ccccc","myhash", "f1", "v1", "f2", "v2");
    reply = RedisModule_Call(ctx,"HGETALL","3c" ,"myhash"); /* 3 stands for resp 3 reply */
    if (RedisModule_CallReplyType(reply) != REDISMODULE_REPLY_MAP) goto fail;

    /* make sure we can not reply to resp2 client with resp3 map */
    if (RedisModule_ReplyWithCallReply(ctx, reply) != REDISMODULE_ERR) goto fail;

    long long items = RedisModule_CallReplyLength(reply);
    if (items != 2) goto fail;

    RedisModuleCallReply *key0, *key1;
    RedisModuleCallReply *val0, *val1;
    if (RedisModule_CallReplyMapElement(reply,0,&key0,&val0) != REDISMODULE_OK) goto fail;
    if (RedisModule_CallReplyMapElement(reply,1,&key1,&val1) != REDISMODULE_OK) goto fail;
    if (!TestMatchReply(key0,"f1")) goto fail;
    if (!TestMatchReply(key1,"f2")) goto fail;
    if (!TestMatchReply(val0,"v1")) goto fail;
    if (!TestMatchReply(val1,"v2")) goto fail;

    RedisModule_ReplyWithSimpleString(ctx,"OK");
    return REDISMODULE_OK;

fail:
    RedisModule_ReplyWithSimpleString(ctx,"ERR");
    return REDISMODULE_OK;
}

int TestCallResp3Bool(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    RedisModule_AutoMemory(ctx);
    RedisModuleCallReply *reply;

    reply = RedisModule_Call(ctx,"DEBUG","3cc" ,"PROTOCOL", "true"); /* 3 stands for resp 3 reply */
    if (RedisModule_CallReplyType(reply) != REDISMODULE_REPLY_BOOL) goto fail;
    /* make sure we can not reply to resp2 client with resp3 bool */
    if (RedisModule_ReplyWithCallReply(ctx, reply) != REDISMODULE_ERR) goto fail;

    if (!RedisModule_CallReplyBool(reply)) goto fail;
    reply = RedisModule_Call(ctx,"DEBUG","3cc" ,"PROTOCOL", "false"); /* 3 stands for resp 3 reply */
    if (RedisModule_CallReplyType(reply) != REDISMODULE_REPLY_BOOL) goto fail;
    if (RedisModule_CallReplyBool(reply)) goto fail;

    RedisModule_ReplyWithSimpleString(ctx,"OK");
    return REDISMODULE_OK;

fail:
    RedisModule_ReplyWithSimpleString(ctx,"ERR");
    return REDISMODULE_OK;
}

int TestCallResp3Null(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    RedisModule_AutoMemory(ctx);
    RedisModuleCallReply *reply;

    reply = RedisModule_Call(ctx,"DEBUG","3cc" ,"PROTOCOL", "null"); /* 3 stands for resp 3 reply */
    if (RedisModule_CallReplyType(reply) != REDISMODULE_REPLY_NULL) goto fail;

    /* make sure we can not reply to resp2 client with resp3 null */
    if (RedisModule_ReplyWithCallReply(ctx, reply) != REDISMODULE_ERR) goto fail;

    RedisModule_ReplyWithSimpleString(ctx,"OK");
    return REDISMODULE_OK;

fail:
    RedisModule_ReplyWithSimpleString(ctx,"ERR");
    return REDISMODULE_OK;
}

int TestCallReplyWithNestedReply(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    RedisModule_AutoMemory(ctx);
    RedisModuleCallReply *reply;

    RedisModule_Call(ctx,"DEL","c","mylist");
    RedisModule_Call(ctx,"RPUSH","ccl","mylist","test",(long long)1234);
    reply = RedisModule_Call(ctx,"LRANGE","ccc","mylist","0","-1");
    if (RedisModule_CallReplyType(reply) != REDISMODULE_REPLY_ARRAY) goto fail;
    if (RedisModule_CallReplyLength(reply) < 1) goto fail;
    RedisModuleCallReply *nestedReply = RedisModule_CallReplyArrayElement(reply, 0);

    RedisModule_ReplyWithCallReply(ctx,nestedReply);
    return REDISMODULE_OK;

fail:
    RedisModule_ReplyWithSimpleString(ctx,"ERR");
    return REDISMODULE_OK;
}

int TestCallReplyWithArrayReply(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    RedisModule_AutoMemory(ctx);
    RedisModuleCallReply *reply;

    RedisModule_Call(ctx,"DEL","c","mylist");
    RedisModule_Call(ctx,"RPUSH","ccl","mylist","test",(long long)1234);
    reply = RedisModule_Call(ctx,"LRANGE","ccc","mylist","0","-1");
    if (RedisModule_CallReplyType(reply) != REDISMODULE_REPLY_ARRAY) goto fail;

    RedisModule_ReplyWithCallReply(ctx,reply);
    return REDISMODULE_OK;

fail:
    RedisModule_ReplyWithSimpleString(ctx,"ERR");
    return REDISMODULE_OK;
}

int TestCallResp3Double(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    RedisModule_AutoMemory(ctx);
    RedisModuleCallReply *reply;

    reply = RedisModule_Call(ctx,"DEBUG","3cc" ,"PROTOCOL", "double"); /* 3 stands for resp 3 reply */
    if (RedisModule_CallReplyType(reply) != REDISMODULE_REPLY_DOUBLE) goto fail;

    /* make sure we can not reply to resp2 client with resp3 double*/
    if (RedisModule_ReplyWithCallReply(ctx, reply) != REDISMODULE_ERR) goto fail;

    double d = RedisModule_CallReplyDouble(reply);
    if (d != 3.1415926535900001) goto fail;
    RedisModule_ReplyWithSimpleString(ctx,"OK");
    return REDISMODULE_OK;

fail:
    RedisModule_ReplyWithSimpleString(ctx,"ERR");
    return REDISMODULE_OK;
}

int TestCallResp3BigNumber(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    RedisModule_AutoMemory(ctx);
    RedisModuleCallReply *reply;

    reply = RedisModule_Call(ctx,"DEBUG","3cc" ,"PROTOCOL", "bignum"); /* 3 stands for resp 3 reply */
    if (RedisModule_CallReplyType(reply) != REDISMODULE_REPLY_BIG_NUMBER) goto fail;

    /* make sure we can not reply to resp2 client with resp3 big number */
    if (RedisModule_ReplyWithCallReply(ctx, reply) != REDISMODULE_ERR) goto fail;

    size_t len;
    const char* big_num = RedisModule_CallReplyBigNumber(reply, &len);
    RedisModule_ReplyWithStringBuffer(ctx,big_num,len);
    return REDISMODULE_OK;

fail:
    RedisModule_ReplyWithSimpleString(ctx,"ERR");
    return REDISMODULE_OK;
}

int TestCallResp3Verbatim(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    RedisModule_AutoMemory(ctx);
    RedisModuleCallReply *reply;

    reply = RedisModule_Call(ctx,"DEBUG","3cc" ,"PROTOCOL", "verbatim"); /* 3 stands for resp 3 reply */
    if (RedisModule_CallReplyType(reply) != REDISMODULE_REPLY_VERBATIM_STRING) goto fail;

    /* make sure we can not reply to resp2 client with resp3 verbatim string */
    if (RedisModule_ReplyWithCallReply(ctx, reply) != REDISMODULE_ERR) goto fail;

    const char* format;
    size_t len;
    const char* str = RedisModule_CallReplyVerbatim(reply, &len, &format);
    RedisModuleString *s = RedisModule_CreateStringPrintf(ctx, "%.*s:%.*s", 3, format, (int)len, str);
    RedisModule_ReplyWithString(ctx,s);
    return REDISMODULE_OK;

fail:
    RedisModule_ReplyWithSimpleString(ctx,"ERR");
    return REDISMODULE_OK;
}

int TestCallResp3Set(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    RedisModule_AutoMemory(ctx);
    RedisModuleCallReply *reply;

    RedisModule_Call(ctx,"DEL","c","myset");
    RedisModule_Call(ctx,"sadd","ccc","myset", "v1", "v2");
    reply = RedisModule_Call(ctx,"smembers","3c" ,"myset"); // N stands for resp 3 reply
    if (RedisModule_CallReplyType(reply) != REDISMODULE_REPLY_SET) goto fail;

    /* make sure we can not reply to resp2 client with resp3 set */
    if (RedisModule_ReplyWithCallReply(ctx, reply) != REDISMODULE_ERR) goto fail;

    long long items = RedisModule_CallReplyLength(reply);
    if (items != 2) goto fail;

    RedisModuleCallReply *val0, *val1;

    val0 = RedisModule_CallReplySetElement(reply,0);
    val1 = RedisModule_CallReplySetElement(reply,1);

    /*
     * The order of elements on sets are not promised so we just
     * veridy that the reply matches one of the elements.
     */
    if (!TestMatchReply(val0,"v1") && !TestMatchReply(val0,"v2")) goto fail;
    if (!TestMatchReply(val1,"v1") && !TestMatchReply(val1,"v2")) goto fail;

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

/* TEST.STRING.TRUNCATE -- Test truncating an existing string object. */
int TestStringTruncate(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    RedisModule_Call(ctx, "SET", "cc", "foo", "abcde");
    RedisModuleKey *k = RedisModule_OpenKey(ctx, RedisModule_CreateStringPrintf(ctx, "foo"), REDISMODULE_READ | REDISMODULE_WRITE);
    if (!k) return failTest(ctx, "Could not create key");

    size_t len = 0;
    char* s;

    /* expand from 5 to 8 and check null pad */
    if (REDISMODULE_ERR == RedisModule_StringTruncate(k, 8)) {
        return failTest(ctx, "Could not truncate string value (8)");
    }
    s = RedisModule_StringDMA(k, &len, REDISMODULE_READ);
    if (!s) {
        return failTest(ctx, "Failed to read truncated string (8)");
    } else if (len != 8) {
        return failTest(ctx, "Failed to expand string value (8)");
    } else if (0 != strncmp(s, "abcde\0\0\0", 8)) {
        return failTest(ctx, "Failed to null pad string value (8)");
    }

    /* shrink from 8 to 4 */
    if (REDISMODULE_ERR == RedisModule_StringTruncate(k, 4)) {
        return failTest(ctx, "Could not truncate string value (4)");
    }
    s = RedisModule_StringDMA(k, &len, REDISMODULE_READ);
    if (!s) {
        return failTest(ctx, "Failed to read truncated string (4)");
    } else if (len != 4) {
        return failTest(ctx, "Failed to shrink string value (4)");
    } else if (0 != strncmp(s, "abcd", 4)) {
        return failTest(ctx, "Failed to truncate string value (4)");
    }

    /* shrink to 0 */
    if (REDISMODULE_ERR == RedisModule_StringTruncate(k, 0)) {
        return failTest(ctx, "Could not truncate string value (0)");
    }
    s = RedisModule_StringDMA(k, &len, REDISMODULE_READ);
    if (!s) {
        return failTest(ctx, "Failed to read truncated string (0)");
    } else if (len != 0) {
        return failTest(ctx, "Failed to shrink string value to (0)");
    }

    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

int NotifyCallback(RedisModuleCtx *ctx, int type, const char *event,
                   RedisModuleString *key) {
  RedisModule_AutoMemory(ctx);
  /* Increment a counter on the notifications: for each key notified we
   * increment a counter */
  RedisModule_Log(ctx, "notice", "Got event type %d, event %s, key %s", type,
                  event, RedisModule_StringPtrLen(key, NULL));

  RedisModule_Call(ctx, "HINCRBY", "csc", "notifications", key, "1");
  return REDISMODULE_OK;
}

/* TEST.NOTIFICATIONS -- Test Keyspace Notifications. */
int TestNotifications(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);
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

    /* Disable RDB saving and test the flag. */
    RedisModule_Call(ctx, "config", "ccc", "set", "save", "");
    flags = RedisModule_GetContextFlags(ctx);
    if (flags & REDISMODULE_CTX_FLAGS_RDB) FAIL("RDB Flag was set");
    /* Enable RDB to test RDB flags */
    RedisModule_Call(ctx, "config", "ccc", "set", "save", "900 1");
    flags = RedisModule_GetContextFlags(ctx);
    if (!(flags & REDISMODULE_CTX_FLAGS_RDB)) FAIL("RDB Flag was not set after config set");

    if (!(flags & REDISMODULE_CTX_FLAGS_MASTER)) FAIL("Master flag was not set");
    if (flags & REDISMODULE_CTX_FLAGS_SLAVE) FAIL("Slave flag was set");
    if (flags & REDISMODULE_CTX_FLAGS_READONLY) FAIL("Read-only flag was set");
    if (flags & REDISMODULE_CTX_FLAGS_CLUSTER) FAIL("Cluster flag was set");

    /* Disable maxmemory and test the flag. (it is implicitly set in 32bit builds. */
    RedisModule_Call(ctx, "config", "ccc", "set", "maxmemory", "0");
    flags = RedisModule_GetContextFlags(ctx);
    if (flags & REDISMODULE_CTX_FLAGS_MAXMEMORY) FAIL("Maxmemory flag was set");

    /* Enable maxmemory and test the flag. */
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

    if (RedisModule_CallReplyType(reply) == REDISMODULE_REPLY_ERROR) {
        RedisModule_Log(ctx,"warning","Test error reply: %s",
            RedisModule_CallReplyStringPtr(reply, NULL));
        return 0;
    } else if (RedisModule_CallReplyType(reply) != REDISMODULE_REPLY_STRING) {
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
    if (RedisModule_CallReplyType(reply) == REDISMODULE_REPLY_ERROR) {
        RedisModule_Log(ctx,"warning","Test error reply: %s",
            RedisModule_CallReplyStringPtr(reply, NULL));
        return 0;
    } else if (RedisModule_CallReplyType(reply) != REDISMODULE_REPLY_INTEGER) {
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

/* TEST.BASICS -- Run all the tests.
 * Note: it is useful to run these tests from the module rather than TCL
 * since it's easier to check the reply types like that (make a distinction
 * between 0 and "0", etc. */
int TestBasics(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
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

    T("test.callresp3map","");
    if (!TestAssertStringReply(ctx,reply,"OK",2)) goto fail;

    T("test.callresp3set","");
    if (!TestAssertStringReply(ctx,reply,"OK",2)) goto fail;

    T("test.callresp3double","");
    if (!TestAssertStringReply(ctx,reply,"OK",2)) goto fail;

    T("test.callresp3bool","");
    if (!TestAssertStringReply(ctx,reply,"OK",2)) goto fail;

    T("test.callresp3null","");
    if (!TestAssertStringReply(ctx,reply,"OK",2)) goto fail;

    T("test.callreplywithnestedreply","");
    if (!TestAssertStringReply(ctx,reply,"test",4)) goto fail;

    T("test.callreplywithbignumberreply","");
    if (!TestAssertStringReply(ctx,reply,"1234567999999999999999999999999999999",37)) goto fail;

    T("test.callreplywithverbatimstringreply","");
    if (!TestAssertStringReply(ctx,reply,"txt:This is a verbatim\nstring",29)) goto fail;

    T("test.ctxflags","");
    if (!TestAssertStringReply(ctx,reply,"OK",2)) goto fail;

    T("test.string.append","");
    if (!TestAssertStringReply(ctx,reply,"foobar",6)) goto fail;

    T("test.string.truncate","");
    if (!TestAssertStringReply(ctx,reply,"OK",2)) goto fail;

    T("test.unlink","");
    if (!TestAssertStringReply(ctx,reply,"OK",2)) goto fail;

    T("test.string.append.am","");
    if (!TestAssertStringReply(ctx,reply,"foobar",6)) goto fail;

    T("test.string.printf", "cc", "foo", "bar");
    if (!TestAssertStringReply(ctx,reply,"Got 3 args. argv[1]: foo, argv[2]: bar",38)) goto fail;

    T("test.notify", "");
    if (!TestAssertStringReply(ctx,reply,"OK",2)) goto fail;

    T("test.callreplywitharrayreply", "");
    if (RedisModule_CallReplyType(reply) != REDISMODULE_REPLY_ARRAY) goto fail;
    if (RedisModule_CallReplyLength(reply) != 2) goto fail;
    if (!TestAssertStringReply(ctx,RedisModule_CallReplyArrayElement(reply, 0),"test",4)) goto fail;
    if (!TestAssertStringReply(ctx,RedisModule_CallReplyArrayElement(reply, 1),"1234",4)) goto fail;

    RedisModule_ReplyWithSimpleString(ctx,"ALL TESTS PASSED");
    return REDISMODULE_OK;

fail:
    RedisModule_ReplyWithSimpleString(ctx,
        "SOME TEST DID NOT PASS! Check server logs");
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

    if (RedisModule_CreateCommand(ctx,"test.callresp3map",
        TestCallResp3Map,"write deny-oom",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"test.callresp3attribute",
        TestCallResp3Attribute,"write deny-oom",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"test.callresp3set",
        TestCallResp3Set,"write deny-oom",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"test.callresp3double",
        TestCallResp3Double,"write deny-oom",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"test.callresp3bool",
        TestCallResp3Bool,"write deny-oom",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"test.callresp3null",
        TestCallResp3Null,"write deny-oom",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"test.callreplywitharrayreply",
        TestCallReplyWithArrayReply,"write deny-oom",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"test.callreplywithnestedreply",
        TestCallReplyWithNestedReply,"write deny-oom",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"test.callreplywithbignumberreply",
        TestCallResp3BigNumber,"write deny-oom",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"test.callreplywithverbatimstringreply",
        TestCallResp3Verbatim,"write deny-oom",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"test.string.append",
        TestStringAppend,"write deny-oom",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"test.string.append.am",
        TestStringAppendAM,"write deny-oom",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"test.string.truncate",
        TestStringTruncate,"write deny-oom",1,1,1) == REDISMODULE_ERR)
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

    if (RedisModule_CreateCommand(ctx,"test.basics",
        TestBasics,"readonly",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    /* the following commands are used by an external test and should not be added to TestBasics */
    if (RedisModule_CreateCommand(ctx,"test.rmcallautomode",
        TestCallRespAutoMode,"readonly",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"test.getresp",
        TestGetResp,"readonly",1,1,1) == REDISMODULE_ERR)
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
