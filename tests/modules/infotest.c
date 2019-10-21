/* Copyright (c) 2019, Salvatore Sanfilippo <antirez at gmail dot com>
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

#include "redismodule.h"

#include <string.h>

void InfoFunc(RedisModuleInfoCtx *ctx, int for_crash_report) {
    RedisModule_InfoAddSection(ctx, "");
    RedisModule_InfoAddFieldLongLong(ctx, "global", -2);

    RedisModule_InfoAddSection(ctx, "Spanish");
    RedisModule_InfoAddFieldCString(ctx, "uno", "one");
    RedisModule_InfoAddFieldLongLong(ctx, "dos", 2);

    RedisModule_InfoAddSection(ctx, "Italian");
    RedisModule_InfoAddFieldLongLong(ctx, "due", 2);
    RedisModule_InfoAddFieldDouble(ctx, "tre", 3.3);

    RedisModule_InfoAddSection(ctx, "keyspace");
    RedisModule_InfoBeginDictField(ctx, "db0");
    RedisModule_InfoAddFieldLongLong(ctx, "keys", 3);
    RedisModule_InfoAddFieldLongLong(ctx, "expires", 1);
    RedisModule_InfoEndDictField(ctx);

    if (for_crash_report) {
        RedisModule_InfoAddSection(ctx, "Klingon");
        RedisModule_InfoAddFieldCString(ctx, "one", "wa’");
        RedisModule_InfoAddFieldCString(ctx, "two", "cha’");
        RedisModule_InfoAddFieldCString(ctx, "three", "wej");
    }

}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    if (RedisModule_Init(ctx,"infotest",1,REDISMODULE_APIVER_1)
            == REDISMODULE_ERR) return REDISMODULE_ERR;

    if (RedisModule_RegisterInfoFunc(ctx, InfoFunc) == REDISMODULE_ERR) return REDISMODULE_ERR;

    return REDISMODULE_OK;
}
