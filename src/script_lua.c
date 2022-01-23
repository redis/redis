/*
 * Copyright (c) 2009-2021, Redis Ltd.
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

#include "script_lua.h"

#include "server.h"
#include "sha1.h"
#include "rand.h"
#include "cluster.h"
#include "monotonic.h"
#include "resp_parser.h"
#include "version.h"
#include <lauxlib.h>
#include <lualib.h>
#include <ctype.h>
#include <math.h>

static int redis_math_random (lua_State *L);
static int redis_math_randomseed (lua_State *L);
static void redisProtocolToLuaType_Int(void *ctx, long long val, const char *proto, size_t proto_len);
static void redisProtocolToLuaType_BulkString(void *ctx, const char *str, size_t len, const char *proto, size_t proto_len);
static void redisProtocolToLuaType_NullBulkString(void *ctx, const char *proto, size_t proto_len);
static void redisProtocolToLuaType_NullArray(void *ctx, const char *proto, size_t proto_len);
static void redisProtocolToLuaType_Status(void *ctx, const char *str, size_t len, const char *proto, size_t proto_len);
static void redisProtocolToLuaType_Error(void *ctx, const char *str, size_t len, const char *proto, size_t proto_len);
static void redisProtocolToLuaType_Array(struct ReplyParser *parser, void *ctx, size_t len, const char *proto);
static void redisProtocolToLuaType_Map(struct ReplyParser *parser, void *ctx, size_t len, const char *proto);
static void redisProtocolToLuaType_Set(struct ReplyParser *parser, void *ctx, size_t len, const char *proto);
static void redisProtocolToLuaType_Null(void *ctx, const char *proto, size_t proto_len);
static void redisProtocolToLuaType_Bool(void *ctx, int val, const char *proto, size_t proto_len);
static void redisProtocolToLuaType_Double(void *ctx, double d, const char *proto, size_t proto_len);
static void redisProtocolToLuaType_BigNumber(void *ctx, const char *str, size_t len, const char *proto, size_t proto_len);
static void redisProtocolToLuaType_VerbatimString(void *ctx, const char *format, const char *str, size_t len, const char *proto, size_t proto_len);
static void redisProtocolToLuaType_Attribute(struct ReplyParser *parser, void *ctx, size_t len, const char *proto);
static void luaReplyToRedisReply(client *c, client* script_client, lua_State *lua);

/*
 * Save the give pointer on Lua registry, used to save the Lua context and
 * function context so we can retrieve them from lua_State.
 */
void luaSaveOnRegistry(lua_State* lua, const char* name, void* ptr) {
    lua_pushstring(lua, name);
    if (ptr) {
        lua_pushlightuserdata(lua, ptr);
    } else {
        lua_pushnil(lua);
    }
    lua_settable(lua, LUA_REGISTRYINDEX);
}

/*
 * Get a saved pointer from registry
 */
void* luaGetFromRegistry(lua_State* lua, const char* name) {
    lua_pushstring(lua, name);
    lua_gettable(lua, LUA_REGISTRYINDEX);

    if (lua_isnil(lua, -1)) {
        return NULL;
    }
    /* must be light user data */
    serverAssert(lua_islightuserdata(lua, -1));

    void* ptr = (void*) lua_topointer(lua, -1);
    serverAssert(ptr);

    /* pops the value */
    lua_pop(lua, 1);

    return ptr;
}

/* ---------------------------------------------------------------------------
 * Redis reply to Lua type conversion functions.
 * ------------------------------------------------------------------------- */

/* Take a Redis reply in the Redis protocol format and convert it into a
 * Lua type. Thanks to this function, and the introduction of not connected
 * clients, it is trivial to implement the redis() lua function.
 *
 * Basically we take the arguments, execute the Redis command in the context
 * of a non connected client, then take the generated reply and convert it
 * into a suitable Lua type. With this trick the scripting feature does not
 * need the introduction of a full Redis internals API. The script
 * is like a normal client that bypasses all the slow I/O paths.
 *
 * Note: in this function we do not do any sanity check as the reply is
 * generated by Redis directly. This allows us to go faster.
 *
 * Errors are returned as a table with a single 'err' field set to the
 * error string.
 */

static const ReplyParserCallbacks DefaultLuaTypeParserCallbacks = {
    .null_array_callback = redisProtocolToLuaType_NullArray,
    .bulk_string_callback = redisProtocolToLuaType_BulkString,
    .null_bulk_string_callback = redisProtocolToLuaType_NullBulkString,
    .error_callback = redisProtocolToLuaType_Error,
    .simple_str_callback = redisProtocolToLuaType_Status,
    .long_callback = redisProtocolToLuaType_Int,
    .array_callback = redisProtocolToLuaType_Array,
    .set_callback = redisProtocolToLuaType_Set,
    .map_callback = redisProtocolToLuaType_Map,
    .bool_callback = redisProtocolToLuaType_Bool,
    .double_callback = redisProtocolToLuaType_Double,
    .null_callback = redisProtocolToLuaType_Null,
    .big_number_callback = redisProtocolToLuaType_BigNumber,
    .verbatim_string_callback = redisProtocolToLuaType_VerbatimString,
    .attribute_callback = redisProtocolToLuaType_Attribute,
    .error = NULL,
};

static void redisProtocolToLuaType(lua_State *lua, char* reply) {
    ReplyParser parser = {.curr_location = reply, .callbacks = DefaultLuaTypeParserCallbacks};

    parseReply(&parser, lua);
}

static void redisProtocolToLuaType_Int(void *ctx, long long val, const char *proto, size_t proto_len) {
    UNUSED(proto);
    UNUSED(proto_len);
    if (!ctx) {
        return;
    }

    lua_State *lua = ctx;
    if (!lua_checkstack(lua, 1)) {
        /* Increase the Lua stack if needed, to make sure there is enough room
         * to push elements to the stack. On failure, exit with panic. */
        serverPanic("lua stack limit reach when parsing redis.call reply");
    }
    lua_pushnumber(lua,(lua_Number)val);
}

static void redisProtocolToLuaType_NullBulkString(void *ctx, const char *proto, size_t proto_len) {
    UNUSED(proto);
    UNUSED(proto_len);
    if (!ctx) {
        return;
    }

    lua_State *lua = ctx;
    if (!lua_checkstack(lua, 1)) {
        /* Increase the Lua stack if needed, to make sure there is enough room
         * to push elements to the stack. On failure, exit with panic. */
        serverPanic("lua stack limit reach when parsing redis.call reply");
    }
    lua_pushboolean(lua,0);
}

static void redisProtocolToLuaType_NullArray(void *ctx, const char *proto, size_t proto_len) {
    UNUSED(proto);
    UNUSED(proto_len);
    if (!ctx) {
        return;
    }
    lua_State *lua = ctx;
    if (!lua_checkstack(lua, 1)) {
        /* Increase the Lua stack if needed, to make sure there is enough room
         * to push elements to the stack. On failure, exit with panic. */
        serverPanic("lua stack limit reach when parsing redis.call reply");
    }
    lua_pushboolean(lua,0);
}


static void redisProtocolToLuaType_BulkString(void *ctx, const char *str, size_t len, const char *proto, size_t proto_len) {
    UNUSED(proto);
    UNUSED(proto_len);
    if (!ctx) {
        return;
    }

    lua_State *lua = ctx;
    if (!lua_checkstack(lua, 1)) {
        /* Increase the Lua stack if needed, to make sure there is enough room
         * to push elements to the stack. On failure, exit with panic. */
        serverPanic("lua stack limit reach when parsing redis.call reply");
    }
    lua_pushlstring(lua,str,len);
}

static void redisProtocolToLuaType_Status(void *ctx, const char *str, size_t len, const char *proto, size_t proto_len) {
    UNUSED(proto);
    UNUSED(proto_len);
    if (!ctx) {
        return;
    }

    lua_State *lua = ctx;
    if (!lua_checkstack(lua, 3)) {
        /* Increase the Lua stack if needed, to make sure there is enough room
         * to push elements to the stack. On failure, exit with panic. */
        serverPanic("lua stack limit reach when parsing redis.call reply");
    }
    lua_newtable(lua);
    lua_pushstring(lua,"ok");
    lua_pushlstring(lua,str,len);
    lua_settable(lua,-3);
}

static void redisProtocolToLuaType_Error(void *ctx, const char *str, size_t len, const char *proto, size_t proto_len) {
    UNUSED(proto);
    UNUSED(proto_len);
    if (!ctx) {
        return;
    }

    lua_State *lua = ctx;
    if (!lua_checkstack(lua, 3)) {
        /* Increase the Lua stack if needed, to make sure there is enough room
         * to push elements to the stack. On failure, exit with panic. */
        serverPanic("lua stack limit reach when parsing redis.call reply");
    }
    lua_newtable(lua);
    lua_pushstring(lua,"err");
    lua_pushlstring(lua,str,len);
    lua_settable(lua,-3);
}

static void redisProtocolToLuaType_Map(struct ReplyParser *parser, void *ctx, size_t len, const char *proto) {
    UNUSED(proto);
    lua_State *lua = ctx;
    if (lua) {
        if (!lua_checkstack(lua, 3)) {
            /* Increase the Lua stack if needed, to make sure there is enough room
             * to push elements to the stack. On failure, exit with panic. */
            serverPanic("lua stack limit reach when parsing redis.call reply");
        }
        lua_newtable(lua);
        lua_pushstring(lua, "map");
        lua_newtable(lua);
    }
    for (size_t j = 0; j < len; j++) {
        parseReply(parser,lua);
        parseReply(parser,lua);
        if (lua) lua_settable(lua,-3);
    }
    if (lua) lua_settable(lua,-3);
}

static void redisProtocolToLuaType_Set(struct ReplyParser *parser, void *ctx, size_t len, const char *proto) {
    UNUSED(proto);

    lua_State *lua = ctx;
    if (lua) {
        if (!lua_checkstack(lua, 3)) {
            /* Increase the Lua stack if needed, to make sure there is enough room
             * to push elements to the stack. On failure, exit with panic. */
            serverPanic("lua stack limit reach when parsing redis.call reply");
        }
        lua_newtable(lua);
        lua_pushstring(lua, "set");
        lua_newtable(lua);
    }
    for (size_t j = 0; j < len; j++) {
        parseReply(parser,lua);
        if (lua) {
            if (!lua_checkstack(lua, 1)) {
                /* Increase the Lua stack if needed, to make sure there is enough room
                 * to push elements to the stack. On failure, exit with panic.
                 * Notice that here we need to check the stack again because the recursive
                 * call to redisProtocolToLuaType might have use the room allocated in the stack*/
                serverPanic("lua stack limit reach when parsing redis.call reply");
            }
            lua_pushboolean(lua,1);
            lua_settable(lua,-3);
        }
    }
    if (lua) lua_settable(lua,-3);
}

static void redisProtocolToLuaType_Array(struct ReplyParser *parser, void *ctx, size_t len, const char *proto) {
    UNUSED(proto);

    lua_State *lua = ctx;
    if (lua){
        if (!lua_checkstack(lua, 2)) {
            /* Increase the Lua stack if needed, to make sure there is enough room
             * to push elements to the stack. On failure, exit with panic. */
            serverPanic("lua stack limit reach when parsing redis.call reply");
        }
        lua_newtable(lua);
    }
    for (size_t j = 0; j < len; j++) {
        if (lua) lua_pushnumber(lua,j+1);
        parseReply(parser,lua);
        if (lua) lua_settable(lua,-3);
    }
}

static void redisProtocolToLuaType_Attribute(struct ReplyParser *parser, void *ctx, size_t len, const char *proto) {
    UNUSED(proto);

    /* Parse the attribute reply.
     * Currently, we do not expose the attribute to the Lua script so
     * we just need to continue parsing and ignore it (the NULL ensures that the
     * reply will be ignored). */
    for (size_t j = 0; j < len; j++) {
        parseReply(parser,NULL);
        parseReply(parser,NULL);
    }

    /* Parse the reply itself. */
    parseReply(parser,ctx);
}

static void redisProtocolToLuaType_VerbatimString(void *ctx, const char *format, const char *str, size_t len, const char *proto, size_t proto_len) {
    UNUSED(proto);
    UNUSED(proto_len);
    if (!ctx) {
        return;
    }

    lua_State *lua = ctx;
    if (!lua_checkstack(lua, 5)) {
        /* Increase the Lua stack if needed, to make sure there is enough room
         * to push elements to the stack. On failure, exit with panic. */
        serverPanic("lua stack limit reach when parsing redis.call reply");
    }
    lua_newtable(lua);
    lua_pushstring(lua,"verbatim_string");
    lua_newtable(lua);
    lua_pushstring(lua,"string");
    lua_pushlstring(lua,str,len);
    lua_settable(lua,-3);
    lua_pushstring(lua,"format");
    lua_pushlstring(lua,format,3);
    lua_settable(lua,-3);
    lua_settable(lua,-3);
}

static void redisProtocolToLuaType_BigNumber(void *ctx, const char *str, size_t len, const char *proto, size_t proto_len) {
    UNUSED(proto);
    UNUSED(proto_len);
    if (!ctx) {
        return;
    }

    lua_State *lua = ctx;
    if (!lua_checkstack(lua, 3)) {
        /* Increase the Lua stack if needed, to make sure there is enough room
         * to push elements to the stack. On failure, exit with panic. */
        serverPanic("lua stack limit reach when parsing redis.call reply");
    }
    lua_newtable(lua);
    lua_pushstring(lua,"big_number");
    lua_pushlstring(lua,str,len);
    lua_settable(lua,-3);
}

static void redisProtocolToLuaType_Null(void *ctx, const char *proto, size_t proto_len) {
    UNUSED(proto);
    UNUSED(proto_len);
    if (!ctx) {
        return;
    }

    lua_State *lua = ctx;
    if (!lua_checkstack(lua, 1)) {
        /* Increase the Lua stack if needed, to make sure there is enough room
         * to push elements to the stack. On failure, exit with panic. */
        serverPanic("lua stack limit reach when parsing redis.call reply");
    }
    lua_pushnil(lua);
}

static void redisProtocolToLuaType_Bool(void *ctx, int val, const char *proto, size_t proto_len) {
    UNUSED(proto);
    UNUSED(proto_len);
    if (!ctx) {
        return;
    }

    lua_State *lua = ctx;
    if (!lua_checkstack(lua, 1)) {
        /* Increase the Lua stack if needed, to make sure there is enough room
         * to push elements to the stack. On failure, exit with panic. */
        serverPanic("lua stack limit reach when parsing redis.call reply");
    }
    lua_pushboolean(lua,val);
}

static void redisProtocolToLuaType_Double(void *ctx, double d, const char *proto, size_t proto_len) {
    UNUSED(proto);
    UNUSED(proto_len);
    if (!ctx) {
        return;
    }

    lua_State *lua = ctx;
    if (!lua_checkstack(lua, 3)) {
        /* Increase the Lua stack if needed, to make sure there is enough room
         * to push elements to the stack. On failure, exit with panic. */
        serverPanic("lua stack limit reach when parsing redis.call reply");
    }
    lua_newtable(lua);
    lua_pushstring(lua,"double");
    lua_pushnumber(lua,d);
    lua_settable(lua,-3);
}

/* This function is used in order to push an error on the Lua stack in the
 * format used by redis.pcall to return errors, which is a lua table
 * with a single "err" field set to the error string. Note that this
 * table is never a valid reply by proper commands, since the returned
 * tables are otherwise always indexed by integers, never by strings. */
void luaPushError(lua_State *lua, char *error) {
    lua_Debug dbg;

    /* If debugging is active and in step mode, log errors resulting from
     * Redis commands. */
    if (ldbIsEnabled()) {
        ldbLog(sdscatprintf(sdsempty(),"<error> %s",error));
    }

    lua_newtable(lua);
    lua_pushstring(lua,"err");

    /* Attempt to figure out where this function was called, if possible */
    if(lua_getstack(lua, 1, &dbg) && lua_getinfo(lua, "nSl", &dbg)) {
        sds msg = sdscatprintf(sdsempty(), "%s: %d: %s",
            dbg.source, dbg.currentline, error);
        lua_pushstring(lua, msg);
        sdsfree(msg);
    } else {
        lua_pushstring(lua, error);
    }
    lua_settable(lua,-3);
}

/* In case the error set into the Lua stack by luaPushError() was generated
 * by the non-error-trapping version of redis.pcall(), which is redis.call(),
 * this function will raise the Lua error so that the execution of the
 * script will be halted. */
int luaRaiseError(lua_State *lua) {
    lua_pushstring(lua,"err");
    lua_gettable(lua,-2);
    return lua_error(lua);
}


/* ---------------------------------------------------------------------------
 * Lua reply to Redis reply conversion functions.
 * ------------------------------------------------------------------------- */

/* Reply to client 'c' converting the top element in the Lua stack to a
 * Redis reply. As a side effect the element is consumed from the stack.  */
static void luaReplyToRedisReply(client *c, client* script_client, lua_State *lua) {
    int t = lua_type(lua,-1);

    if (!lua_checkstack(lua, 4)) {
        /* Increase the Lua stack if needed to make sure there is enough room
         * to push 4 elements to the stack. On failure, return error.
         * Notice that we need, in the worst case, 4 elements because returning a map might
         * require push 4 elements to the Lua stack.*/
        addReplyErrorFormat(c, "reached lua stack limit");
        lua_pop(lua,1); /* pop the element from the stack */
        return;
    }

    switch(t) {
    case LUA_TSTRING:
        addReplyBulkCBuffer(c,(char*)lua_tostring(lua,-1),lua_strlen(lua,-1));
        break;
    case LUA_TBOOLEAN:
        if (script_client->resp == 2)
            addReply(c,lua_toboolean(lua,-1) ? shared.cone :
                                               shared.null[c->resp]);
        else
            addReplyBool(c,lua_toboolean(lua,-1));
        break;
    case LUA_TNUMBER:
        addReplyLongLong(c,(long long)lua_tonumber(lua,-1));
        break;
    case LUA_TTABLE:
        /* We need to check if it is an array, an error, or a status reply.
         * Error are returned as a single element table with 'err' field.
         * Status replies are returned as single element table with 'ok'
         * field. */

        /* Handle error reply. */
        /* we took care of the stack size on function start */
        lua_pushstring(lua,"err");
        lua_gettable(lua,-2);
        t = lua_type(lua,-1);
        if (t == LUA_TSTRING) {
            addReplyErrorFormat(c,"-%s",lua_tostring(lua,-1));
            lua_pop(lua,2);
            return;
        }
        lua_pop(lua,1); /* Discard field name pushed before. */

        /* Handle status reply. */
        lua_pushstring(lua,"ok");
        lua_gettable(lua,-2);
        t = lua_type(lua,-1);
        if (t == LUA_TSTRING) {
            sds ok = sdsnew(lua_tostring(lua,-1));
            sdsmapchars(ok,"\r\n","  ",2);
            addReplySds(c,sdscatprintf(sdsempty(),"+%s\r\n",ok));
            sdsfree(ok);
            lua_pop(lua,2);
            return;
        }
        lua_pop(lua,1); /* Discard field name pushed before. */

        /* Handle double reply. */
        lua_pushstring(lua,"double");
        lua_gettable(lua,-2);
        t = lua_type(lua,-1);
        if (t == LUA_TNUMBER) {
            addReplyDouble(c,lua_tonumber(lua,-1));
            lua_pop(lua,2);
            return;
        }
        lua_pop(lua,1); /* Discard field name pushed before. */

        /* Handle big number reply. */
        lua_pushstring(lua,"big_number");
        lua_gettable(lua,-2);
        t = lua_type(lua,-1);
        if (t == LUA_TSTRING) {
            sds big_num = sdsnewlen(lua_tostring(lua,-1), lua_strlen(lua,-1));
            sdsmapchars(big_num,"\r\n","  ",2);
            addReplyBigNum(c,big_num,sdslen(big_num));
            sdsfree(big_num);
            lua_pop(lua,2);
            return;
        }
        lua_pop(lua,1); /* Discard field name pushed before. */

        /* Handle verbatim reply. */
        lua_pushstring(lua,"verbatim_string");
        lua_gettable(lua,-2);
        t = lua_type(lua,-1);
        if (t == LUA_TTABLE) {
            lua_pushstring(lua,"format");
            lua_gettable(lua,-2);
            t = lua_type(lua,-1);
            if (t == LUA_TSTRING){
                char* format = (char*)lua_tostring(lua,-1);
                lua_pushstring(lua,"string");
                lua_gettable(lua,-3);
                t = lua_type(lua,-1);
                if (t == LUA_TSTRING){
                    size_t len;
                    char* str = (char*)lua_tolstring(lua,-1,&len);
                    addReplyVerbatim(c, str, len, format);
                    lua_pop(lua,4);
                    return;
                }
                lua_pop(lua,1);
            }
            lua_pop(lua,1);
        }
        lua_pop(lua,1); /* Discard field name pushed before. */

        /* Handle map reply. */
        lua_pushstring(lua,"map");
        lua_gettable(lua,-2);
        t = lua_type(lua,-1);
        if (t == LUA_TTABLE) {
            int maplen = 0;
            void *replylen = addReplyDeferredLen(c);
            /* we took care of the stack size on function start */
            lua_pushnil(lua); /* Use nil to start iteration. */
            while (lua_next(lua,-2)) {
                /* Stack now: table, key, value */
                lua_pushvalue(lua,-2);        /* Dup key before consuming. */
                luaReplyToRedisReply(c, script_client, lua); /* Return key. */
                luaReplyToRedisReply(c, script_client, lua); /* Return value. */
                /* Stack now: table, key. */
                maplen++;
            }
            setDeferredMapLen(c,replylen,maplen);
            lua_pop(lua,2);
            return;
        }
        lua_pop(lua,1); /* Discard field name pushed before. */

        /* Handle set reply. */
        lua_pushstring(lua,"set");
        lua_gettable(lua,-2);
        t = lua_type(lua,-1);
        if (t == LUA_TTABLE) {
            int setlen = 0;
            void *replylen = addReplyDeferredLen(c);
            /* we took care of the stack size on function start */
            lua_pushnil(lua); /* Use nil to start iteration. */
            while (lua_next(lua,-2)) {
                /* Stack now: table, key, true */
                lua_pop(lua,1);               /* Discard the boolean value. */
                lua_pushvalue(lua,-1);        /* Dup key before consuming. */
                luaReplyToRedisReply(c, script_client, lua); /* Return key. */
                /* Stack now: table, key. */
                setlen++;
            }
            setDeferredSetLen(c,replylen,setlen);
            lua_pop(lua,2);
            return;
        }
        lua_pop(lua,1); /* Discard field name pushed before. */

        /* Handle the array reply. */
        void *replylen = addReplyDeferredLen(c);
        int j = 1, mbulklen = 0;
        while(1) {
            /* we took care of the stack size on function start */
            lua_pushnumber(lua,j++);
            lua_gettable(lua,-2);
            t = lua_type(lua,-1);
            if (t == LUA_TNIL) {
                lua_pop(lua,1);
                break;
            }
            luaReplyToRedisReply(c, script_client, lua);
            mbulklen++;
        }
        setDeferredArrayLen(c,replylen,mbulklen);
        break;
    default:
        addReplyNull(c);
    }
    lua_pop(lua,1);
}

/* ---------------------------------------------------------------------------
 * Lua redis.* functions implementations.
 * ------------------------------------------------------------------------- */

#define LUA_CMD_OBJCACHE_SIZE 32
#define LUA_CMD_OBJCACHE_MAX_LEN 64
static int luaRedisGenericCommand(lua_State *lua, int raise_error) {
    int j, argc = lua_gettop(lua);
    scriptRunCtx* rctx = luaGetFromRegistry(lua, REGISTRY_RUN_CTX_NAME);
    if (!rctx) {
        luaPushError(lua, "redis.call/pcall can only be called inside a script invocation");
        return luaRaiseError(lua);
    }
    sds err = NULL;
    client* c = rctx->c;
    sds reply;

    /* Cached across calls. */
    static robj **argv = NULL;
    static int argv_size = 0;
    static robj *cached_objects[LUA_CMD_OBJCACHE_SIZE];
    static size_t cached_objects_len[LUA_CMD_OBJCACHE_SIZE];
    static int inuse = 0;   /* Recursive calls detection. */

    /* By using Lua debug hooks it is possible to trigger a recursive call
     * to luaRedisGenericCommand(), which normally should never happen.
     * To make this function reentrant is futile and makes it slower, but
     * we should at least detect such a misuse, and abort. */
    if (inuse) {
        char *recursion_warning =
            "luaRedisGenericCommand() recursive call detected. "
            "Are you doing funny stuff with Lua debug hooks?";
        serverLog(LL_WARNING,"%s",recursion_warning);
        luaPushError(lua,recursion_warning);
        return 1;
    }
    inuse++;

    /* Require at least one argument */
    if (argc == 0) {
        luaPushError(lua,
            "Please specify at least one argument for redis.call()");
        inuse--;
        return raise_error ? luaRaiseError(lua) : 1;
    }

    /* Build the arguments vector */
    if (argv_size < argc) {
        argv = zrealloc(argv,sizeof(robj*)*argc);
        argv_size = argc;
    }

    for (j = 0; j < argc; j++) {
        char *obj_s;
        size_t obj_len;
        char dbuf[64];

        if (lua_type(lua,j+1) == LUA_TNUMBER) {
            /* We can't use lua_tolstring() for number -> string conversion
             * since Lua uses a format specifier that loses precision. */
            lua_Number num = lua_tonumber(lua,j+1);

            obj_len = snprintf(dbuf,sizeof(dbuf),"%.17g",(double)num);
            obj_s = dbuf;
        } else {
            obj_s = (char*)lua_tolstring(lua,j+1,&obj_len);
            if (obj_s == NULL) break; /* Not a string. */
        }

        /* Try to use a cached object. */
        if (j < LUA_CMD_OBJCACHE_SIZE && cached_objects[j] &&
            cached_objects_len[j] >= obj_len)
        {
            sds s = cached_objects[j]->ptr;
            argv[j] = cached_objects[j];
            cached_objects[j] = NULL;
            memcpy(s,obj_s,obj_len+1);
            sdssetlen(s, obj_len);
        } else {
            argv[j] = createStringObject(obj_s, obj_len);
        }
    }

    /* Check if one of the arguments passed by the Lua script
     * is not a string or an integer (lua_isstring() return true for
     * integers as well). */
    if (j != argc) {
        j--;
        while (j >= 0) {
            decrRefCount(argv[j]);
            j--;
        }
        luaPushError(lua,
            "Lua redis() command arguments must be strings or integers");
        inuse--;
        return raise_error ? luaRaiseError(lua) : 1;
    }

    /* Pop all arguments from the stack, we do not need them anymore
     * and this way we guaranty we will have room on the stack for the result. */
    lua_pop(lua, argc);

    /* Log the command if debugging is active. */
    if (ldbIsEnabled()) {
        sds cmdlog = sdsnew("<redis>");
        for (j = 0; j < c->argc; j++) {
            if (j == 10) {
                cmdlog = sdscatprintf(cmdlog," ... (%d more)",
                    c->argc-j-1);
                break;
            } else {
                cmdlog = sdscatlen(cmdlog," ",1);
                cmdlog = sdscatsds(cmdlog,c->argv[j]->ptr);
            }
        }
        ldbLog(cmdlog);
    }


    scriptCall(rctx, argv, argc, &err);
    if (err) {
        luaPushError(lua, err);
        sdsfree(err);
        goto cleanup;
    }

    /* Convert the result of the Redis command into a suitable Lua type.
     * The first thing we need is to create a single string from the client
     * output buffers. */
    if (listLength(c->reply) == 0 && (size_t)c->bufpos < c->buf_usable_size) {
        /* This is a fast path for the common case of a reply inside the
         * client static buffer. Don't create an SDS string but just use
         * the client buffer directly. */
        c->buf[c->bufpos] = '\0';
        reply = c->buf;
        c->bufpos = 0;
    } else {
        reply = sdsnewlen(c->buf,c->bufpos);
        c->bufpos = 0;
        while(listLength(c->reply)) {
            clientReplyBlock *o = listNodeValue(listFirst(c->reply));

            reply = sdscatlen(reply,o->buf,o->used);
            listDelNode(c->reply,listFirst(c->reply));
        }
    }
    if (raise_error && reply[0] != '-') raise_error = 0;
    redisProtocolToLuaType(lua,reply);

    /* If the debugger is active, log the reply from Redis. */
    if (ldbIsEnabled())
        ldbLogRedisReply(reply);

    if (reply != c->buf) sdsfree(reply);
    c->reply_bytes = 0;

cleanup:
    /* Clean up. Command code may have changed argv/argc so we use the
     * argv/argc of the client instead of the local variables. */
    for (j = 0; j < c->argc; j++) {
        robj *o = c->argv[j];

        /* Try to cache the object in the cached_objects array.
         * The object must be small, SDS-encoded, and with refcount = 1
         * (we must be the only owner) for us to cache it. */
        if (j < LUA_CMD_OBJCACHE_SIZE &&
            o->refcount == 1 &&
            (o->encoding == OBJ_ENCODING_RAW ||
             o->encoding == OBJ_ENCODING_EMBSTR) &&
            sdslen(o->ptr) <= LUA_CMD_OBJCACHE_MAX_LEN)
        {
            sds s = o->ptr;
            if (cached_objects[j]) decrRefCount(cached_objects[j]);
            cached_objects[j] = o;
            cached_objects_len[j] = sdsalloc(s);
        } else {
            decrRefCount(o);
        }
    }

    if (c->argv != argv) {
        zfree(c->argv);
        argv = NULL;
        argv_size = 0;
    }

    c->user = NULL;
    c->argv = NULL;
    c->argc = 0;

    if (raise_error) {
        /* If we are here we should have an error in the stack, in the
         * form of a table with an "err" field. Extract the string to
         * return the plain error. */
        inuse--;
        return luaRaiseError(lua);
    }
    inuse--;
    return 1;
}

/* redis.call() */
static int luaRedisCallCommand(lua_State *lua) {
    return luaRedisGenericCommand(lua,1);
}

/* redis.pcall() */
static int luaRedisPCallCommand(lua_State *lua) {
    return luaRedisGenericCommand(lua,0);
}

/* This adds redis.sha1hex(string) to Lua scripts using the same hashing
 * function used for sha1ing lua scripts. */
static int luaRedisSha1hexCommand(lua_State *lua) {
    int argc = lua_gettop(lua);
    char digest[41];
    size_t len;
    char *s;

    if (argc != 1) {
        lua_pushstring(lua, "wrong number of arguments");
        return lua_error(lua);
    }

    s = (char*)lua_tolstring(lua,1,&len);
    sha1hex(digest,s,len);
    lua_pushstring(lua,digest);
    return 1;
}

/* Returns a table with a single field 'field' set to the string value
 * passed as argument. This helper function is handy when returning
 * a Redis Protocol error or status reply from Lua:
 *
 * return redis.error_reply("ERR Some Error")
 * return redis.status_reply("ERR Some Error")
 */
static int luaRedisReturnSingleFieldTable(lua_State *lua, char *field) {
    if (lua_gettop(lua) != 1 || lua_type(lua,-1) != LUA_TSTRING) {
        luaPushError(lua, "wrong number or type of arguments");
        return 1;
    }

    lua_newtable(lua);
    lua_pushstring(lua, field);
    lua_pushvalue(lua, -3);
    lua_settable(lua, -3);
    return 1;
}

/* redis.error_reply() */
static int luaRedisErrorReplyCommand(lua_State *lua) {
    return luaRedisReturnSingleFieldTable(lua,"err");
}

/* redis.status_reply() */
static int luaRedisStatusReplyCommand(lua_State *lua) {
    return luaRedisReturnSingleFieldTable(lua,"ok");
}

/* redis.set_repl()
 *
 * Set the propagation of write commands executed in the context of the
 * script to on/off for AOF and slaves. */
static int luaRedisSetReplCommand(lua_State *lua) {
    int flags, argc = lua_gettop(lua);

    scriptRunCtx* rctx = luaGetFromRegistry(lua, REGISTRY_RUN_CTX_NAME);
    if (!rctx) {
        lua_pushstring(lua, "redis.set_repl can only be called inside a script invocation");
        return lua_error(lua);
    }

    if (argc != 1) {
         lua_pushstring(lua, "redis.set_repl() requires two arguments.");
         return lua_error(lua);
    }

    flags = lua_tonumber(lua,-1);
    if ((flags & ~(PROPAGATE_AOF|PROPAGATE_REPL)) != 0) {
        lua_pushstring(lua, "Invalid replication flags. Use REPL_AOF, REPL_REPLICA, REPL_ALL or REPL_NONE.");
        return lua_error(lua);
    }

    scriptSetRepl(rctx, flags);
    return 0;
}

/* redis.log() */
static int luaLogCommand(lua_State *lua) {
    int j, argc = lua_gettop(lua);
    int level;
    sds log;

    if (argc < 2) {
        lua_pushstring(lua, "redis.log() requires two arguments or more.");
        return lua_error(lua);
    } else if (!lua_isnumber(lua,-argc)) {
        lua_pushstring(lua, "First argument must be a number (log level).");
        return lua_error(lua);
    }
    level = lua_tonumber(lua,-argc);
    if (level < LL_DEBUG || level > LL_WARNING) {
        lua_pushstring(lua, "Invalid debug level.");
        return lua_error(lua);
    }
    if (level < server.verbosity) return 0;

    /* Glue together all the arguments */
    log = sdsempty();
    for (j = 1; j < argc; j++) {
        size_t len;
        char *s;

        s = (char*)lua_tolstring(lua,(-argc)+j,&len);
        if (s) {
            if (j != 1) log = sdscatlen(log," ",1);
            log = sdscatlen(log,s,len);
        }
    }
    serverLogRaw(level,log);
    sdsfree(log);
    return 0;
}

/* redis.setresp() */
static int luaSetResp(lua_State *lua) {
    scriptRunCtx* rctx = luaGetFromRegistry(lua, REGISTRY_RUN_CTX_NAME);
    if (!rctx) {
        lua_pushstring(lua, "redis.setresp can only be called inside a script invocation");
        return lua_error(lua);
    }
    int argc = lua_gettop(lua);

    if (argc != 1) {
        lua_pushstring(lua, "redis.setresp() requires one argument.");
        return lua_error(lua);
    }

    int resp = lua_tonumber(lua,-argc);
    if (resp != 2 && resp != 3) {
        lua_pushstring(lua, "RESP version must be 2 or 3.");
        return lua_error(lua);
    }
    scriptSetResp(rctx, resp);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Lua engine initialization and reset.
 * ------------------------------------------------------------------------- */

static void luaLoadLib(lua_State *lua, const char *libname, lua_CFunction luafunc) {
  lua_pushcfunction(lua, luafunc);
  lua_pushstring(lua, libname);
  lua_call(lua, 1, 0);
}

LUALIB_API int (luaopen_cjson) (lua_State *L);
LUALIB_API int (luaopen_struct) (lua_State *L);
LUALIB_API int (luaopen_cmsgpack) (lua_State *L);
LUALIB_API int (luaopen_bit) (lua_State *L);

static void luaLoadLibraries(lua_State *lua) {
    luaLoadLib(lua, "", luaopen_base);
    luaLoadLib(lua, LUA_TABLIBNAME, luaopen_table);
    luaLoadLib(lua, LUA_STRLIBNAME, luaopen_string);
    luaLoadLib(lua, LUA_MATHLIBNAME, luaopen_math);
    luaLoadLib(lua, LUA_DBLIBNAME, luaopen_debug);
    luaLoadLib(lua, "cjson", luaopen_cjson);
    luaLoadLib(lua, "struct", luaopen_struct);
    luaLoadLib(lua, "cmsgpack", luaopen_cmsgpack);
    luaLoadLib(lua, "bit", luaopen_bit);

#if 0 /* Stuff that we don't load currently, for sandboxing concerns. */
    luaLoadLib(lua, LUA_LOADLIBNAME, luaopen_package);
    luaLoadLib(lua, LUA_OSLIBNAME, luaopen_os);
#endif
}

/* Remove a functions that we don't want to expose to the Redis scripting
 * environment. */
static void luaRemoveUnsupportedFunctions(lua_State *lua) {
    lua_pushnil(lua);
    lua_setglobal(lua,"loadfile");
    lua_pushnil(lua);
    lua_setglobal(lua,"dofile");
}

/* Return sds of the string value located on stack at the given index.
 * Return NULL if the value is not a string. */
sds luaGetStringSds(lua_State *lua, int index) {
    if (!lua_isstring(lua, index)) {
        return NULL;
    }

    size_t len;
    const char *str = lua_tolstring(lua, index, &len);
    sds str_sds = sdsnewlen(str, len);
    return str_sds;
}

/* This function installs metamethods in the global table _G that prevent
 * the creation of globals accidentally.
 *
 * It should be the last to be called in the scripting engine initialization
 * sequence, because it may interact with creation of globals.
 *
 * On Legacy Lua (eval) we need to check 'w ~= \"main\"' otherwise we will not be able
 * to create the global 'function <sha> ()' variable. On Functions Lua engine we do not use
 * this trick so it's not needed. */
void luaEnableGlobalsProtection(lua_State *lua, int is_eval) {
    char *s[32];
    sds code = sdsempty();
    int j = 0;

    /* strict.lua from: http://metalua.luaforge.net/src/lib/strict.lua.html.
     * Modified to be adapted to Redis. */
    s[j++]="local dbg=debug\n";
    s[j++]="local mt = {}\n";
    s[j++]="setmetatable(_G, mt)\n";
    s[j++]="mt.__newindex = function (t, n, v)\n";
    s[j++]="  if dbg.getinfo(2) then\n";
    s[j++]="    local w = dbg.getinfo(2, \"S\").what\n";
    s[j++]=     is_eval ? "    if w ~= \"main\" and w ~= \"C\" then\n" : "    if w ~= \"C\" then\n";
    s[j++]="      error(\"Script attempted to create global variable '\"..tostring(n)..\"'\", 2)\n";
    s[j++]="    end\n";
    s[j++]="  end\n";
    s[j++]="  rawset(t, n, v)\n";
    s[j++]="end\n";
    s[j++]="mt.__index = function (t, n)\n";
    s[j++]="  if dbg.getinfo(2) and dbg.getinfo(2, \"S\").what ~= \"C\" then\n";
    s[j++]="    error(\"Script attempted to access nonexistent global variable '\"..tostring(n)..\"'\", 2)\n";
    s[j++]="  end\n";
    s[j++]="  return rawget(t, n)\n";
    s[j++]="end\n";
    s[j++]="debug = nil\n";
    s[j++]=NULL;

    for (j = 0; s[j] != NULL; j++) code = sdscatlen(code,s[j],strlen(s[j]));
    luaL_loadbuffer(lua,code,sdslen(code),"@enable_strict_lua");
    lua_pcall(lua,0,0,0);
    sdsfree(code);
}

/* Create a global protection function and put it to registry.
 * This need to be called once in the lua_State lifetime.
 * After called it is possible to use luaSetGlobalProtection
 * to set global protection on a give table.
 *
 * The function assumes the Lua stack have a least enough
 * space to push 2 element, its up to the caller to verify
 * this before calling this function.
 *
 * Notice, the difference between this and luaEnableGlobalsProtection
 * is that luaEnableGlobalsProtection is enabling global protection
 * on the current Lua globals. This registering a global protection
 * function that later can be applied on any table. */
void luaRegisterGlobalProtectionFunction(lua_State *lua) {
    lua_pushstring(lua, REGISTRY_SET_GLOBALS_PROTECTION_NAME);
    char *global_protection_func =       "local dbg = debug\n"
                                         "local globals_protection = function (t)\n"
                                         "   local mt = {}\n"
                                         "   setmetatable(t, mt)\n"
                                         "   mt.__newindex = function (t, n, v)\n"
                                         "       if dbg.getinfo(2) then\n"
                                         "           local w = dbg.getinfo(2, \"S\").what\n"
                                         "           if w ~= \"C\" then\n"
                                         "               error(\"Script attempted to create global variable '\"..tostring(n)..\"'\", 2)\n"
                                         "           end"
                                         "       end"
                                         "       rawset(t, n, v)\n"
                                         "   end\n"
                                         "   mt.__index = function (t, n)\n"
                                         "       if dbg.getinfo(2) and dbg.getinfo(2, \"S\").what ~= \"C\" then\n"
                                         "           error(\"Script attempted to access nonexistent global variable '\"..tostring(n)..\"'\", 2)\n"
                                         "       end\n"
                                         "       return rawget(t, n)\n"
                                         "   end\n"
                                         "end\n"
                                         "return globals_protection";
    int res = luaL_loadbuffer(lua, global_protection_func, strlen(global_protection_func), "@global_protection_def");
    serverAssert(res == 0);
    res = lua_pcall(lua,0,1,0);
    serverAssert(res == 0);
    lua_settable(lua, LUA_REGISTRYINDEX);
}

/* Set global protection on a given table.
 * The table need to be located on the top of the lua stack.
 * After called, it will no longer be possible to set
 * new items on the table. The function is not removing
 * the table from the top of the stack!
 *
 * The function assumes the Lua stack have a least enough
 * space to push 2 element, its up to the caller to verify
 * this before calling this function. */
void luaSetGlobalProtection(lua_State *lua) {
    lua_pushstring(lua, REGISTRY_SET_GLOBALS_PROTECTION_NAME);
    lua_gettable(lua, LUA_REGISTRYINDEX);
    lua_pushvalue(lua, -2);
    int res = lua_pcall(lua, 1, 0, 0);
    serverAssert(res == 0);
}

void luaRegisterVersion(lua_State* lua) {
    lua_pushstring(lua,"REDIS_VERSION_NUM");
    lua_pushnumber(lua,REDIS_VERSION_NUM);
    lua_settable(lua,-3);

    lua_pushstring(lua,"REDIS_VERSION");
    lua_pushstring(lua,REDIS_VERSION);
    lua_settable(lua,-3);
}

void luaRegisterLogFunction(lua_State* lua) {
    /* redis.log and log levels. */
    lua_pushstring(lua,"log");
    lua_pushcfunction(lua,luaLogCommand);
    lua_settable(lua,-3);

    lua_pushstring(lua,"LOG_DEBUG");
    lua_pushnumber(lua,LL_DEBUG);
    lua_settable(lua,-3);

    lua_pushstring(lua,"LOG_VERBOSE");
    lua_pushnumber(lua,LL_VERBOSE);
    lua_settable(lua,-3);

    lua_pushstring(lua,"LOG_NOTICE");
    lua_pushnumber(lua,LL_NOTICE);
    lua_settable(lua,-3);

    lua_pushstring(lua,"LOG_WARNING");
    lua_pushnumber(lua,LL_WARNING);
    lua_settable(lua,-3);
}

void luaRegisterRedisAPI(lua_State* lua) {
    luaLoadLibraries(lua);
    luaRemoveUnsupportedFunctions(lua);

    /* Register the redis commands table and fields */
    lua_newtable(lua);

    /* redis.call */
    lua_pushstring(lua,"call");
    lua_pushcfunction(lua,luaRedisCallCommand);
    lua_settable(lua,-3);

    /* redis.pcall */
    lua_pushstring(lua,"pcall");
    lua_pushcfunction(lua,luaRedisPCallCommand);
    lua_settable(lua,-3);

    luaRegisterLogFunction(lua);

    luaRegisterVersion(lua);

    /* redis.setresp */
    lua_pushstring(lua,"setresp");
    lua_pushcfunction(lua,luaSetResp);
    lua_settable(lua,-3);

    /* redis.sha1hex */
    lua_pushstring(lua, "sha1hex");
    lua_pushcfunction(lua, luaRedisSha1hexCommand);
    lua_settable(lua, -3);

    /* redis.error_reply and redis.status_reply */
    lua_pushstring(lua, "error_reply");
    lua_pushcfunction(lua, luaRedisErrorReplyCommand);
    lua_settable(lua, -3);
    lua_pushstring(lua, "status_reply");
    lua_pushcfunction(lua, luaRedisStatusReplyCommand);
    lua_settable(lua, -3);

    /* redis.set_repl and associated flags. */
    lua_pushstring(lua,"set_repl");
    lua_pushcfunction(lua,luaRedisSetReplCommand);
    lua_settable(lua,-3);

    lua_pushstring(lua,"REPL_NONE");
    lua_pushnumber(lua,PROPAGATE_NONE);
    lua_settable(lua,-3);

    lua_pushstring(lua,"REPL_AOF");
    lua_pushnumber(lua,PROPAGATE_AOF);
    lua_settable(lua,-3);

    lua_pushstring(lua,"REPL_SLAVE");
    lua_pushnumber(lua,PROPAGATE_REPL);
    lua_settable(lua,-3);

    lua_pushstring(lua,"REPL_REPLICA");
    lua_pushnumber(lua,PROPAGATE_REPL);
    lua_settable(lua,-3);

    lua_pushstring(lua,"REPL_ALL");
    lua_pushnumber(lua,PROPAGATE_AOF|PROPAGATE_REPL);

    lua_settable(lua,-3);
    /* Finally set the table as 'redis' global var. */
    lua_setglobal(lua,REDIS_API_NAME);

    /* Replace math.random and math.randomseed with our implementations. */
    lua_getglobal(lua,"math");

    lua_pushstring(lua,"random");
    lua_pushcfunction(lua,redis_math_random);
    lua_settable(lua,-3);

    lua_pushstring(lua,"randomseed");
    lua_pushcfunction(lua,redis_math_randomseed);
    lua_settable(lua,-3);

    lua_setglobal(lua,"math");
}

/* Set an array of Redis String Objects as a Lua array (table) stored into a
 * global variable. */
static void luaCreateArray(lua_State *lua, robj **elev, int elec) {
    int j;

    lua_newtable(lua);
    for (j = 0; j < elec; j++) {
        lua_pushlstring(lua,(char*)elev[j]->ptr,sdslen(elev[j]->ptr));
        lua_rawseti(lua,-2,j+1);
    }
}

/* ---------------------------------------------------------------------------
 * Redis provided math.random
 * ------------------------------------------------------------------------- */

/* We replace math.random() with our implementation that is not affected
 * by specific libc random() implementations and will output the same sequence
 * (for the same seed) in every arch. */

/* The following implementation is the one shipped with Lua itself but with
 * rand() replaced by redisLrand48(). */
static int redis_math_random (lua_State *L) {
  scriptRunCtx* rctx = luaGetFromRegistry(L, REGISTRY_RUN_CTX_NAME);
  if (!rctx) {
    return luaL_error(L, "math.random can only be called inside a script invocation");
  }

  /* the `%' avoids the (rare) case of r==1, and is needed also because on
     some systems (SunOS!) `rand()' may return a value larger than RAND_MAX */
  lua_Number r = (lua_Number)(redisLrand48()%REDIS_LRAND48_MAX) /
                                (lua_Number)REDIS_LRAND48_MAX;
  switch (lua_gettop(L)) {  /* check number of arguments */
    case 0: {  /* no arguments */
      lua_pushnumber(L, r);  /* Number between 0 and 1 */
      break;
    }
    case 1: {  /* only upper limit */
      int u = luaL_checkint(L, 1);
      luaL_argcheck(L, 1<=u, 1, "interval is empty");
      lua_pushnumber(L, floor(r*u)+1);  /* int between 1 and `u' */
      break;
    }
    case 2: {  /* lower and upper limits */
      int l = luaL_checkint(L, 1);
      int u = luaL_checkint(L, 2);
      luaL_argcheck(L, l<=u, 2, "interval is empty");
      lua_pushnumber(L, floor(r*(u-l+1))+l);  /* int between `l' and `u' */
      break;
    }
    default: return luaL_error(L, "wrong number of arguments");
  }
  return 1;
}

static int redis_math_randomseed (lua_State *L) {
  scriptRunCtx* rctx = luaGetFromRegistry(L, REGISTRY_RUN_CTX_NAME);
  if (!rctx) {
    return luaL_error(L, "math.randomseed can only be called inside a script invocation");
  }
  redisSrand48(luaL_checkint(L, 1));
  return 0;
}

/* This is the Lua script "count" hook that we use to detect scripts timeout. */
static void luaMaskCountHook(lua_State *lua, lua_Debug *ar) {
    UNUSED(ar);
    scriptRunCtx* rctx = luaGetFromRegistry(lua, REGISTRY_RUN_CTX_NAME);
    if (scriptInterrupt(rctx) == SCRIPT_KILL) {
        serverLog(LL_WARNING,"Lua script killed by user with SCRIPT KILL.");

        /*
         * Set the hook to invoke all the time so the user
         * will not be able to catch the error with pcall and invoke
         * pcall again which will prevent the script from ever been killed
         */
        lua_sethook(lua, luaMaskCountHook, LUA_MASKLINE, 0);

        lua_pushstring(lua,"Script killed by user with SCRIPT KILL...");
        lua_error(lua);
    }
}

void luaCallFunction(scriptRunCtx* run_ctx, lua_State *lua, robj** keys, size_t nkeys, robj** args, size_t nargs, int debug_enabled) {
    client* c = run_ctx->original_client;
    int delhook = 0;

    /* We must set it before we set the Lua hook, theoretically the
     * Lua hook might be called wheneven we run any Lua instruction
     * such as 'luaSetGlobalArray' and we want the run_ctx to be available
     * each time the Lua hook is invoked. */
    luaSaveOnRegistry(lua, REGISTRY_RUN_CTX_NAME, run_ctx);

    if (server.busy_reply_threshold > 0 && !debug_enabled) {
        lua_sethook(lua,luaMaskCountHook,LUA_MASKCOUNT,100000);
        delhook = 1;
    } else if (debug_enabled) {
        lua_sethook(lua,luaLdbLineHook,LUA_MASKLINE|LUA_MASKCOUNT,100000);
        delhook = 1;
    }

    /* Populate the argv and keys table accordingly to the arguments that
     * EVAL received. */
    luaCreateArray(lua,keys,nkeys);
    /* On eval, keys and arguments are globals. */
    if (run_ctx->flags & SCRIPT_EVAL_MODE) lua_setglobal(lua,"KEYS");
    luaCreateArray(lua,args,nargs);
    if (run_ctx->flags & SCRIPT_EVAL_MODE) lua_setglobal(lua,"ARGV");

    /* At this point whether this script was never seen before or if it was
     * already defined, we can call it.
     * On eval mode, we have zero arguments and expect a single return value.
     * In addition the error handler is located on position -2 on the Lua stack.
     * On function mode, we pass 2 arguments (the keys and args tables),
     * and the error handler is located on position -4 (stack: error_handler, callback, keys, args) */
    int err;
    if (run_ctx->flags & SCRIPT_EVAL_MODE) {
        err = lua_pcall(lua,0,1,-2);
    } else {
        err = lua_pcall(lua,2,1,-4);
    }

    /* Call the Lua garbage collector from time to time to avoid a
     * full cycle performed by Lua, which adds too latency.
     *
     * The call is performed every LUA_GC_CYCLE_PERIOD executed commands
     * (and for LUA_GC_CYCLE_PERIOD collection steps) because calling it
     * for every command uses too much CPU. */
    #define LUA_GC_CYCLE_PERIOD 50
    {
        static long gc_count = 0;

        gc_count++;
        if (gc_count == LUA_GC_CYCLE_PERIOD) {
            lua_gc(lua,LUA_GCSTEP,LUA_GC_CYCLE_PERIOD);
            gc_count = 0;
        }
    }

    if (err) {
        addReplyErrorFormat(c,"Error running script (call to %s): %s\n",
                run_ctx->funcname, lua_tostring(lua,-1));
        lua_pop(lua,1); /* Consume the Lua reply and remove error handler. */
    } else {
        /* On success convert the Lua return value into Redis protocol, and
         * send it to * the client. */
        luaReplyToRedisReply(c, run_ctx->c, lua); /* Convert and consume the reply. */
    }

    /* Perform some cleanup that we need to do both on error and success. */
    if (delhook) lua_sethook(lua,NULL,0,0); /* Disable hook */

    /* remove run_ctx from registry, its only applicable for the current script. */
    luaSaveOnRegistry(lua, REGISTRY_RUN_CTX_NAME, NULL);
}

unsigned long luaMemory(lua_State *lua) {
    return lua_gc(lua, LUA_GCCOUNT, 0) * 1024LL;
}
