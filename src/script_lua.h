/*
 * Copyright (c) 2009-2021, Redis Labs Ltd.
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

#ifndef __SCRIPT_LUA_H_
#define __SCRIPT_LUA_H_

#include "server.h"
#include "script.h"
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

typedef struct luaCtx {
    lua_State *lua; /* The Lua interpreter. We use just one for all clients */
    client *lua_client;   /* The "fake client" to query Redis from Lua */
    char *lua_cur_script; /* SHA1 of the script currently running, or NULL */
    dict *lua_scripts;         /* A dictionary of SHA1 -> Lua scripts */
    unsigned long long lua_scripts_mem;  /* Cached scripts' memory + oh */
    int lua_replicate_commands; /* True if we are doing single commands repl. */
    int lua_write_dirty;
    int lua_random_dirty;
    int lua_multi_emitted;
    int lua_repl;
    int lua_kill;
    monotime lua_time_start;  /* monotonic timer to detect timed-out script */
    mstime_t lua_time_snapshot; /* Snapshot of mstime when script is started */
} luaCtx;

extern luaCtx lctx;

void luaEngineRegisterRedisAPI(lua_State* lua);
void scriptingEnableGlobalsProtection(lua_State *lua);
void luaSetGlobalArray(lua_State *lua, char *var, robj **elev, int elec);
void luaMaskCountHook(lua_State *lua, lua_Debug *ar);
void luaReplyToRedisReply(client *c, lua_State *lua);



#endif /* __SCRIPT_LUA_H_ */
