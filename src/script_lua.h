/*
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of the Redis Source Available License 2.0
 * (RSALv2) or the Server Side Public License v1 (SSPLv1).
 */

#ifndef __SCRIPT_LUA_H_
#define __SCRIPT_LUA_H_

/*
 * script_lua.c unit provides shared functionality between
 * eval.c and function_lua.c. Functionality provided:
 *
 * * Execute Lua code, assuming that the code is located on
 *   the top of the Lua stack. In addition, parsing the execution
 *   result and convert it to the resp and reply ot the client.
 *
 * * Run Redis commands from within the Lua code (Including
 *   parsing the reply and create a Lua object out of it).
 *
 * * Register Redis API to the Lua interpreter. Only shared
 *   API are registered (API that is only relevant on eval.c
 *   (like debugging) are registered on eval.c).
 *
 * Uses script.c for interaction back with Redis.
 */

#include "server.h"
#include "script.h"
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#define REGISTRY_RUN_CTX_NAME "__RUN_CTX__"
#define REGISTRY_SET_GLOBALS_PROTECTION_NAME "__GLOBAL_PROTECTION__"
#define REDIS_API_NAME "redis"

typedef struct errorInfo {
    sds msg;
    sds source;
    sds line;
    int ignore_err_stats_update;
}errorInfo;

void luaRegisterRedisAPI(lua_State* lua);
sds luaGetStringSds(lua_State *lua, int index);
void luaRegisterGlobalProtectionFunction(lua_State *lua);
void luaSetErrorMetatable(lua_State *lua);
void luaSetAllowListProtection(lua_State *lua);
void luaSetTableProtectionRecursively(lua_State *lua);
void luaRegisterLogFunction(lua_State* lua);
void luaRegisterVersion(lua_State* lua);
void luaPushErrorBuff(lua_State *lua, sds err_buff);
void luaPushError(lua_State *lua, const char *error);
int luaError(lua_State *lua);
void luaSaveOnRegistry(lua_State* lua, const char* name, void* ptr);
void* luaGetFromRegistry(lua_State* lua, const char* name);
void luaCallFunction(scriptRunCtx* r_ctx, lua_State *lua, robj** keys, size_t nkeys, robj** args, size_t nargs, int debug_enabled);
void luaExtractErrorInformation(lua_State *lua, errorInfo *err_info);
void luaErrorInformationDiscard(errorInfo *err_info);
unsigned long luaMemory(lua_State *lua);
void luaGC(lua_State *lua, int *gc_count);

#endif /* __SCRIPT_LUA_H_ */
