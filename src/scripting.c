/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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

#include "server.h"
#include "sha1.h"
#include "rand.h"
#include "cluster.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <ctype.h>
#include <math.h>

char *redisProtocolToLuaType_Int(lua_State *lua, char *reply);
char *redisProtocolToLuaType_Bulk(lua_State *lua, char *reply);
char *redisProtocolToLuaType_Status(lua_State *lua, char *reply);
char *redisProtocolToLuaType_Error(lua_State *lua, char *reply);
char *redisProtocolToLuaType_MultiBulk(lua_State *lua, char *reply);
int redis_math_random (lua_State *L);
int redis_math_randomseed (lua_State *L);
void ldbInit(void);
void ldbDisable(client *c);
void ldbEnable(client *c);
void evalGenericCommandWithDebugging(client *c, int evalsha);
void luaLdbLineHook(lua_State *lua, lua_Debug *ar);
void ldbLog(sds entry);
void ldbLogRedisReply(char *reply);
sds ldbCatStackValue(sds s, lua_State *lua, int idx);

/* Debugger shared state is stored inside this global structure. */
#define LDB_BREAKPOINTS_MAX 64  /* Max number of breakpoints. */
#define LDB_MAX_LEN_DEFAULT 256 /* Default len limit for replies / var dumps. */
struct ldbState {
    int fd;     /* Socket of the debugging client. */
    int active; /* Are we debugging EVAL right now? */
    int forked; /* Is this a fork()ed debugging session? */
    list *logs; /* List of messages to send to the client. */
    list *traces; /* Messages about Redis commands executed since last stop.*/
    list *children; /* All forked debugging sessions pids. */
    int bp[LDB_BREAKPOINTS_MAX]; /* An array of breakpoints line numbers. */
    int bpcount; /* Number of valid entries inside bp. */
    int step;   /* Stop at next line ragardless of breakpoints. */
    int luabp;  /* Stop at next line because redis.breakpoint() was called. */
    sds *src;   /* Lua script source code split by line. */
    int lines;  /* Number of lines in 'src'. */
    int currentline;    /* Current line number. */
    sds cbuf;   /* Debugger client command buffer. */
    size_t maxlen;  /* Max var dump / reply length. */
    int maxlen_hint_sent; /* Did we already hint about "set maxlen"? */
} ldb;

/* ---------------------------------------------------------------------------
 * Utility functions.
 * ------------------------------------------------------------------------- */

/* Perform the SHA1 of the input string. We use this both for hashing script
 * bodies in order to obtain the Lua function name, and in the implementation
 * of redis.sha1().
 *
 * 'digest' should point to a 41 bytes buffer: 40 for SHA1 converted into an
 * hexadecimal number, plus 1 byte for null term. */
void sha1hex(char *digest, char *script, size_t len) {
    SHA1_CTX ctx;
    unsigned char hash[20];
    char *cset = "0123456789abcdef";
    int j;

    SHA1Init(&ctx);
    SHA1Update(&ctx,(unsigned char*)script,len);
    SHA1Final(hash,&ctx);

    for (j = 0; j < 20; j++) {
        digest[j*2] = cset[((hash[j]&0xF0)>>4)];
        digest[j*2+1] = cset[(hash[j]&0xF)];
    }
    digest[40] = '\0';
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

char *redisProtocolToLuaType(lua_State *lua, char* reply) {
    char *p = reply;

    switch(*p) {
    case ':': p = redisProtocolToLuaType_Int(lua,reply); break;
    case '$': p = redisProtocolToLuaType_Bulk(lua,reply); break;
    case '+': p = redisProtocolToLuaType_Status(lua,reply); break;
    case '-': p = redisProtocolToLuaType_Error(lua,reply); break;
    case '*': p = redisProtocolToLuaType_MultiBulk(lua,reply); break;
    }
    return p;
}

char *redisProtocolToLuaType_Int(lua_State *lua, char *reply) {
    char *p = strchr(reply+1,'\r');
    long long value;

    string2ll(reply+1,p-reply-1,&value);
    lua_pushnumber(lua,(lua_Number)value);
    return p+2;
}

char *redisProtocolToLuaType_Bulk(lua_State *lua, char *reply) {
    char *p = strchr(reply+1,'\r');
    long long bulklen;

    string2ll(reply+1,p-reply-1,&bulklen);
    if (bulklen == -1) {
        lua_pushboolean(lua,0);
        return p+2;
    } else {
        lua_pushlstring(lua,p+2,bulklen);
        return p+2+bulklen+2;
    }
}

char *redisProtocolToLuaType_Status(lua_State *lua, char *reply) {
    char *p = strchr(reply+1,'\r');

    lua_newtable(lua);
    lua_pushstring(lua,"ok");
    lua_pushlstring(lua,reply+1,p-reply-1);
    lua_settable(lua,-3);
    return p+2;
}

char *redisProtocolToLuaType_Error(lua_State *lua, char *reply) {
    char *p = strchr(reply+1,'\r');

    lua_newtable(lua);
    lua_pushstring(lua,"err");
    lua_pushlstring(lua,reply+1,p-reply-1);
    lua_settable(lua,-3);
    return p+2;
}

char *redisProtocolToLuaType_MultiBulk(lua_State *lua, char *reply) {
    char *p = strchr(reply+1,'\r');
    long long mbulklen;
    int j = 0;

    string2ll(reply+1,p-reply-1,&mbulklen);
    p += 2;
    if (mbulklen == -1) {
        lua_pushboolean(lua,0);
        return p;
    }
    lua_newtable(lua);
    for (j = 0; j < mbulklen; j++) {
        lua_pushnumber(lua,j+1);
        p = redisProtocolToLuaType(lua,p);
        lua_settable(lua,-3);
    }
    return p;
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
    if (ldb.active && ldb.step) {
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

/* Sort the array currently in the stack. We do this to make the output
 * of commands like KEYS or SMEMBERS something deterministic when called
 * from Lua (to play well with AOf/replication).
 *
 * The array is sorted using table.sort itself, and assuming all the
 * list elements are strings. */
void luaSortArray(lua_State *lua) {
    /* Initial Stack: array */
    lua_getglobal(lua,"table");
    lua_pushstring(lua,"sort");
    lua_gettable(lua,-2);       /* Stack: array, table, table.sort */
    lua_pushvalue(lua,-3);      /* Stack: array, table, table.sort, array */
    if (lua_pcall(lua,1,0,0)) {
        /* Stack: array, table, error */

        /* We are not interested in the error, we assume that the problem is
         * that there are 'false' elements inside the array, so we try
         * again with a slower function but able to handle this case, that
         * is: table.sort(table, __redis__compare_helper) */
        lua_pop(lua,1);             /* Stack: array, table */
        lua_pushstring(lua,"sort"); /* Stack: array, table, sort */
        lua_gettable(lua,-2);       /* Stack: array, table, table.sort */
        lua_pushvalue(lua,-3);      /* Stack: array, table, table.sort, array */
        lua_getglobal(lua,"__redis__compare_helper");
        /* Stack: array, table, table.sort, array, __redis__compare_helper */
        lua_call(lua,2,0);
    }
    /* Stack: array (sorted), table */
    lua_pop(lua,1);             /* Stack: array (sorted) */
}

/* ---------------------------------------------------------------------------
 * Lua reply to Redis reply conversion functions.
 * ------------------------------------------------------------------------- */

void luaReplyToRedisReply(client *c, lua_State *lua) {
    int t = lua_type(lua,-1);

    switch(t) {
    case LUA_TSTRING:
        addReplyBulkCBuffer(c,(char*)lua_tostring(lua,-1),lua_strlen(lua,-1));
        break;
    case LUA_TBOOLEAN:
        addReply(c,lua_toboolean(lua,-1) ? shared.cone : shared.nullbulk);
        break;
    case LUA_TNUMBER:
        addReplyLongLong(c,(long long)lua_tonumber(lua,-1));
        break;
    case LUA_TTABLE:
        /* We need to check if it is an array, an error, or a status reply.
         * Error are returned as a single element table with 'err' field.
         * Status replies are returned as single element table with 'ok'
         * field. */
        lua_pushstring(lua,"err");
        lua_gettable(lua,-2);
        t = lua_type(lua,-1);
        if (t == LUA_TSTRING) {
            sds err = sdsnew(lua_tostring(lua,-1));
            sdsmapchars(err,"\r\n","  ",2);
            addReplySds(c,sdscatprintf(sdsempty(),"-%s\r\n",err));
            sdsfree(err);
            lua_pop(lua,2);
            return;
        }

        lua_pop(lua,1);
        lua_pushstring(lua,"ok");
        lua_gettable(lua,-2);
        t = lua_type(lua,-1);
        if (t == LUA_TSTRING) {
            sds ok = sdsnew(lua_tostring(lua,-1));
            sdsmapchars(ok,"\r\n","  ",2);
            addReplySds(c,sdscatprintf(sdsempty(),"+%s\r\n",ok));
            sdsfree(ok);
            lua_pop(lua,1);
        } else {
            void *replylen = addDeferredMultiBulkLength(c);
            int j = 1, mbulklen = 0;

            lua_pop(lua,1); /* Discard the 'ok' field value we popped */
            while(1) {
                lua_pushnumber(lua,j++);
                lua_gettable(lua,-2);
                t = lua_type(lua,-1);
                if (t == LUA_TNIL) {
                    lua_pop(lua,1);
                    break;
                }
                luaReplyToRedisReply(c, lua);
                mbulklen++;
            }
            setDeferredMultiBulkLength(c,replylen,mbulklen);
        }
        break;
    default:
        addReply(c,shared.nullbulk);
    }
    lua_pop(lua,1);
}

/* ---------------------------------------------------------------------------
 * Lua redis.* functions implementations.
 * ------------------------------------------------------------------------- */

#define LUA_CMD_OBJCACHE_SIZE 32
#define LUA_CMD_OBJCACHE_MAX_LEN 64
int luaRedisGenericCommand(lua_State *lua, int raise_error) {
    int j, argc = lua_gettop(lua);
    struct redisCommand *cmd;
    client *c = server.lua_client;
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

    /* Setup our fake client for command execution */
    c->argv = argv;
    c->argc = argc;

    /* Log the command if debugging is active. */
    if (ldb.active && ldb.step) {
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

    /* Command lookup */
    cmd = lookupCommand(argv[0]->ptr);
    if (!cmd || ((cmd->arity > 0 && cmd->arity != argc) ||
                   (argc < -cmd->arity)))
    {
        if (cmd)
            luaPushError(lua,
                "Wrong number of args calling Redis command From Lua script");
        else
            luaPushError(lua,"Unknown Redis command called from Lua script");
        goto cleanup;
    }
    c->cmd = c->lastcmd = cmd;

    /* There are commands that are not allowed inside scripts. */
    if (cmd->flags & CMD_NOSCRIPT) {
        luaPushError(lua, "This Redis command is not allowed from scripts");
        goto cleanup;
    }

    /* Write commands are forbidden against read-only slaves, or if a
     * command marked as non-deterministic was already called in the context
     * of this script. */
    if (cmd->flags & CMD_WRITE) {
        if (server.lua_random_dirty && !server.lua_replicate_commands) {
            luaPushError(lua,
                "Write commands not allowed after non deterministic commands. Call redis.replicate_commands() at the start of your script in order to switch to single commands replication mode.");
            goto cleanup;
        } else if (server.masterhost && server.repl_slave_ro &&
                   !server.loading &&
                   !(server.lua_caller->flags & CLIENT_MASTER))
        {
            luaPushError(lua, shared.roslaveerr->ptr);
            goto cleanup;
        } else if (server.stop_writes_on_bgsave_err &&
                   server.saveparamslen > 0 &&
                   server.lastbgsave_status == C_ERR)
        {
            luaPushError(lua, shared.bgsaveerr->ptr);
            goto cleanup;
        }
    }

    /* If we reached the memory limit configured via maxmemory, commands that
     * could enlarge the memory usage are not allowed, but only if this is the
     * first write in the context of this script, otherwise we can't stop
     * in the middle. */
    if (server.maxmemory && server.lua_write_dirty == 0 &&
        (cmd->flags & CMD_DENYOOM))
    {
        if (freeMemoryIfNeeded() == C_ERR) {
            luaPushError(lua, shared.oomerr->ptr);
            goto cleanup;
        }
    }

    if (cmd->flags & CMD_RANDOM) server.lua_random_dirty = 1;
    if (cmd->flags & CMD_WRITE) server.lua_write_dirty = 1;

    /* If this is a Redis Cluster node, we need to make sure Lua is not
     * trying to access non-local keys, with the exception of commands
     * received from our master or when loading the AOF back in memory. */
    if (server.cluster_enabled && !server.loading &&
        !(server.lua_caller->flags & CLIENT_MASTER))
    {
        /* Duplicate relevant flags in the lua client. */
        c->flags &= ~(CLIENT_READONLY|CLIENT_ASKING);
        c->flags |= server.lua_caller->flags & (CLIENT_READONLY|CLIENT_ASKING);
        if (getNodeByQuery(c,c->cmd,c->argv,c->argc,NULL,NULL) !=
                           server.cluster->myself)
        {
            luaPushError(lua,
                "Lua script attempted to access a non local key in a "
                "cluster node");
            goto cleanup;
        }
    }

    /* If we are using single commands replication, we need to wrap what
     * we propagate into a MULTI/EXEC block, so that it will be atomic like
     * a Lua script in the context of AOF and slaves. */
    if (server.lua_replicate_commands &&
        !server.lua_multi_emitted &&
        server.lua_write_dirty &&
        server.lua_repl != PROPAGATE_NONE)
    {
        execCommandPropagateMulti(server.lua_caller);
        server.lua_multi_emitted = 1;
    }

    /* Run the command */
    int call_flags = CMD_CALL_SLOWLOG | CMD_CALL_STATS;
    if (server.lua_replicate_commands) {
        /* Set flags according to redis.set_repl() settings. */
        if (server.lua_repl & PROPAGATE_AOF)
            call_flags |= CMD_CALL_PROPAGATE_AOF;
        if (server.lua_repl & PROPAGATE_REPL)
            call_flags |= CMD_CALL_PROPAGATE_REPL;
    }
    call(c,call_flags);

    /* Convert the result of the Redis command into a suitable Lua type.
     * The first thing we need is to create a single string from the client
     * output buffers. */
    if (listLength(c->reply) == 0 && c->bufpos < PROTO_REPLY_CHUNK_BYTES) {
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
            sds o = listNodeValue(listFirst(c->reply));

            reply = sdscatsds(reply,o);
            listDelNode(c->reply,listFirst(c->reply));
        }
    }
    if (raise_error && reply[0] != '-') raise_error = 0;
    redisProtocolToLuaType(lua,reply);

    /* If the debugger is active, log the reply from Redis. */
    if (ldb.active && ldb.step)
        ldbLogRedisReply(reply);

    /* Sort the output array if needed, assuming it is a non-null multi bulk
     * reply as expected. */
    if ((cmd->flags & CMD_SORT_FOR_SCRIPT) &&
        (server.lua_replicate_commands == 0) &&
        (reply[0] == '*' && reply[1] != '-')) {
            luaSortArray(lua);
    }
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
int luaRedisCallCommand(lua_State *lua) {
    return luaRedisGenericCommand(lua,1);
}

/* redis.pcall() */
int luaRedisPCallCommand(lua_State *lua) {
    return luaRedisGenericCommand(lua,0);
}

/* This adds redis.sha1hex(string) to Lua scripts using the same hashing
 * function used for sha1ing lua scripts. */
int luaRedisSha1hexCommand(lua_State *lua) {
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
int luaRedisReturnSingleFieldTable(lua_State *lua, char *field) {
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
int luaRedisErrorReplyCommand(lua_State *lua) {
    return luaRedisReturnSingleFieldTable(lua,"err");
}

/* redis.status_reply() */
int luaRedisStatusReplyCommand(lua_State *lua) {
    return luaRedisReturnSingleFieldTable(lua,"ok");
}

/* redis.replicate_commands()
 *
 * Turn on single commands replication if the script never called
 * a write command so far, and returns true. Otherwise if the script
 * already started to write, returns false and stick to whole scripts
 * replication, which is our default. */
int luaRedisReplicateCommandsCommand(lua_State *lua) {
    if (server.lua_write_dirty) {
        lua_pushboolean(lua,0);
    } else {
        server.lua_replicate_commands = 1;
        /* When we switch to single commands replication, we can provide
         * different math.random() sequences at every call, which is what
         * the user normally expects. */
        redisSrand48(rand());
        lua_pushboolean(lua,1);
    }
    return 1;
}

/* redis.breakpoint()
 *
 * Allows to stop execution during a debuggign session from within
 * the Lua code implementation, like if a breakpoint was set in the code
 * immediately after the function. */
int luaRedisBreakpointCommand(lua_State *lua) {
    if (ldb.active) {
        ldb.luabp = 1;
        lua_pushboolean(lua,1);
    } else {
        lua_pushboolean(lua,0);
    }
    return 1;
}

/* redis.debug()
 *
 * Log a string message into the output console.
 * Can take multiple arguments that will be separated by commas.
 * Nothing is returned to the caller. */
int luaRedisDebugCommand(lua_State *lua) {
    if (!ldb.active) return 0;
    int argc = lua_gettop(lua);
    sds log = sdscatprintf(sdsempty(),"<debug> line %d: ", ldb.currentline);
    while(argc--) {
        log = ldbCatStackValue(log,lua,-1 - argc);
        if (argc != 0) log = sdscatlen(log,", ",2);
    }
    ldbLog(log);
    return 0;
}

/* redis.set_repl()
 *
 * Set the propagation of write commands executed in the context of the
 * script to on/off for AOF and slaves. */
int luaRedisSetReplCommand(lua_State *lua) {
    int argc = lua_gettop(lua);
    int flags;

    if (server.lua_replicate_commands == 0) {
        lua_pushstring(lua, "You can set the replication behavior only after turning on single commands replication with redis.replicate_commands().");
        return lua_error(lua);
    } else if (argc != 1) {
        lua_pushstring(lua, "redis.set_repl() requires two arguments.");
        return lua_error(lua);
    }

    flags = lua_tonumber(lua,-1);
    if ((flags & ~(PROPAGATE_AOF|PROPAGATE_REPL)) != 0) {
        lua_pushstring(lua, "Invalid replication flags. Use REPL_AOF, REPL_SLAVE, REPL_ALL or REPL_NONE.");
        return lua_error(lua);
    }
    server.lua_repl = flags;
    return 0;
}

/* redis.log() */
int luaLogCommand(lua_State *lua) {
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

/* ---------------------------------------------------------------------------
 * Lua engine initialization and reset.
 * ------------------------------------------------------------------------- */

void luaLoadLib(lua_State *lua, const char *libname, lua_CFunction luafunc) {
  lua_pushcfunction(lua, luafunc);
  lua_pushstring(lua, libname);
  lua_call(lua, 1, 0);
}

LUALIB_API int (luaopen_cjson) (lua_State *L);
LUALIB_API int (luaopen_struct) (lua_State *L);
LUALIB_API int (luaopen_cmsgpack) (lua_State *L);
LUALIB_API int (luaopen_bit) (lua_State *L);

void luaLoadLibraries(lua_State *lua) {
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
void luaRemoveUnsupportedFunctions(lua_State *lua) {
    lua_pushnil(lua);
    lua_setglobal(lua,"loadfile");
    lua_pushnil(lua);
    lua_setglobal(lua,"dofile");
}

/* This function installs metamethods in the global table _G that prevent
 * the creation of globals accidentally.
 *
 * It should be the last to be called in the scripting engine initialization
 * sequence, because it may interact with creation of globals. */
void scriptingEnableGlobalsProtection(lua_State *lua) {
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
    s[j++]="    if w ~= \"main\" and w ~= \"C\" then\n";
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

/* Initialize the scripting environment.
 *
 * This function is called the first time at server startup with
 * the 'setup' argument set to 1.
 *
 * It can be called again multiple times during the lifetime of the Redis
 * process, with 'setup' set to 0, and following a scriptingRelease() call,
 * in order to reset the Lua scripting environment.
 *
 * However it is simpler to just call scriptingReset() that does just that. */
void scriptingInit(int setup) {
    lua_State *lua = lua_open();

    if (setup) {
        server.lua_client = NULL;
        server.lua_caller = NULL;
        server.lua_timedout = 0;
        server.lua_always_replicate_commands = 0; /* Only DEBUG can change it.*/
        ldbInit();
    }

    luaLoadLibraries(lua);
    luaRemoveUnsupportedFunctions(lua);

    /* Initialize a dictionary we use to map SHAs to scripts.
     * This is useful for replication, as we need to replicate EVALSHA
     * as EVAL, so we need to remember the associated script. */
    server.lua_scripts = dictCreate(&shaScriptObjectDictType,NULL);

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

    /* redis.replicate_commands */
    lua_pushstring(lua, "replicate_commands");
    lua_pushcfunction(lua, luaRedisReplicateCommandsCommand);
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

    lua_pushstring(lua,"REPL_ALL");
    lua_pushnumber(lua,PROPAGATE_AOF|PROPAGATE_REPL);
    lua_settable(lua,-3);

    /* redis.breakpoint */
    lua_pushstring(lua,"breakpoint");
    lua_pushcfunction(lua,luaRedisBreakpointCommand);
    lua_settable(lua,-3);

    /* redis.debug */
    lua_pushstring(lua,"debug");
    lua_pushcfunction(lua,luaRedisDebugCommand);
    lua_settable(lua,-3);

    /* Finally set the table as 'redis' global var. */
    lua_setglobal(lua,"redis");

    /* Replace math.random and math.randomseed with our implementations. */
    lua_getglobal(lua,"math");

    lua_pushstring(lua,"random");
    lua_pushcfunction(lua,redis_math_random);
    lua_settable(lua,-3);

    lua_pushstring(lua,"randomseed");
    lua_pushcfunction(lua,redis_math_randomseed);
    lua_settable(lua,-3);

    lua_setglobal(lua,"math");

    /* Add a helper function that we use to sort the multi bulk output of non
     * deterministic commands, when containing 'false' elements. */
    {
        char *compare_func =    "function __redis__compare_helper(a,b)\n"
                                "  if a == false then a = '' end\n"
                                "  if b == false then b = '' end\n"
                                "  return a<b\n"
                                "end\n";
        luaL_loadbuffer(lua,compare_func,strlen(compare_func),"@cmp_func_def");
        lua_pcall(lua,0,0,0);
    }

    /* Add a helper function we use for pcall error reporting.
     * Note that when the error is in the C function we want to report the
     * information about the caller, that's what makes sense from the point
     * of view of the user debugging a script. */
    {
        char *errh_func =       "local dbg = debug\n"
                                "function __redis__err__handler(err)\n"
                                "  local i = dbg.getinfo(2,'nSl')\n"
                                "  if i and i.what == 'C' then\n"
                                "    i = dbg.getinfo(3,'nSl')\n"
                                "  end\n"
                                "  if i then\n"
                                "    return i.source .. ':' .. i.currentline .. ': ' .. err\n"
                                "  else\n"
                                "    return err\n"
                                "  end\n"
                                "end\n";
        luaL_loadbuffer(lua,errh_func,strlen(errh_func),"@err_handler_def");
        lua_pcall(lua,0,0,0);
    }

    /* Create the (non connected) client that we use to execute Redis commands
     * inside the Lua interpreter.
     * Note: there is no need to create it again when this function is called
     * by scriptingReset(). */
    if (server.lua_client == NULL) {
        server.lua_client = createClient(-1);
        server.lua_client->flags |= CLIENT_LUA;
    }

    /* Lua beginners often don't use "local", this is likely to introduce
     * subtle bugs in their code. To prevent problems we protect accesses
     * to global variables. */
    scriptingEnableGlobalsProtection(lua);

    server.lua = lua;
}

/* Release resources related to Lua scripting.
 * This function is used in order to reset the scripting environment. */
void scriptingRelease(void) {
    dictRelease(server.lua_scripts);
    lua_close(server.lua);
}

void scriptingReset(void) {
    scriptingRelease();
    scriptingInit(0);
}

/* Set an array of Redis String Objects as a Lua array (table) stored into a
 * global variable. */
void luaSetGlobalArray(lua_State *lua, char *var, robj **elev, int elec) {
    int j;

    lua_newtable(lua);
    for (j = 0; j < elec; j++) {
        lua_pushlstring(lua,(char*)elev[j]->ptr,sdslen(elev[j]->ptr));
        lua_rawseti(lua,-2,j+1);
    }
    lua_setglobal(lua,var);
}

/* ---------------------------------------------------------------------------
 * Redis provided math.random
 * ------------------------------------------------------------------------- */

/* We replace math.random() with our implementation that is not affected
 * by specific libc random() implementations and will output the same sequence
 * (for the same seed) in every arch. */

/* The following implementation is the one shipped with Lua itself but with
 * rand() replaced by redisLrand48(). */
int redis_math_random (lua_State *L) {
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

int redis_math_randomseed (lua_State *L) {
  redisSrand48(luaL_checkint(L, 1));
  return 0;
}

/* ---------------------------------------------------------------------------
 * EVAL and SCRIPT commands implementation
 * ------------------------------------------------------------------------- */

/* Define a lua function with the specified function name and body.
 * The function name musts be a 42 characters long string, since all the
 * functions we defined in the Lua context are in the form:
 *
 *   f_<hex sha1 sum>
 *
 * On success C_OK is returned, and nothing is left on the Lua stack.
 * On error C_ERR is returned and an appropriate error is set in the
 * client context. */
int luaCreateFunction(client *c, lua_State *lua, char *funcname, robj *body) {
    sds funcdef = sdsempty();

    funcdef = sdscat(funcdef,"function ");
    funcdef = sdscatlen(funcdef,funcname,42);
    funcdef = sdscatlen(funcdef,"() ",3);
    funcdef = sdscatlen(funcdef,body->ptr,sdslen(body->ptr));
    funcdef = sdscatlen(funcdef,"\nend",4);

    if (luaL_loadbuffer(lua,funcdef,sdslen(funcdef),"@user_script")) {
        addReplyErrorFormat(c,"Error compiling script (new function): %s\n",
            lua_tostring(lua,-1));
        lua_pop(lua,1);
        sdsfree(funcdef);
        return C_ERR;
    }
    sdsfree(funcdef);
    if (lua_pcall(lua,0,0,0)) {
        addReplyErrorFormat(c,"Error running script (new function): %s\n",
            lua_tostring(lua,-1));
        lua_pop(lua,1);
        return C_ERR;
    }

    /* We also save a SHA1 -> Original script map in a dictionary
     * so that we can replicate / write in the AOF all the
     * EVALSHA commands as EVAL using the original script. */
    {
        int retval = dictAdd(server.lua_scripts,
                             sdsnewlen(funcname+2,40),body);
        serverAssertWithInfo(c,NULL,retval == DICT_OK);
        incrRefCount(body);
    }
    return C_OK;
}

/* This is the Lua script "count" hook that we use to detect scripts timeout. */
void luaMaskCountHook(lua_State *lua, lua_Debug *ar) {
    long long elapsed;
    UNUSED(ar);
    UNUSED(lua);

    elapsed = mstime() - server.lua_time_start;
    if (elapsed >= server.lua_time_limit && server.lua_timedout == 0) {
        serverLog(LL_WARNING,"Lua slow script detected: still in execution after %lld milliseconds. You can try killing the script using the SCRIPT KILL command.",elapsed);
        server.lua_timedout = 1;
        /* Once the script timeouts we reenter the event loop to permit others
         * to call SCRIPT KILL or SHUTDOWN NOSAVE if needed. For this reason
         * we need to mask the client executing the script from the event loop.
         * If we don't do that the client may disconnect and could no longer be
         * here when the EVAL command will return. */
         aeDeleteFileEvent(server.el, server.lua_caller->fd, AE_READABLE);
    }
    if (server.lua_timedout) processEventsWhileBlocked();
    if (server.lua_kill) {
        serverLog(LL_WARNING,"Lua script killed by user with SCRIPT KILL.");
        lua_pushstring(lua,"Script killed by user with SCRIPT KILL...");
        lua_error(lua);
    }
}

void evalGenericCommand(client *c, int evalsha) {
    lua_State *lua = server.lua;
    char funcname[43];
    long long numkeys;
    int delhook = 0, err;

    /* When we replicate whole scripts, we want the same PRNG sequence at
     * every call so that our PRNG is not affected by external state. */
    redisSrand48(0);

    /* We set this flag to zero to remember that so far no random command
     * was called. This way we can allow the user to call commands like
     * SRANDMEMBER or RANDOMKEY from Lua scripts as far as no write command
     * is called (otherwise the replication and AOF would end with non
     * deterministic sequences).
     *
     * Thanks to this flag we'll raise an error every time a write command
     * is called after a random command was used. */
    server.lua_random_dirty = 0;
    server.lua_write_dirty = 0;
    server.lua_replicate_commands = server.lua_always_replicate_commands;
    server.lua_multi_emitted = 0;
    server.lua_repl = PROPAGATE_AOF|PROPAGATE_REPL;

    /* Get the number of arguments that are keys */
    if (getLongLongFromObjectOrReply(c,c->argv[2],&numkeys,NULL) != C_OK)
        return;
    if (numkeys > (c->argc - 3)) {
        addReplyError(c,"Number of keys can't be greater than number of args");
        return;
    } else if (numkeys < 0) {
        addReplyError(c,"Number of keys can't be negative");
        return;
    }

    /* We obtain the script SHA1, then check if this function is already
     * defined into the Lua state */
    funcname[0] = 'f';
    funcname[1] = '_';
    if (!evalsha) {
        /* Hash the code if this is an EVAL call */
        sha1hex(funcname+2,c->argv[1]->ptr,sdslen(c->argv[1]->ptr));
    } else {
        /* We already have the SHA if it is a EVALSHA */
        int j;
        char *sha = c->argv[1]->ptr;

        /* Convert to lowercase. We don't use tolower since the function
         * managed to always show up in the profiler output consuming
         * a non trivial amount of time. */
        for (j = 0; j < 40; j++)
            funcname[j+2] = (sha[j] >= 'A' && sha[j] <= 'Z') ?
                sha[j]+('a'-'A') : sha[j];
        funcname[42] = '\0';
    }

    /* Push the pcall error handler function on the stack. */
    lua_getglobal(lua, "__redis__err__handler");

    /* Try to lookup the Lua function */
    lua_getglobal(lua, funcname);
    if (lua_isnil(lua,-1)) {
        lua_pop(lua,1); /* remove the nil from the stack */
        /* Function not defined... let's define it if we have the
         * body of the function. If this is an EVALSHA call we can just
         * return an error. */
        if (evalsha) {
            lua_pop(lua,1); /* remove the error handler from the stack. */
            addReply(c, shared.noscripterr);
            return;
        }
        if (luaCreateFunction(c,lua,funcname,c->argv[1]) == C_ERR) {
            lua_pop(lua,1); /* remove the error handler from the stack. */
            /* The error is sent to the client by luaCreateFunction()
             * itself when it returns C_ERR. */
            return;
        }
        /* Now the following is guaranteed to return non nil */
        lua_getglobal(lua, funcname);
        serverAssert(!lua_isnil(lua,-1));
    }

    /* Populate the argv and keys table accordingly to the arguments that
     * EVAL received. */
    luaSetGlobalArray(lua,"KEYS",c->argv+3,numkeys);
    luaSetGlobalArray(lua,"ARGV",c->argv+3+numkeys,c->argc-3-numkeys);

    /* Select the right DB in the context of the Lua client */
    selectDb(server.lua_client,c->db->id);

    /* Set a hook in order to be able to stop the script execution if it
     * is running for too much time.
     * We set the hook only if the time limit is enabled as the hook will
     * make the Lua script execution slower.
     *
     * If we are debugging, we set instead a "line" hook so that the
     * debugger is call-back at every line executed by the script. */
    server.lua_caller = c;
    server.lua_time_start = mstime();
    server.lua_kill = 0;
    if (server.lua_time_limit > 0 && server.masterhost == NULL &&
        ldb.active == 0)
    {
        lua_sethook(lua,luaMaskCountHook,LUA_MASKCOUNT,100000);
        delhook = 1;
    } else if (ldb.active) {
        lua_sethook(server.lua,luaLdbLineHook,LUA_MASKLINE|LUA_MASKCOUNT,100000);
        delhook = 1;
    }

    /* At this point whether this script was never seen before or if it was
     * already defined, we can call it. We have zero arguments and expect
     * a single return value. */
    err = lua_pcall(lua,0,1,-2);

    /* Perform some cleanup that we need to do both on error and success. */
    if (delhook) lua_sethook(lua,NULL,0,0); /* Disable hook */
    if (server.lua_timedout) {
        server.lua_timedout = 0;
        /* Restore the readable handler that was unregistered when the
         * script timeout was detected. */
        aeCreateFileEvent(server.el,c->fd,AE_READABLE,
                          readQueryFromClient,c);
    }
    server.lua_caller = NULL;

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
            funcname, lua_tostring(lua,-1));
        lua_pop(lua,2); /* Consume the Lua reply and remove error handler. */
    } else {
        /* On success convert the Lua return value into Redis protocol, and
         * send it to * the client. */
        luaReplyToRedisReply(c,lua); /* Convert and consume the reply. */
        lua_pop(lua,1); /* Remove the error handler. */
    }

    /* If we are using single commands replication, emit EXEC if there
     * was at least a write. */
    if (server.lua_replicate_commands) {
        preventCommandPropagation(c);
        if (server.lua_multi_emitted) {
            robj *propargv[1];
            propargv[0] = createStringObject("EXEC",4);
            alsoPropagate(server.execCommand,c->db->id,propargv,1,
                PROPAGATE_AOF|PROPAGATE_REPL);
            decrRefCount(propargv[0]);
        }
    }

    /* EVALSHA should be propagated to Slave and AOF file as full EVAL, unless
     * we are sure that the script was already in the context of all the
     * attached slaves *and* the current AOF file if enabled.
     *
     * To do so we use a cache of SHA1s of scripts that we already propagated
     * as full EVAL, that's called the Replication Script Cache.
     *
     * For repliation, everytime a new slave attaches to the master, we need to
     * flush our cache of scripts that can be replicated as EVALSHA, while
     * for AOF we need to do so every time we rewrite the AOF file. */
    if (evalsha && !server.lua_replicate_commands) {
        if (!replicationScriptCacheExists(c->argv[1]->ptr)) {
            /* This script is not in our script cache, replicate it as
             * EVAL, then add it into the script cache, as from now on
             * slaves and AOF know about it. */
            robj *script = dictFetchValue(server.lua_scripts,c->argv[1]->ptr);

            replicationScriptCacheAdd(c->argv[1]->ptr);
            serverAssertWithInfo(c,NULL,script != NULL);
            rewriteClientCommandArgument(c,0,
                resetRefCount(createStringObject("EVAL",4)));
            rewriteClientCommandArgument(c,1,script);
            forceCommandPropagation(c,PROPAGATE_REPL|PROPAGATE_AOF);
        }
    }
}

void evalCommand(client *c) {
    if (!(c->flags & CLIENT_LUA_DEBUG))
        evalGenericCommand(c,0);
    else
        evalGenericCommandWithDebugging(c,0);
}

void evalShaCommand(client *c) {
    if (sdslen(c->argv[1]->ptr) != 40) {
        /* We know that a match is not possible if the provided SHA is
         * not the right length. So we return an error ASAP, this way
         * evalGenericCommand() can be implemented without string length
         * sanity check */
        addReply(c, shared.noscripterr);
        return;
    }
    if (!(c->flags & CLIENT_LUA_DEBUG))
        evalGenericCommand(c,1);
    else {
        addReplyError(c,"Please use EVAL instead of EVALSHA for debugging");
        return;
    }
}

void scriptCommand(client *c) {
    if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"flush")) {
        scriptingReset();
        addReply(c,shared.ok);
        replicationScriptCacheFlush();
        server.dirty++; /* Propagating this command is a good idea. */
    } else if (c->argc >= 2 && !strcasecmp(c->argv[1]->ptr,"exists")) {
        int j;

        addReplyMultiBulkLen(c, c->argc-2);
        for (j = 2; j < c->argc; j++) {
            if (dictFind(server.lua_scripts,c->argv[j]->ptr))
                addReply(c,shared.cone);
            else
                addReply(c,shared.czero);
        }
    } else if (c->argc == 3 && !strcasecmp(c->argv[1]->ptr,"load")) {
        char funcname[43];
        sds sha;

        funcname[0] = 'f';
        funcname[1] = '_';
        sha1hex(funcname+2,c->argv[2]->ptr,sdslen(c->argv[2]->ptr));
        sha = sdsnewlen(funcname+2,40);
        if (dictFind(server.lua_scripts,sha) == NULL) {
            if (luaCreateFunction(c,server.lua,funcname,c->argv[2])
                    == C_ERR) {
                sdsfree(sha);
                return;
            }
        }
        addReplyBulkCBuffer(c,funcname+2,40);
        sdsfree(sha);
        forceCommandPropagation(c,PROPAGATE_REPL|PROPAGATE_AOF);
    } else if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"kill")) {
        if (server.lua_caller == NULL) {
            addReplySds(c,sdsnew("-NOTBUSY No scripts in execution right now.\r\n"));
        } else if (server.lua_write_dirty) {
            addReplySds(c,sdsnew("-UNKILLABLE Sorry the script already executed write commands against the dataset. You can either wait the script termination or kill the server in a hard way using the SHUTDOWN NOSAVE command.\r\n"));
        } else {
            server.lua_kill = 1;
            addReply(c,shared.ok);
        }
    } else if (c->argc == 3 && !strcasecmp(c->argv[1]->ptr,"debug")) {
        if (clientHasPendingReplies(c)) {
            addReplyError(c,"SCRIPT DEBUG must be called outside a pipeline");
            return;
        }
        if (!strcasecmp(c->argv[2]->ptr,"no")) {
            ldbDisable(c);
            addReply(c,shared.ok);
        } else if (!strcasecmp(c->argv[2]->ptr,"yes")) {
            ldbEnable(c);
            addReply(c,shared.ok);
        } else if (!strcasecmp(c->argv[2]->ptr,"sync")) {
            ldbEnable(c);
            addReply(c,shared.ok);
            c->flags |= CLIENT_LUA_DEBUG_SYNC;
        } else {
            addReplyError(c,"Use SCRIPT DEBUG yes/sync/no");
        }
    } else {
        addReplyError(c, "Unknown SCRIPT subcommand or wrong # of args.");
    }
}

/* ---------------------------------------------------------------------------
 * LDB: Redis Lua debugging facilities
 * ------------------------------------------------------------------------- */

/* Initialize Lua debugger data structures. */
void ldbInit(void) {
    ldb.fd = -1;
    ldb.active = 0;
    ldb.logs = listCreate();
    listSetFreeMethod(ldb.logs,(void (*)(void*))sdsfree);
    ldb.children = listCreate();
    ldb.src = NULL;
    ldb.lines = 0;
    ldb.cbuf = sdsempty();
}

/* Remove all the pending messages in the specified list. */
void ldbFlushLog(list *log) {
    listNode *ln;

    while((ln = listFirst(log)) != NULL)
        listDelNode(log,ln);
}

/* Enable debug mode of Lua scripts for this client. */
void ldbEnable(client *c) {
    c->flags |= CLIENT_LUA_DEBUG;
    ldbFlushLog(ldb.logs);
    ldb.fd = c->fd;
    ldb.step = 1;
    ldb.bpcount = 0;
    ldb.luabp = 0;
    sdsfree(ldb.cbuf);
    ldb.cbuf = sdsempty();
    ldb.maxlen = LDB_MAX_LEN_DEFAULT;
    ldb.maxlen_hint_sent = 0;
}

/* Exit debugging mode from the POV of client. This function is not enough
 * to properly shut down a client debugging session, see ldbEndSession()
 * for more information. */
void ldbDisable(client *c) {
    c->flags &= ~(CLIENT_LUA_DEBUG|CLIENT_LUA_DEBUG_SYNC);
}

/* Append a log entry to the specified LDB log. */
void ldbLog(sds entry) {
    listAddNodeTail(ldb.logs,entry);
}

/* A version of ldbLog() which prevents producing logs greater than
 * ldb.maxlen. The first time the limit is reached an hint is generated
 * to inform the user that reply trimming can be disabled using the
 * debugger "maxlen" command. */
void ldbLogWithMaxLen(sds entry) {
    int trimmed = 0;
    if (ldb.maxlen && sdslen(entry) > ldb.maxlen) {
        sdsrange(entry,0,ldb.maxlen-1);
        entry = sdscatlen(entry," ...",4);
        trimmed = 1;
    }
    ldbLog(entry);
    if (trimmed && ldb.maxlen_hint_sent == 0) {
        ldb.maxlen_hint_sent = 1;
        ldbLog(sdsnew(
        "<hint> The above reply was trimmed. Use 'maxlen 0' to disable trimming."));
    }
}

/* Send ldb.logs to the debugging client as a multi-bulk reply
 * consisting of simple strings. Log entries which include newlines have them
 * replaced with spaces. The entries sent are also consumed. */
void ldbSendLogs(void) {
    sds proto = sdsempty();
    proto = sdscatfmt(proto,"*%i\r\n", (int)listLength(ldb.logs));
    while(listLength(ldb.logs)) {
        listNode *ln = listFirst(ldb.logs);
        proto = sdscatlen(proto,"+",1);
        sdsmapchars(ln->value,"\r\n","  ",2);
        proto = sdscatsds(proto,ln->value);
        proto = sdscatlen(proto,"\r\n",2);
        listDelNode(ldb.logs,ln);
    }
    if (write(ldb.fd,proto,sdslen(proto)) == -1) {
        /* Avoid warning. We don't check the return value of write()
         * since the next read() will catch the I/O error and will
         * close the debugging session. */
    }
    sdsfree(proto);
}

/* Start a debugging session before calling EVAL implementation.
 * The techique we use is to capture the client socket file descriptor,
 * in order to perform direct I/O with it from within Lua hooks. This
 * way we don't have to re-enter Redis in order to handle I/O.
 *
 * The function returns 1 if the caller should proceed to call EVAL,
 * and 0 if instead the caller should abort the operation (this happens
 * for the parent in a forked session, since it's up to the children
 * to continue, or when fork returned an error).
 *
 * The caller should call ldbEndSession() only if ldbStartSession()
 * returned 1. */
int ldbStartSession(client *c) {
    ldb.forked = (c->flags & CLIENT_LUA_DEBUG_SYNC) == 0;
    if (ldb.forked) {
        pid_t cp = fork();
        if (cp == -1) {
            addReplyError(c,"Fork() failed: can't run EVAL in debugging mode.");
            return 0;
        } else if (cp == 0) {
            /* Child. Let's ignore important signals handled by the parent. */
            struct sigaction act;
            sigemptyset(&act.sa_mask);
            act.sa_flags = 0;
            act.sa_handler = SIG_IGN;
            sigaction(SIGTERM, &act, NULL);
            sigaction(SIGINT, &act, NULL);

            /* Log the creation of the child and close the listening
             * socket to make sure if the parent crashes a reset is sent
             * to the clients. */
            serverLog(LL_WARNING,"Redis forked for debugging eval");
            closeListeningSockets(0);
        } else {
            /* Parent */
            listAddNodeTail(ldb.children,(void*)(unsigned long)cp);
            freeClientAsync(c); /* Close the client in the parent side. */
            return 0;
        }
    } else {
        serverLog(LL_WARNING,
            "Redis synchronous debugging eval session started");
    }

    /* Setup our debugging session. */
    anetBlock(NULL,ldb.fd);
    anetSendTimeout(NULL,ldb.fd,5000);
    ldb.active = 1;

    /* First argument of EVAL is the script itself. We split it into different
     * lines since this is the way the debugger accesses the source code. */
    sds srcstring = sdsdup(c->argv[1]->ptr);
    size_t srclen = sdslen(srcstring);
    while(srclen && (srcstring[srclen-1] == '\n' ||
                     srcstring[srclen-1] == '\r'))
    {
        srcstring[--srclen] = '\0';
    }
    sdssetlen(srcstring,srclen);
    ldb.src = sdssplitlen(srcstring,sdslen(srcstring),"\n",1,&ldb.lines);
    sdsfree(srcstring);
    return 1;
}

/* End a debugging session after the EVAL call with debugging enabled
 * returned. */
void ldbEndSession(client *c) {
    /* Emit the remaining logs and an <endsession> mark. */
    ldbLog(sdsnew("<endsession>"));
    ldbSendLogs();

    /* If it's a fork()ed session, we just exit. */
    if (ldb.forked) {
        writeToClient(c->fd, c, 0);
        serverLog(LL_WARNING,"Lua debugging session child exiting");
        exitFromChild(0);
    } else {
        serverLog(LL_WARNING,
            "Redis synchronous debugging eval session ended");
    }

    /* Otherwise let's restore client's state. */
    anetNonBlock(NULL,ldb.fd);
    anetSendTimeout(NULL,ldb.fd,0);

    /* Close the client connectin after sending the final EVAL reply
     * in order to signal the end of the debugging session. */
    c->flags |= CLIENT_CLOSE_AFTER_REPLY;

    /* Cleanup. */
    sdsfreesplitres(ldb.src,ldb.lines);
    ldb.lines = 0;
    ldb.active = 0;
}

/* If the specified pid is among the list of children spawned for
 * forked debugging sessions, it is removed from the children list.
 * If the pid was found non-zero is returned. */
int ldbRemoveChild(pid_t pid) {
    listNode *ln = listSearchKey(ldb.children,(void*)(unsigned long)pid);
    if (ln) {
        listDelNode(ldb.children,ln);
        return 1;
    }
    return 0;
}

/* Return the number of children we still did not received termination
 * acknowledge via wait() in the parent process. */
int ldbPendingChildren(void) {
    return listLength(ldb.children);
}

/* Kill all the forked sessions. */
void ldbKillForkedSessions(void) {
    listIter li;
    listNode *ln;

    listRewind(ldb.children,&li);
    while((ln = listNext(&li))) {
        pid_t pid = (unsigned long) ln->value;
        serverLog(LL_WARNING,"Killing debugging session %ld",(long)pid);
        kill(pid,SIGKILL);
    }
    listRelease(ldb.children);
    ldb.children = listCreate();
}

/* Wrapper for EVAL / EVALSHA that enables debugging, and makes sure
 * that when EVAL returns, whatever happened, the session is ended. */
void evalGenericCommandWithDebugging(client *c, int evalsha) {
    if (ldbStartSession(c)) {
        evalGenericCommand(c,evalsha);
        ldbEndSession(c);
    } else {
        ldbDisable(c);
    }
}

/* Return a pointer to ldb.src source code line, considering line to be
 * one-based, and returning a special string for out of range lines. */
char *ldbGetSourceLine(int line) {
    int idx = line-1;
    if (idx < 0 || idx >= ldb.lines) return "<out of range source code line>";
    return ldb.src[idx];
}

/* Return true if there is a breakpoint in the specified line. */
int ldbIsBreakpoint(int line) {
    int j;

    for (j = 0; j < ldb.bpcount; j++)
        if (ldb.bp[j] == line) return 1;
    return 0;
}

/* Add the specified breakpoint. Ignore it if we already reached the max.
 * Returns 1 if the breakpoint was added (or was already set). 0 if there is
 * no space for the breakpoint or if the line is invalid. */
int ldbAddBreakpoint(int line) {
    if (line <= 0 || line > ldb.lines) return 0;
    if (!ldbIsBreakpoint(line) && ldb.bpcount != LDB_BREAKPOINTS_MAX) {
        ldb.bp[ldb.bpcount++] = line;
        return 1;
    }
    return 0;
}

/* Remove the specified breakpoint, returning 1 if the operation was
 * performed or 0 if there was no such breakpoint. */
int ldbDelBreakpoint(int line) {
    int j;

    for (j = 0; j < ldb.bpcount; j++) {
        if (ldb.bp[j] == line) {
            ldb.bpcount--;
            memmove(ldb.bp+j,ldb.bp+j+1,ldb.bpcount-j);
            return 1;
        }
    }
    return 0;
}

/* Expect a valid multi-bulk command in the debugging client query buffer.
 * On success the command is parsed and returned as an array of SDS strings,
 * otherwise NULL is returned and there is to read more buffer. */
sds *ldbReplParseCommand(int *argcp) {
    sds *argv = NULL;
    int argc = 0;
    if (sdslen(ldb.cbuf) == 0) return NULL;

    /* Working on a copy is simpler in this case. We can modify it freely
     * for the sake of simpler parsing. */
    sds copy = sdsdup(ldb.cbuf);
    char *p = copy;

    /* This Redis protocol parser is a joke... just the simplest thing that
     * works in this context. It is also very forgiving regarding broken
     * protocol. */

    /* Seek and parse *<count>\r\n. */
    p = strchr(p,'*'); if (!p) goto protoerr;
    char *plen = p+1; /* Multi bulk len pointer. */
    p = strstr(p,"\r\n"); if (!p) goto protoerr;
    *p = '\0'; p += 2;
    *argcp = atoi(plen);
    if (*argcp <= 0 || *argcp > 1024) goto protoerr;

    /* Parse each argument. */
    argv = zmalloc(sizeof(sds)*(*argcp));
    argc = 0;
    while(argc < *argcp) {
        if (*p != '$') goto protoerr;
        plen = p+1; /* Bulk string len pointer. */
        p = strstr(p,"\r\n"); if (!p) goto protoerr;
        *p = '\0'; p += 2;
        int slen = atoi(plen); /* Length of this arg. */
        if (slen <= 0 || slen > 1024) goto protoerr;
        argv[argc++] = sdsnewlen(p,slen);
        p += slen; /* Skip the already parsed argument. */
        if (p[0] != '\r' || p[1] != '\n') goto protoerr;
        p += 2; /* Skip \r\n. */
    }
    sdsfree(copy);
    return argv;

protoerr:
    sdsfreesplitres(argv,argc);
    sdsfree(copy);
    return NULL;
}

/* Log the specified line in the Lua debugger output. */
void ldbLogSourceLine(int lnum) {
    char *line = ldbGetSourceLine(lnum);
    char *prefix;
    int bp = ldbIsBreakpoint(lnum);
    int current = ldb.currentline == lnum;

    if (current && bp)
        prefix = "->#";
    else if (current)
        prefix = "-> ";
    else if (bp)
        prefix = "  #";
    else
        prefix = "   ";
    sds thisline = sdscatprintf(sdsempty(),"%s%-3d %s", prefix, lnum, line);
    ldbLog(thisline);
}

/* Implement the "list" command of the Lua debugger. If around is 0
 * the whole file is listed, otherwise only a small portion of the file
 * around the specified line is shown. When a line number is specified
 * the amonut of context (lines before/after) is specified via the
 * 'context' argument. */
void ldbList(int around, int context) {
    int j;

    for (j = 1; j <= ldb.lines; j++) {
        if (around != 0 && abs(around-j) > context) continue;
        ldbLogSourceLine(j);
    }
}

/* Append an human readable representation of the Lua value at position 'idx'
 * on the stack of the 'lua' state, to the SDS string passed as argument.
 * The new SDS string with the represented value attached is returned.
 * Used in order to implement ldbLogStackValue().
 *
 * The element is not automatically removed from the stack, nor it is
 * converted to a different type. */
#define LDB_MAX_VALUES_DEPTH (LUA_MINSTACK/2)
sds ldbCatStackValueRec(sds s, lua_State *lua, int idx, int level) {
    int t = lua_type(lua,idx);

    if (level++ == LDB_MAX_VALUES_DEPTH)
        return sdscat(s,"<max recursion level reached! Nested table?>");

    switch(t) {
    case LUA_TSTRING:
        {
        size_t strl;
        char *strp = (char*)lua_tolstring(lua,idx,&strl);
        s = sdscatrepr(s,strp,strl);
        }
        break;
    case LUA_TBOOLEAN:
        s = sdscat(s,lua_toboolean(lua,idx) ? "true" : "false");
        break;
    case LUA_TNUMBER:
        s = sdscatprintf(s,"%g",(double)lua_tonumber(lua,idx));
        break;
    case LUA_TNIL:
        s = sdscatlen(s,"nil",3);
        break;
    case LUA_TTABLE:
        {
        int expected_index = 1; /* First index we expect in an array. */
        int is_array = 1; /* Will be set to null if check fails. */
        /* Note: we create two representations at the same time, one
         * assuming the table is an array, one assuming it is not. At the
         * end we know what is true and select the right one. */
        sds repr1 = sdsempty();
        sds repr2 = sdsempty();
        lua_pushnil(lua); /* The first key to start the iteration is nil. */
        while (lua_next(lua,idx-1)) {
            /* Test if so far the table looks like an array. */
            if (is_array &&
                (lua_type(lua,-2) != LUA_TNUMBER ||
                 lua_tonumber(lua,-2) != expected_index)) is_array = 0;
            /* Stack now: table, key, value */
            /* Array repr. */
            repr1 = ldbCatStackValueRec(repr1,lua,-1,level);
            repr1 = sdscatlen(repr1,"; ",2);
            /* Full repr. */
            repr2 = sdscatlen(repr2,"[",1);
            repr2 = ldbCatStackValueRec(repr2,lua,-2,level);
            repr2 = sdscatlen(repr2,"]=",2);
            repr2 = ldbCatStackValueRec(repr2,lua,-1,level);
            repr2 = sdscatlen(repr2,"; ",2);
            lua_pop(lua,1); /* Stack: table, key. Ready for next iteration. */
            expected_index++;
        }
        /* Strip the last " ;" from both the representations. */
        if (sdslen(repr1)) sdsrange(repr1,0,-3);
        if (sdslen(repr2)) sdsrange(repr2,0,-3);
        /* Select the right one and discard the other. */
        s = sdscatlen(s,"{",1);
        s = sdscatsds(s,is_array ? repr1 : repr2);
        s = sdscatlen(s,"}",1);
        sdsfree(repr1);
        sdsfree(repr2);
        }
        break;
    case LUA_TFUNCTION:
    case LUA_TUSERDATA:
    case LUA_TTHREAD:
    case LUA_TLIGHTUSERDATA:
        {
        const void *p = lua_topointer(lua,idx);
        char *typename = "unknown";
        if (t == LUA_TFUNCTION) typename = "function";
        else if (t == LUA_TUSERDATA) typename = "userdata";
        else if (t == LUA_TTHREAD) typename = "thread";
        else if (t == LUA_TLIGHTUSERDATA) typename = "light-userdata";
        s = sdscatprintf(s,"\"%s@%p\"",typename,p);
        }
        break;
    default:
        s = sdscat(s,"\"<unknown-lua-type>\"");
        break;
    }
    return s;
}

/* Higher level wrapper for ldbCatStackValueRec() that just uses an initial
 * recursion level of '0'. */
sds ldbCatStackValue(sds s, lua_State *lua, int idx) {
    return ldbCatStackValueRec(s,lua,idx,0);
}

/* Produce a debugger log entry representing the value of the Lua object
 * currently on the top of the stack. The element is ot popped nor modified.
 * Check ldbCatStackValue() for the actual implementation. */
void ldbLogStackValue(lua_State *lua, char *prefix) {
    sds s = sdsnew(prefix);
    s = ldbCatStackValue(s,lua,-1);
    ldbLogWithMaxLen(s);
}

char *ldbRedisProtocolToHuman_Int(sds *o, char *reply);
char *ldbRedisProtocolToHuman_Bulk(sds *o, char *reply);
char *ldbRedisProtocolToHuman_Status(sds *o, char *reply);
char *ldbRedisProtocolToHuman_MultiBulk(sds *o, char *reply);

/* Get Redis protocol from 'reply' and appends it in human readable form to
 * the passed SDS string 'o'.
 *
 * Note that the SDS string is passed by reference (pointer of pointer to
 * char*) so that we can return a modified pointer, as for SDS semantics. */
char *ldbRedisProtocolToHuman(sds *o, char *reply) {
    char *p = reply;
    switch(*p) {
    case ':': p = ldbRedisProtocolToHuman_Int(o,reply); break;
    case '$': p = ldbRedisProtocolToHuman_Bulk(o,reply); break;
    case '+': p = ldbRedisProtocolToHuman_Status(o,reply); break;
    case '-': p = ldbRedisProtocolToHuman_Status(o,reply); break;
    case '*': p = ldbRedisProtocolToHuman_MultiBulk(o,reply); break;
    }
    return p;
}

/* The following functions are helpers for ldbRedisProtocolToHuman(), each
 * take care of a given Redis return type. */

char *ldbRedisProtocolToHuman_Int(sds *o, char *reply) {
    char *p = strchr(reply+1,'\r');
    *o = sdscatlen(*o,reply+1,p-reply-1);
    return p+2;
}

char *ldbRedisProtocolToHuman_Bulk(sds *o, char *reply) {
    char *p = strchr(reply+1,'\r');
    long long bulklen;

    string2ll(reply+1,p-reply-1,&bulklen);
    if (bulklen == -1) {
        *o = sdscatlen(*o,"NULL",4);
        return p+2;
    } else {
        *o = sdscatrepr(*o,p+2,bulklen);
        return p+2+bulklen+2;
    }
}

char *ldbRedisProtocolToHuman_Status(sds *o, char *reply) {
    char *p = strchr(reply+1,'\r');

    *o = sdscatrepr(*o,reply,p-reply);
    return p+2;
}

char *ldbRedisProtocolToHuman_MultiBulk(sds *o, char *reply) {
    char *p = strchr(reply+1,'\r');
    long long mbulklen;
    int j = 0;

    string2ll(reply+1,p-reply-1,&mbulklen);
    p += 2;
    if (mbulklen == -1) {
        *o = sdscatlen(*o,"NULL",4);
        return p;
    }
    *o = sdscatlen(*o,"[",1);
    for (j = 0; j < mbulklen; j++) {
        p = ldbRedisProtocolToHuman(o,p);
        if (j != mbulklen-1) *o = sdscatlen(*o,",",1);
    }
    *o = sdscatlen(*o,"]",1);
    return p;
}

/* Log a Redis reply as debugger output, in an human readable format.
 * If the resulting string is longer than 'len' plus a few more chars
 * used as prefix, it gets truncated. */
void ldbLogRedisReply(char *reply) {
    sds log = sdsnew("<reply> ");
    ldbRedisProtocolToHuman(&log,reply);
    ldbLogWithMaxLen(log);
}

/* Implements the "print <var>" command of the Lua debugger. It scans for Lua
 * var "varname" starting from the current stack frame up to the top stack
 * frame. The first matching variable is printed. */
void ldbPrint(lua_State *lua, char *varname) {
    lua_Debug ar;

    int l = 0; /* Stack level. */
    while (lua_getstack(lua,l,&ar) != 0) {
        l++;
        const char *name;
        int i = 1; /* Variable index. */
        while((name = lua_getlocal(lua,&ar,i)) != NULL) {
            i++;
            if (strcmp(varname,name) == 0) {
                ldbLogStackValue(lua,"<value> ");
                lua_pop(lua,1);
                return;
            } else {
                lua_pop(lua,1); /* Discard the var name on the stack. */
            }
        }
    }

    /* Let's try with global vars in two selected cases */
    if (!strcmp(varname,"ARGV") || !strcmp(varname,"KEYS")) {
        lua_getglobal(lua, varname);
        ldbLogStackValue(lua,"<value> ");
        lua_pop(lua,1);
    } else {
        ldbLog(sdsnew("No such variable."));
    }
}

/* Implements the "print" command (without arguments) of the Lua debugger.
 * Prints all the variables in the current stack frame. */
void ldbPrintAll(lua_State *lua) {
    lua_Debug ar;
    int vars = 0;

    if (lua_getstack(lua,0,&ar) != 0) {
        const char *name;
        int i = 1; /* Variable index. */
        while((name = lua_getlocal(lua,&ar,i)) != NULL) {
            i++;
            if (!strstr(name,"(*temporary)")) {
                sds prefix = sdscatprintf(sdsempty(),"<value> %s = ",name);
                ldbLogStackValue(lua,prefix);
                sdsfree(prefix);
                vars++;
            }
            lua_pop(lua,1);
        }
    }

    if (vars == 0) {
        ldbLog(sdsnew("No local variables in the current context."));
    }
}

/* Implements the break command to list, add and remove breakpoints. */
void ldbBreak(sds *argv, int argc) {
    if (argc == 1) {
        if (ldb.bpcount == 0) {
            ldbLog(sdsnew("No breakpoints set. Use 'b <line>' to add one."));
            return;
        } else {
            ldbLog(sdscatfmt(sdsempty(),"%i breakpoints set:",ldb.bpcount));
            int j;
            for (j = 0; j < ldb.bpcount; j++)
                ldbLogSourceLine(ldb.bp[j]);
        }
    } else {
        int j;
        for (j = 1; j < argc; j++) {
            char *arg = argv[j];
            long line;
            if (!string2l(arg,sdslen(arg),&line)) {
                ldbLog(sdscatfmt(sdsempty(),"Invalid argument:'%s'",arg));
            } else {
                if (line == 0) {
                    ldb.bpcount = 0;
                    ldbLog(sdsnew("All breakpoints removed."));
                } else if (line > 0) {
                    if (ldb.bpcount == LDB_BREAKPOINTS_MAX) {
                        ldbLog(sdsnew("Too many breakpoints set."));
                    } else if (ldbAddBreakpoint(line)) {
                        ldbList(line,1);
                    } else {
                        ldbLog(sdsnew("Wrong line number."));
                    }
                } else if (line < 0) {
                    if (ldbDelBreakpoint(-line))
                        ldbLog(sdsnew("Breakpoint removed."));
                    else
                        ldbLog(sdsnew("No breakpoint in the specified line."));
                }
            }
        }
    }
}

/* Implements the Lua debugger "eval" command. It just compiles the user
 * passed fragment of code and executes it, showing the result left on
 * the stack. */
void ldbEval(lua_State *lua, sds *argv, int argc) {
    /* Glue the script together if it is composed of multiple arguments. */
    sds code = sdsjoinsds(argv+1,argc-1," ",1);
    sds expr = sdscatsds(sdsnew("return "),code);

    /* Try to compile it as an expression, prepending "return ". */
    if (luaL_loadbuffer(lua,expr,sdslen(expr),"@ldb_eval")) {
        lua_pop(lua,1);
        /* Failed? Try as a statement. */
        if (luaL_loadbuffer(lua,code,sdslen(code),"@ldb_eval")) {
            ldbLog(sdscatfmt(sdsempty(),"<error> %s",lua_tostring(lua,-1)));
            lua_pop(lua,1);
            sdsfree(code);
            return;
        }
    }

    /* Call it. */
    sdsfree(code);
    sdsfree(expr);
    if (lua_pcall(lua,0,1,0)) {
        ldbLog(sdscatfmt(sdsempty(),"<error> %s",lua_tostring(lua,-1)));
        lua_pop(lua,1);
        return;
    }
    ldbLogStackValue(lua,"<retval> ");
    lua_pop(lua,1);
}

/* Implement the debugger "redis" command. We use a trick in order to make
 * the implementation very simple: we just call the Lua redis.call() command
 * implementation, with ldb.step enabled, so as a side effect the Redis command
 * and its reply are logged. */
void ldbRedis(lua_State *lua, sds *argv, int argc) {
    int j, saved_rc = server.lua_replicate_commands;

    lua_getglobal(lua,"redis");
    lua_pushstring(lua,"call");
    lua_gettable(lua,-2);       /* Stack: redis, redis.call */
    for (j = 1; j < argc; j++)
        lua_pushlstring(lua,argv[j],sdslen(argv[j]));
    ldb.step = 1;               /* Force redis.call() to log. */
    server.lua_replicate_commands = 1;
    lua_pcall(lua,argc-1,1,0);  /* Stack: redis, result */
    ldb.step = 0;               /* Disable logging. */
    server.lua_replicate_commands = saved_rc;
    lua_pop(lua,2);             /* Discard the result and clean the stack. */
}

/* Implements "trace" command of the Lua debugger. It just prints a backtrace
 * querying Lua starting from the current callframe back to the outer one. */
void ldbTrace(lua_State *lua) {
    lua_Debug ar;
    int level = 0;

    while(lua_getstack(lua,level,&ar)) {
        lua_getinfo(lua,"Snl",&ar);
        if(strstr(ar.short_src,"user_script") != NULL) {
            ldbLog(sdscatprintf(sdsempty(),"%s %s:",
                (level == 0) ? "In" : "From",
                ar.name ? ar.name : "top level"));
            ldbLogSourceLine(ar.currentline);
        }
        level++;
    }
    if (level == 0) {
        ldbLog(sdsnew("<error> Can't retrieve Lua stack."));
    }
}

/* Impleemnts the debugger "maxlen" command. It just queries or sets the
 * ldb.maxlen variable. */
void ldbMaxlen(sds *argv, int argc) {
    if (argc == 2) {
        int newval = atoi(argv[1]);
        ldb.maxlen_hint_sent = 1; /* User knows about this command. */
        if (newval != 0 && newval <= 60) newval = 60;
        ldb.maxlen = newval;
    }
    if (ldb.maxlen) {
        ldbLog(sdscatprintf(sdsempty(),"<value> replies are truncated at %d bytes.",(int)ldb.maxlen));
    } else {
        ldbLog(sdscatprintf(sdsempty(),"<value> replies are unlimited."));
    }
}

/* Read debugging commands from client.
 * Return C_OK if the debugging session is continuing, otherwise
 * C_ERR if the client closed the connection or is timing out. */
int ldbRepl(lua_State *lua) {
    sds *argv;
    int argc;

    /* We continue processing commands until a command that should return
     * to the Lua interpreter is found. */
    while(1) {
        while((argv = ldbReplParseCommand(&argc)) == NULL) {
            char buf[1024];
            int nread = read(ldb.fd,buf,sizeof(buf));
            if (nread <= 0) {
                /* Make sure the script runs without user input since the
                 * client is no longer connected. */
                ldb.step = 0;
                ldb.bpcount = 0;
                return C_ERR;
            }
            ldb.cbuf = sdscatlen(ldb.cbuf,buf,nread);
        }

        /* Flush the old buffer. */
        sdsfree(ldb.cbuf);
        ldb.cbuf = sdsempty();

        /* Execute the command. */
        if (!strcasecmp(argv[0],"h") || !strcasecmp(argv[0],"help")) {
ldbLog(sdsnew("Redis Lua debugger help:"));
ldbLog(sdsnew("[h]elp               Show this help."));
ldbLog(sdsnew("[s]tep               Run current line and stop again."));
ldbLog(sdsnew("[n]ext               Alias for step."));
ldbLog(sdsnew("[c]continue          Run till next breakpoint."));
ldbLog(sdsnew("[l]list              List source code around current line."));
ldbLog(sdsnew("[l]list [line]       List source code around [line]."));
ldbLog(sdsnew("                     line = 0 means: current position."));
ldbLog(sdsnew("[l]list [line] [ctx] In this form [ctx] specifies how many lines"));
ldbLog(sdsnew("                     to show before/after [line]."));
ldbLog(sdsnew("[w]hole              List all source code. Alias for 'list 1 1000000'."));
ldbLog(sdsnew("[p]rint              Show all the local variables."));
ldbLog(sdsnew("[p]rint <var>        Show the value of the specified variable."));
ldbLog(sdsnew("                     Can also show global vars KEYS and ARGV."));
ldbLog(sdsnew("[b]reak              Show all breakpoints."));
ldbLog(sdsnew("[b]reak <line>       Add a breakpoint to the specified line."));
ldbLog(sdsnew("[b]reak -<line>      Remove breakpoint from the specified line."));
ldbLog(sdsnew("[b]reak 0            Remove all breakpoints."));
ldbLog(sdsnew("[t]race              Show a backtrace."));
ldbLog(sdsnew("[e]eval <code>       Execute some Lua code (in a different callframe)."));
ldbLog(sdsnew("[r]edis <cmd>        Execute a Redis command."));
ldbLog(sdsnew("[m]axlen [len]       Trim logged Redis replies and Lua var dumps to len."));
ldbLog(sdsnew("                     Specifying zero as <len> means unlimited."));
ldbLog(sdsnew("[a]bort              Stop the execution of the script. In sync"));
ldbLog(sdsnew("                     mode dataset changes will be retained."));
ldbLog(sdsnew(""));
ldbLog(sdsnew("Debugger functions you can call from Lua scripts:"));
ldbLog(sdsnew("redis.debug()        Produce logs in the debugger console."));
ldbLog(sdsnew("redis.breakpoint()   Stop execution like if there was a breakpoing."));
ldbLog(sdsnew("                     in the next line of code."));
            ldbSendLogs();
        } else if (!strcasecmp(argv[0],"s") || !strcasecmp(argv[0],"step") ||
                   !strcasecmp(argv[0],"n") || !strcasecmp(argv[0],"next")) {
            ldb.step = 1;
            break;
        } else if (!strcasecmp(argv[0],"c") || !strcasecmp(argv[0],"continue")){
            break;
        } else if (!strcasecmp(argv[0],"t") || !strcasecmp(argv[0],"trace")) {
            ldbTrace(lua);
            ldbSendLogs();
        } else if (!strcasecmp(argv[0],"m") || !strcasecmp(argv[0],"maxlen")) {
            ldbMaxlen(argv,argc);
            ldbSendLogs();
        } else if (!strcasecmp(argv[0],"b") || !strcasecmp(argv[0],"break")) {
            ldbBreak(argv,argc);
            ldbSendLogs();
        } else if (!strcasecmp(argv[0],"e") || !strcasecmp(argv[0],"eval")) {
            ldbEval(lua,argv,argc);
            ldbSendLogs();
        } else if (!strcasecmp(argv[0],"a") || !strcasecmp(argv[0],"abort")) {
            lua_pushstring(lua, "script aborted for user request");
            lua_error(lua);
        } else if (argc > 1 &&
                   (!strcasecmp(argv[0],"r") || !strcasecmp(argv[0],"redis"))) {
            ldbRedis(lua,argv,argc);
            ldbSendLogs();
        } else if ((!strcasecmp(argv[0],"p") || !strcasecmp(argv[0],"print"))) {
            if (argc == 2)
                ldbPrint(lua,argv[1]);
            else
                ldbPrintAll(lua);
            ldbSendLogs();
        } else if (!strcasecmp(argv[0],"l") || !strcasecmp(argv[0],"list")){
            int around = ldb.currentline, ctx = 5;
            if (argc > 1) {
                int num = atoi(argv[1]);
                if (num > 0) around = num;
            }
            if (argc > 2) ctx = atoi(argv[2]);
            ldbList(around,ctx);
            ldbSendLogs();
        } else if (!strcasecmp(argv[0],"w") || !strcasecmp(argv[0],"whole")){
            ldbList(1,1000000);
            ldbSendLogs();
        } else {
            ldbLog(sdsnew("<error> Unknown Redis Lua debugger command or "
                          "wrong number of arguments."));
            ldbSendLogs();
        }

        /* Free the command vector. */
        sdsfreesplitres(argv,argc);
    }

    /* Free the current command argv if we break inside the while loop. */
    sdsfreesplitres(argv,argc);
    return C_OK;
}

/* This is the core of our Lua debugger, called each time Lua is about
 * to start executing a new line. */
void luaLdbLineHook(lua_State *lua, lua_Debug *ar) {
    lua_getstack(lua,0,ar);
    lua_getinfo(lua,"Sl",ar);
    ldb.currentline = ar->currentline;

    int bp = ldbIsBreakpoint(ldb.currentline) || ldb.luabp;
    int timeout = 0;

    /* Events outside our script are not interesting. */
    if(strstr(ar->short_src,"user_script") == NULL) return;

    /* Check if a timeout occurred. */
    if (ar->event == LUA_HOOKCOUNT && ldb.step == 0 && bp == 0) {
        mstime_t elapsed = mstime() - server.lua_time_start;
        mstime_t timelimit = server.lua_time_limit ?
                             server.lua_time_limit : 5000;
        if (elapsed >= timelimit) {
            timeout = 1;
            ldb.step = 1;
        } else {
            return; /* No timeout, ignore the COUNT event. */
        }
    }

    if (ldb.step || bp) {
        char *reason = "step over";
        if (bp) reason = ldb.luabp ? "redis.breakpoint() called" :
                                     "break point";
        else if (timeout) reason = "timeout reached, infinite loop?";
        ldb.step = 0;
        ldb.luabp = 0;
        ldbLog(sdscatprintf(sdsempty(),
            "* Stopped at %d, stop reason = %s",
            ldb.currentline, reason));
        ldbLogSourceLine(ldb.currentline);
        ldbSendLogs();
        if (ldbRepl(lua) == C_ERR && timeout) {
            /* If the client closed the connection and we have a timeout
             * connection, let's kill the script otherwise the process
             * will remain blocked indefinitely. */
            lua_pushstring(lua, "timeout during Lua debugging with client closing connection");
            lua_error(lua);
        }
        server.lua_time_start = mstime();
    }
}

