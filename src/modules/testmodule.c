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

#include "../redismodule.h"

/* ------------------------------- Test units ------------------------------- */

/* TEST.STRING.APPEND -- Test appending to an existing string object. */
int TestStringAppend(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModuleString *s = RedisModule_CreateString(ctx,"foo",3);
    RedisModule_StringAppendBuffer(ctx,s,"bar",3);
    RedisModule_ReplyWithString(ctx,s);
    RedisModule_FreeString(ctx,s);
    return REDISMODULE_OK;
}

/* TEST.STRING.APPEND.AM -- Test append with retain when auto memory is on. */
int TestStringAppendAM(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);
    RedisModuleString *s = RedisModule_CreateString(ctx,"foo",3);
    RedisModule_RetainString(ctx,s);
    RedisModule_StringAppendBuffer(ctx,s,"bar",3);
    RedisModule_ReplyWithString(ctx,s);
    RedisModule_FreeString(ctx,s);
    return REDISMODULE_OK;
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

#define T(name,...) \
    do { \
        RedisModule_Log(ctx,"warning","Testing %s", name); \
        reply = RedisModule_Call(ctx,name,__VA_ARGS__); \
    } while (0);

/* TEST.IT -- Run all the tests. */
int TestIt(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);
    RedisModuleCallReply *reply;

    T("test.string.append","");
    if (!TestAssertStringReply(ctx,reply,"foobar",6)) goto fail;

    T("test.string.append.am","");
    if (!TestAssertStringReply(ctx,reply,"foobar",6)) goto fail;

    RedisModule_ReplyWithSimpleString(ctx,"ALL TESTS PASSED");
    return REDISMODULE_OK;

fail:
    RedisModule_ReplyWithSimpleString(ctx,
        "SOME TEST NOT PASSED! Check server logs");
    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (RedisModule_Init(ctx,"test",1,REDISMODULE_APIVER_1)
        == REDISMODULE_ERR) return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"test.string.append",
        TestStringAppend,"write deny-oom",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"test.string.append.am",
        TestStringAppendAM,"write deny-oom",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"test.it",
        TestIt,"readonly",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}
