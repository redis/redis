/*
 * Copyright (c) 2021, Redis Ltd.
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

/*
 * function_lua.c unit provides the Lua engine functionality.
 * Including registering the engine and implementing the engine
 * callbacks:
 * * Create a function from blob (usually text)
 * * Invoke a function
 * * Free function memory
 * * Get memory usage
 *
 * Uses script_lua.c to run the Lua code.
 */

#include "functions.h"
#include "script_lua.h"
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#define LUA_ENGINE_NAME "LUA"
#define REGISTRY_ENGINE_CTX_NAME "__ENGINE_CTX__"
#define REGISTRY_ERROR_HANDLER_NAME "__ERROR_HANDLER__"
#define REGISTRY_LOAD_CTX_NAME "__LIBRARY_CTX__"
#define LIBRARY_API_NAME "__LIBRARY_API__"
#define LOAD_TIMEOUT_MS 500

/* Lua engine ctx */
typedef struct luaEngineCtx {
    lua_State *lua;
} luaEngineCtx;

/* Lua function ctx */
typedef struct luaFunctionCtx {
    /* Special ID that allows getting the Lua function object from the Lua registry */
    int lua_function_ref;
} luaFunctionCtx;

typedef struct loadCtx {
    functionLibInfo *li;
    monotime start_time;
} loadCtx;

/* Hook for FUNCTION LOAD execution.
 * Used to cancel the execution in case of a timeout (500ms).
 * This execution should be fast and should only register
 * functions so 500ms should be more than enough. */
static void luaEngineLoadHook(lua_State *lua, lua_Debug *ar) {
    UNUSED(ar);
    loadCtx *load_ctx = luaGetFromRegistry(lua, REGISTRY_LOAD_CTX_NAME);
    uint64_t duration = elapsedMs(load_ctx->start_time);
    if (duration > LOAD_TIMEOUT_MS) {
        lua_sethook(lua, luaEngineLoadHook, LUA_MASKLINE, 0);

        lua_pushstring(lua,"FUNCTION LOAD timeout");
        lua_error(lua);
    }
}

/*
 * Compile a given blob and save it on the registry.
 * Return a function ctx with Lua ref that allows to later retrieve the
 * function from the registry.
 *
 * Return NULL on compilation error and set the error to the err variable
 */
static int luaEngineCreate(void *engine_ctx, functionLibInfo *li, sds blob, sds *err) {
    luaEngineCtx *lua_engine_ctx = engine_ctx;
    lua_State *lua = lua_engine_ctx->lua;

    /* Each library will have its own global distinct table.
     * We will create a new fresh Lua table and use
     * lua_setfenv to set the table as the library globals
     * (https://www.lua.org/manual/5.1/manual.html#lua_setfenv)
     *
     * At first, populate this new table with only the 'library' API
     * to make sure only 'library' API is available at start. After the
     * initial run is finished and all functions are registered, add
     * all the default globals to the library global table and delete
     * the library API.
     *
     * There are 2 ways to achieve the last part (add default
     * globals to the new table):
     *
     * 1. Initialize the new table with all the default globals
     * 2. Inheritance using metatable (https://www.lua.org/pil/14.3.html)
     *
     * For now we are choosing the second, we can change it in the future to
     * achieve a better isolation between functions. */
    lua_newtable(lua); /* Global table for the library */
    lua_pushstring(lua, REDIS_API_NAME);
    lua_pushstring(lua, LIBRARY_API_NAME);
    lua_gettable(lua, LUA_REGISTRYINDEX); /* get library function from registry */
    lua_settable(lua, -3); /* push the library table to the new global table */

    /* Set global protection on the new global table */
    luaSetGlobalProtection(lua_engine_ctx->lua);

    /* compile the code */
    if (luaL_loadbuffer(lua, blob, sdslen(blob), "@user_function")) {
        *err = sdscatprintf(sdsempty(), "Error compiling function: %s", lua_tostring(lua, -1));
        lua_pop(lua, 2); /* pops the error and globals table */
        return C_ERR;
    }
    serverAssert(lua_isfunction(lua, -1));

    loadCtx load_ctx = {
        .li = li,
        .start_time = getMonotonicUs(),
    };
    luaSaveOnRegistry(lua, REGISTRY_LOAD_CTX_NAME, &load_ctx);

    /* set the function environment so only 'library' API can be accessed. */
    lua_pushvalue(lua, -2); /* push global table to the front */
    lua_setfenv(lua, -2);

    lua_sethook(lua,luaEngineLoadHook,LUA_MASKCOUNT,100000);
    /* Run the compiled code to allow it to register functions */
    if (lua_pcall(lua,0,0,0)) {
        *err = sdscatprintf(sdsempty(), "Error registering functions: %s", lua_tostring(lua, -1));
        lua_pop(lua, 2); /* pops the error and globals table */
        lua_sethook(lua,NULL,0,0); /* Disable hook */
        luaSaveOnRegistry(lua, REGISTRY_LOAD_CTX_NAME, NULL);
        return C_ERR;
    }
    lua_sethook(lua,NULL,0,0); /* Disable hook */
    luaSaveOnRegistry(lua, REGISTRY_LOAD_CTX_NAME, NULL);

    /* stack contains the global table, lets rearrange it to contains the entire API. */
    /* delete 'redis' API */
    lua_pushstring(lua, REDIS_API_NAME);
    lua_pushnil(lua);
    lua_settable(lua, -3);

    /* create metatable */
    lua_newtable(lua);
    lua_pushstring(lua, "__index");
    lua_pushvalue(lua, LUA_GLOBALSINDEX); /* push original globals */
    lua_settable(lua, -3);
    lua_pushstring(lua, "__newindex");
    lua_pushvalue(lua, LUA_GLOBALSINDEX); /* push original globals */
    lua_settable(lua, -3);

    lua_setmetatable(lua, -2);

    lua_pop(lua, 1); /* pops the global table */

    return C_OK;
}

/*
 * Invole the give function with the given keys and args
 */
static void luaEngineCall(scriptRunCtx *run_ctx,
                          void *engine_ctx,
                          void *compiled_function,
                          robj **keys,
                          size_t nkeys,
                          robj **args,
                          size_t nargs)
{
    luaEngineCtx *lua_engine_ctx = engine_ctx;
    lua_State *lua = lua_engine_ctx->lua;
    luaFunctionCtx *f_ctx = compiled_function;

    /* Push error handler */
    lua_pushstring(lua, REGISTRY_ERROR_HANDLER_NAME);
    lua_gettable(lua, LUA_REGISTRYINDEX);

    lua_rawgeti(lua, LUA_REGISTRYINDEX, f_ctx->lua_function_ref);

    serverAssert(lua_isfunction(lua, -1));

    luaCallFunction(run_ctx, lua, keys, nkeys, args, nargs, 0);
    lua_pop(lua, 1); /* Pop error handler */
}

static size_t luaEngineGetUsedMemoy(void *engine_ctx) {
    luaEngineCtx *lua_engine_ctx = engine_ctx;
    return luaMemory(lua_engine_ctx->lua);
}

static size_t luaEngineFunctionMemoryOverhead(void *compiled_function) {
    return zmalloc_size(compiled_function);
}

static size_t luaEngineMemoryOverhead(void *engine_ctx) {
    luaEngineCtx *lua_engine_ctx = engine_ctx;
    return zmalloc_size(lua_engine_ctx);
}

static void luaEngineFreeFunction(void *engine_ctx, void *compiled_function) {
    luaEngineCtx *lua_engine_ctx = engine_ctx;
    lua_State *lua = lua_engine_ctx->lua;
    luaFunctionCtx *f_ctx = compiled_function;
    lua_unref(lua, f_ctx->lua_function_ref);
    zfree(f_ctx);
}

static int luaRegisterFunction(lua_State *lua) {
    int argc = lua_gettop(lua);
    if (argc < 2 || argc > 3) {
        luaPushError(lua, "wrong number of arguments to redis.register_function");
        return luaRaiseError(lua);
    }
    loadCtx *load_ctx = luaGetFromRegistry(lua, REGISTRY_LOAD_CTX_NAME);
    if (!load_ctx) {
        luaPushError(lua, "redis.register_function can only be called on FUNCTION LOAD command");
        return luaRaiseError(lua);
    }

    if (!lua_isstring(lua, 1)) {
        luaPushError(lua, "first argument to redis.register_function must be a string");
        return luaRaiseError(lua);
    }

    if (!lua_isfunction(lua, 2)) {
        luaPushError(lua, "second argument to redis.register_function must be a function");
        return luaRaiseError(lua);
    }

    if (argc == 3 && !lua_isstring(lua, 3)) {
        luaPushError(lua, "third argument to redis.register_function must be a string");
        return luaRaiseError(lua);
    }

    size_t function_name_len;
    const char *function_name = lua_tolstring(lua, 1, &function_name_len);
    sds function_name_sds = sdsnewlen(function_name, function_name_len);

    sds desc_sds = NULL;
    if (argc == 3){
        size_t desc_len;
        const char *desc = lua_tolstring(lua, 3, &desc_len);
        desc_sds = sdsnewlen(desc, desc_len);
        lua_pop(lua, 1); /* pop out the description */
    }

    int lua_function_ref = luaL_ref(lua, LUA_REGISTRYINDEX);

    luaFunctionCtx *lua_f_ctx = zmalloc(sizeof(*lua_f_ctx));
    *lua_f_ctx = (luaFunctionCtx ) { .lua_function_ref = lua_function_ref, };

    sds err = NULL;
    if (functionLibCreateFunction(function_name_sds, lua_f_ctx, load_ctx->li, desc_sds, &err) != C_OK) {
        sdsfree(function_name_sds);
        if (desc_sds) sdsfree(desc_sds);
        lua_unref(lua, lua_f_ctx->lua_function_ref);
        zfree(lua_f_ctx);
        luaPushError(lua, err);
        sdsfree(err);
        return luaRaiseError(lua);
    }

    return 0;
}

/* Initialize Lua engine, should be called once on start. */
int luaEngineInitEngine() {
    luaEngineCtx *lua_engine_ctx = zmalloc(sizeof(*lua_engine_ctx));
    lua_engine_ctx->lua = lua_open();

    luaRegisterRedisAPI(lua_engine_ctx->lua);

    /* Register the library commands table and fields and store it to registry */
    lua_pushstring(lua_engine_ctx->lua, LIBRARY_API_NAME);
    lua_newtable(lua_engine_ctx->lua);

    lua_pushstring(lua_engine_ctx->lua, "register_function");
    lua_pushcfunction(lua_engine_ctx->lua, luaRegisterFunction);
    lua_settable(lua_engine_ctx->lua, -3);

    luaRegisterLogFunction(lua_engine_ctx->lua);

    lua_settable(lua_engine_ctx->lua, LUA_REGISTRYINDEX);

    /* Save error handler to registry */
    lua_pushstring(lua_engine_ctx->lua, REGISTRY_ERROR_HANDLER_NAME);
    char *errh_func =       "local dbg = debug\n"
                            "local error_handler = function (err)\n"
                            "  local i = dbg.getinfo(2,'nSl')\n"
                            "  if i and i.what == 'C' then\n"
                            "    i = dbg.getinfo(3,'nSl')\n"
                            "  end\n"
                            "  if i then\n"
                            "    return i.source .. ':' .. i.currentline .. ': ' .. err\n"
                            "  else\n"
                            "    return err\n"
                            "  end\n"
                            "end\n"
                            "return error_handler";
    luaL_loadbuffer(lua_engine_ctx->lua, errh_func, strlen(errh_func), "@err_handler_def");
    lua_pcall(lua_engine_ctx->lua,0,1,0);
    lua_settable(lua_engine_ctx->lua, LUA_REGISTRYINDEX);

    /* Save global protection to registry */
    luaRegisterGlobalProtectionFunction(lua_engine_ctx->lua);

    /* Set global protection on globals */
    lua_pushvalue(lua_engine_ctx->lua, LUA_GLOBALSINDEX);
    luaSetGlobalProtection(lua_engine_ctx->lua);
    lua_pop(lua_engine_ctx->lua, 1);

    /* save the engine_ctx on the registry so we can get it from the Lua interpreter */
    luaSaveOnRegistry(lua_engine_ctx->lua, REGISTRY_ENGINE_CTX_NAME, lua_engine_ctx);

    engine *lua_engine = zmalloc(sizeof(*lua_engine));
    *lua_engine = (engine) {
        .engine_ctx = lua_engine_ctx,
        .create = luaEngineCreate,
        .call = luaEngineCall,
        .get_used_memory = luaEngineGetUsedMemoy,
        .get_function_memory_overhead = luaEngineFunctionMemoryOverhead,
        .get_engine_memory_overhead = luaEngineMemoryOverhead,
        .free_function = luaEngineFreeFunction,
    };
    return functionsRegisterEngine(LUA_ENGINE_NAME, lua_engine);
}
