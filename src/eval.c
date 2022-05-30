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
#include "monotonic.h"
#include "resp_parser.h"
#include "script_lua.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <ctype.h>
#include <math.h>

void ldbInit(void);
void ldbDisable(client *c);
void ldbEnable(client *c);
void evalGenericCommandWithDebugging(client *c, int evalsha);
sds ldbCatStackValue(sds s, lua_State *lua, int idx);

static void dictLuaScriptDestructor(dict *d, void *val) {
    UNUSED(d);
    if (val == NULL) return; /* Lazy freeing will set value to NULL. */
    decrRefCount(((luaScript*)val)->body);
    zfree(val);
}

static uint64_t dictStrCaseHash(const void *key) {
    return dictGenCaseHashFunction((unsigned char*)key, strlen((char*)key));
}

/* server.lua_scripts sha (as sds string) -> scripts (as luaScript) cache. */
dictType shaScriptObjectDictType = {
        dictStrCaseHash,            /* hash function */
        NULL,                       /* key dup */
        NULL,                       /* val dup */
        dictSdsKeyCaseCompare,      /* key compare */
        dictSdsDestructor,          /* key destructor */
        dictLuaScriptDestructor,    /* val destructor */
        NULL                        /* allow to expand */
};

/* Lua context */
struct luaCtx {
    lua_State *lua; /* The Lua interpreter. We use just one for all clients */
    client *lua_client;   /* The "fake client" to query Redis from Lua */
    dict *lua_scripts;         /* A dictionary of SHA1 -> Lua scripts */
    unsigned long long lua_scripts_mem;  /* Cached scripts' memory + oh */
} lctx;

/* Debugger shared state is stored inside this global structure. */
#define LDB_BREAKPOINTS_MAX 64  /* Max number of breakpoints. */
#define LDB_MAX_LEN_DEFAULT 256 /* Default len limit for replies / var dumps. */
struct ldbState {
    connection *conn; /* Connection of the debugging client. */
    int active; /* Are we debugging EVAL right now? */
    int forked; /* Is this a fork()ed debugging session? */
    list *logs; /* List of messages to send to the client. */
    list *traces; /* Messages about Redis commands executed since last stop.*/
    list *children; /* All forked debugging sessions pids. */
    int bp[LDB_BREAKPOINTS_MAX]; /* An array of breakpoints line numbers. */
    int bpcount; /* Number of valid entries inside bp. */
    int step;   /* Stop at next line regardless of breakpoints. */
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

/* redis.breakpoint()
 *
 * Allows to stop execution during a debugging session from within
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

/* redis.replicate_commands()
 *
 * DEPRECATED: Now do nothing and always return true.
 * Turn on single commands replication if the script never called
 * a write command so far, and returns true. Otherwise if the script
 * already started to write, returns false and stick to whole scripts
 * replication, which is our default. */
int luaRedisReplicateCommandsCommand(lua_State *lua) {
    lua_pushboolean(lua,1);
    return 1;
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
        lctx.lua_client = NULL;
        server.script_caller = NULL;
        server.script_disable_deny_script = 0;
        ldbInit();
    }

    /* Initialize a dictionary we use to map SHAs to scripts.
     * This is useful for replication, as we need to replicate EVALSHA
     * as EVAL, so we need to remember the associated script. */
    lctx.lua_scripts = dictCreate(&shaScriptObjectDictType);
    lctx.lua_scripts_mem = 0;

    luaRegisterRedisAPI(lua);

    /* register debug commands */
    lua_getglobal(lua,"redis");

    /* redis.breakpoint */
    lua_pushstring(lua,"breakpoint");
    lua_pushcfunction(lua,luaRedisBreakpointCommand);
    lua_settable(lua,-3);

    /* redis.debug */
    lua_pushstring(lua,"debug");
    lua_pushcfunction(lua,luaRedisDebugCommand);
    lua_settable(lua,-3);

    /* redis.replicate_commands */
    lua_pushstring(lua, "replicate_commands");
    lua_pushcfunction(lua, luaRedisReplicateCommandsCommand);
    lua_settable(lua, -3);

    lua_setglobal(lua,"redis");

    /* Add a helper function we use for pcall error reporting.
     * Note that when the error is in the C function we want to report the
     * information about the caller, that's what makes sense from the point
     * of view of the user debugging a script. */
    {
        char *errh_func =       "local dbg = debug\n"
                                "debug = nil\n"
                                "function __redis__err__handler(err)\n"
                                "  local i = dbg.getinfo(2,'nSl')\n"
                                "  if i and i.what == 'C' then\n"
                                "    i = dbg.getinfo(3,'nSl')\n"
                                "  end\n"
                                "  if type(err) ~= 'table' then\n"
                                "    err = {err='ERR ' .. tostring(err)}"
                                "  end"
                                "  if i then\n"
                                "    err['source'] = i.source\n"
                                "    err['line'] = i.currentline\n"
                                "  end"
                                "  return err\n"
                                "end\n";
        luaL_loadbuffer(lua,errh_func,strlen(errh_func),"@err_handler_def");
        lua_pcall(lua,0,0,0);
    }

    /* Create the (non connected) client that we use to execute Redis commands
     * inside the Lua interpreter.
     * Note: there is no need to create it again when this function is called
     * by scriptingReset(). */
    if (lctx.lua_client == NULL) {
        lctx.lua_client = createClient(NULL);
        lctx.lua_client->flags |= CLIENT_SCRIPT;

        /* We do not want to allow blocking commands inside Lua */
        lctx.lua_client->flags |= CLIENT_DENY_BLOCKING;
    }

    /* Lock the global table from any changes */
    lua_pushvalue(lua, LUA_GLOBALSINDEX);
    luaSetErrorMetatable(lua);
    /* Recursively lock all tables that can be reached from the global table */
    luaSetTableProtectionRecursively(lua);
    lua_pop(lua, 1);

    lctx.lua = lua;
}

/* Release resources related to Lua scripting.
 * This function is used in order to reset the scripting environment. */
void scriptingRelease(int async) {
    if (async)
        freeLuaScriptsAsync(lctx.lua_scripts);
    else
        dictRelease(lctx.lua_scripts);
    lctx.lua_scripts_mem = 0;
    lua_close(lctx.lua);
}

void scriptingReset(int async) {
    scriptingRelease(async);
    scriptingInit(0);
}

/* ---------------------------------------------------------------------------
 * EVAL and SCRIPT commands implementation
 * ------------------------------------------------------------------------- */

static void evalCalcFunctionName(int evalsha, sds script, char *out_funcname) {
    /* We obtain the script SHA1, then check if this function is already
     * defined into the Lua state */
    out_funcname[0] = 'f';
    out_funcname[1] = '_';
    if (!evalsha) {
        /* Hash the code if this is an EVAL call */
        sha1hex(out_funcname+2,script,sdslen(script));
    } else {
        /* We already have the SHA if it is an EVALSHA */
        int j;
        char *sha = script;

        /* Convert to lowercase. We don't use tolower since the function
         * managed to always show up in the profiler output consuming
         * a non trivial amount of time. */
        for (j = 0; j < 40; j++)
            out_funcname[j+2] = (sha[j] >= 'A' && sha[j] <= 'Z') ?
                sha[j]+('a'-'A') : sha[j];
        out_funcname[42] = '\0';
    }
}

/* Helper function to try and extract shebang flags from the script body.
 * If no shebang is found, return with success and COMPAT mode flag.
 * The err arg is optional, can be used to get a detailed error string.
 * The out_shebang_len arg is optional, can be used to trim the shebang from the script.
 * Returns C_OK on success, and C_ERR on error. */
int evalExtractShebangFlags(sds body, uint64_t *out_flags, ssize_t *out_shebang_len, sds *err) {
    ssize_t shebang_len = 0;
    uint64_t script_flags = SCRIPT_FLAG_EVAL_COMPAT_MODE;
    if (!strncmp(body, "#!", 2)) {
        int numparts,j;
        char *shebang_end = strchr(body, '\n');
        if (shebang_end == NULL) {
            if (err)
                *err = sdsnew("Invalid script shebang");
            return C_ERR;
        }
        shebang_len = shebang_end - body;
        sds shebang = sdsnewlen(body, shebang_len);
        sds *parts = sdssplitargs(shebang, &numparts);
        sdsfree(shebang);
        if (!parts || numparts == 0) {
            if (err)
                *err = sdsnew("Invalid engine in script shebang");
            sdsfreesplitres(parts, numparts);
            return C_ERR;
        }
        /* Verify lua interpreter was specified */
        if (strcmp(parts[0], "#!lua")) {
            if (err)
                *err = sdscatfmt(sdsempty(), "Unexpected engine in script shebang: %s", parts[0]);
            sdsfreesplitres(parts, numparts);
            return C_ERR;
        }
        script_flags &= ~SCRIPT_FLAG_EVAL_COMPAT_MODE;
        for (j = 1; j < numparts; j++) {
            if (!strncmp(parts[j], "flags=", 6)) {
                sdsrange(parts[j], 6, -1);
                int numflags, jj;
                sds *flags = sdssplitlen(parts[j], sdslen(parts[j]), ",", 1, &numflags);
                for (jj = 0; jj < numflags; jj++) {
                    scriptFlag *sf;
                    for (sf = scripts_flags_def; sf->flag; sf++) {
                        if (!strcmp(flags[jj], sf->str)) break;
                    }
                    if (!sf->flag) {
                        if (err)
                            *err = sdscatfmt(sdsempty(), "Unexpected flag in script shebang: %s", flags[jj]);
                        sdsfreesplitres(flags, numflags);
                        sdsfreesplitres(parts, numparts);
                        return C_ERR;
                    }
                    script_flags |= sf->flag;
                }
                sdsfreesplitres(flags, numflags);
            } else {
                /* We only support function flags options for lua scripts */
                if (err)
                    *err = sdscatfmt(sdsempty(), "Unknown lua shebang option: %s", parts[j]);
                sdsfreesplitres(parts, numparts);
                return C_ERR;
            }
        }
        sdsfreesplitres(parts, numparts);
    }
    if (out_shebang_len)
        *out_shebang_len = shebang_len;
    *out_flags = script_flags;
    return C_OK;
}

/* Try to extract command flags if we can, returns the modified flags.
 * Note that it does not guarantee the command arguments are right. */
uint64_t evalGetCommandFlags(client *c, uint64_t cmd_flags) {
    char funcname[43];
    int evalsha = c->cmd->proc == evalShaCommand || c->cmd->proc == evalShaRoCommand;
    if (evalsha && sdslen(c->argv[1]->ptr) != 40)
        return cmd_flags;
    evalCalcFunctionName(evalsha, c->argv[1]->ptr, funcname);
    char *lua_cur_script = funcname + 2;
    dictEntry *de = dictFind(lctx.lua_scripts, lua_cur_script);
    uint64_t script_flags;
    if (!de) {
        if (evalsha)
            return cmd_flags;
        if (evalExtractShebangFlags(c->argv[1]->ptr, &script_flags, NULL, NULL) == C_ERR)
            return cmd_flags;
    } else {
        luaScript *l = dictGetVal(de);
        script_flags = l->flags;
    }
    if (script_flags & SCRIPT_FLAG_EVAL_COMPAT_MODE)
        return cmd_flags;
    return scriptFlagsToCmdFlags(cmd_flags, script_flags);
}

/* Define a Lua function with the specified body.
 * The function name will be generated in the following form:
 *
 *   f_<hex sha1 sum>
 *
 * The function increments the reference count of the 'body' object as a
 * side effect of a successful call.
 *
 * On success a pointer to an SDS string representing the function SHA1 of the
 * just added function is returned (and will be valid until the next call
 * to scriptingReset() function), otherwise NULL is returned.
 *
 * The function handles the fact of being called with a script that already
 * exists, and in such a case, it behaves like in the success case.
 *
 * If 'c' is not NULL, on error the client is informed with an appropriate
 * error describing the nature of the problem and the Lua interpreter error. */
sds luaCreateFunction(client *c, robj *body) {
    char funcname[43];
    dictEntry *de;
    uint64_t script_flags;

    funcname[0] = 'f';
    funcname[1] = '_';
    sha1hex(funcname+2,body->ptr,sdslen(body->ptr));

    if ((de = dictFind(lctx.lua_scripts,funcname+2)) != NULL) {
        return dictGetKey(de);
    }

    /* Handle shebang header in script code */
    ssize_t shebang_len = 0;
    sds err = NULL;
    if (evalExtractShebangFlags(body->ptr, &script_flags, &shebang_len, &err) == C_ERR) {
        addReplyErrorSds(c, err);
        return NULL;
    }

    /* Note that in case of a shebang line we skip it but keep the line feed to conserve the user's line numbers */
    if (luaL_loadbuffer(lctx.lua,(char*)body->ptr + shebang_len,sdslen(body->ptr) - shebang_len,"@user_script")) {
        if (c != NULL) {
            addReplyErrorFormat(c,
                "Error compiling script (new function): %s",
                lua_tostring(lctx.lua,-1));
        }
        lua_pop(lctx.lua,1);
        return NULL;
    }

    serverAssert(lua_isfunction(lctx.lua, -1));

    lua_setfield(lctx.lua, LUA_REGISTRYINDEX, funcname);

    /* We also save a SHA1 -> Original script map in a dictionary
     * so that we can replicate / write in the AOF all the
     * EVALSHA commands as EVAL using the original script. */
    luaScript *l = zcalloc(sizeof(luaScript));
    l->body = body;
    l->flags = script_flags;
    sds sha = sdsnewlen(funcname+2,40);
    int retval = dictAdd(lctx.lua_scripts,sha,l);
    serverAssertWithInfo(c ? c : lctx.lua_client,NULL,retval == DICT_OK);
    lctx.lua_scripts_mem += sdsZmallocSize(sha) + getStringObjectSdsUsedMemory(body);
    incrRefCount(body);
    return sha;
}

void prepareLuaClient(void) {
    /* Select the right DB in the context of the Lua client */
    selectDb(lctx.lua_client,server.script_caller->db->id);
    lctx.lua_client->resp = 2; /* Default is RESP2, scripts can change it. */

    /* If we are in MULTI context, flag Lua client as CLIENT_MULTI. */
    if (server.script_caller->flags & CLIENT_MULTI) {
        lctx.lua_client->flags |= CLIENT_MULTI;
    }
}

void resetLuaClient(void) {
    /* After the script done, remove the MULTI state. */
    lctx.lua_client->flags &= ~CLIENT_MULTI;
}

void evalGenericCommand(client *c, int evalsha) {
    lua_State *lua = lctx.lua;
    char funcname[43];
    long long numkeys;

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

    evalCalcFunctionName(evalsha, c->argv[1]->ptr, funcname);

    /* Push the pcall error handler function on the stack. */
    lua_getglobal(lua, "__redis__err__handler");

    /* Try to lookup the Lua function */
    lua_getfield(lua, LUA_REGISTRYINDEX, funcname);
    if (lua_isnil(lua,-1)) {
        lua_pop(lua,1); /* remove the nil from the stack */
        /* Function not defined... let's define it if we have the
         * body of the function. If this is an EVALSHA call we can just
         * return an error. */
        if (evalsha) {
            lua_pop(lua,1); /* remove the error handler from the stack. */
            addReplyErrorObject(c, shared.noscripterr);
            return;
        }
        if (luaCreateFunction(c,c->argv[1]) == NULL) {
            lua_pop(lua,1); /* remove the error handler from the stack. */
            /* The error is sent to the client by luaCreateFunction()
             * itself when it returns NULL. */
            return;
        }
        /* Now the following is guaranteed to return non nil */
        lua_getfield(lua, LUA_REGISTRYINDEX, funcname);
        serverAssert(!lua_isnil(lua,-1));
    }

    char *lua_cur_script = funcname + 2;
    dictEntry *de = dictFind(lctx.lua_scripts, lua_cur_script);
    luaScript *l = dictGetVal(de);
    int ro = c->cmd->proc == evalRoCommand || c->cmd->proc == evalShaRoCommand;

    scriptRunCtx rctx;
    if (scriptPrepareForRun(&rctx, lctx.lua_client, c, lua_cur_script, l->flags, ro) != C_OK) {
        lua_pop(lua,2); /* Remove the function and error handler. */
        return;
    }
    rctx.flags |= SCRIPT_EVAL_MODE; /* mark the current run as EVAL (as opposed to FCALL) so we'll
                                      get appropriate error messages and logs */

    luaCallFunction(&rctx, lua, c->argv+3, numkeys, c->argv+3+numkeys, c->argc-3-numkeys, ldb.active);
    lua_pop(lua,1); /* Remove the error handler. */
    scriptResetRun(&rctx);
}

void evalCommand(client *c) {
    /* Explicitly feed monitor here so that lua commands appear after their
     * script command. */
    replicationFeedMonitors(c,server.monitors,c->db->id,c->argv,c->argc);
    if (!(c->flags & CLIENT_LUA_DEBUG))
        evalGenericCommand(c,0);
    else
        evalGenericCommandWithDebugging(c,0);
}

void evalRoCommand(client *c) {
    evalCommand(c);
}

void evalShaCommand(client *c) {
    /* Explicitly feed monitor here so that lua commands appear after their
     * script command. */
    replicationFeedMonitors(c,server.monitors,c->db->id,c->argv,c->argc);
    if (sdslen(c->argv[1]->ptr) != 40) {
        /* We know that a match is not possible if the provided SHA is
         * not the right length. So we return an error ASAP, this way
         * evalGenericCommand() can be implemented without string length
         * sanity check */
        addReplyErrorObject(c, shared.noscripterr);
        return;
    }
    if (!(c->flags & CLIENT_LUA_DEBUG))
        evalGenericCommand(c,1);
    else {
        addReplyError(c,"Please use EVAL instead of EVALSHA for debugging");
        return;
    }
}

void evalShaRoCommand(client *c) {
    evalShaCommand(c);
}

void scriptCommand(client *c) {
    if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"help")) {
        const char *help[] = {
"DEBUG (YES|SYNC|NO)",
"    Set the debug mode for subsequent scripts executed.",
"EXISTS <sha1> [<sha1> ...]",
"    Return information about the existence of the scripts in the script cache.",
"FLUSH [ASYNC|SYNC]",
"    Flush the Lua scripts cache. Very dangerous on replicas.",
"    When called without the optional mode argument, the behavior is determined by the",
"    lazyfree-lazy-user-flush configuration directive. Valid modes are:",
"    * ASYNC: Asynchronously flush the scripts cache.",
"    * SYNC: Synchronously flush the scripts cache.",
"KILL",
"    Kill the currently executing Lua script.",
"LOAD <script>",
"    Load a script into the scripts cache without executing it.",
NULL
        };
        addReplyHelp(c, help);
    } else if (c->argc >= 2 && !strcasecmp(c->argv[1]->ptr,"flush")) {
        int async = 0;
        if (c->argc == 3 && !strcasecmp(c->argv[2]->ptr,"sync")) {
            async = 0;
        } else if (c->argc == 3 && !strcasecmp(c->argv[2]->ptr,"async")) {
            async = 1;
        } else if (c->argc == 2) {
            async = server.lazyfree_lazy_user_flush ? 1 : 0;
        } else {
            addReplyError(c,"SCRIPT FLUSH only support SYNC|ASYNC option");
            return;
        }
        scriptingReset(async);
        addReply(c,shared.ok);
    } else if (c->argc >= 2 && !strcasecmp(c->argv[1]->ptr,"exists")) {
        int j;

        addReplyArrayLen(c, c->argc-2);
        for (j = 2; j < c->argc; j++) {
            if (dictFind(lctx.lua_scripts,c->argv[j]->ptr))
                addReply(c,shared.cone);
            else
                addReply(c,shared.czero);
        }
    } else if (c->argc == 3 && !strcasecmp(c->argv[1]->ptr,"load")) {
        sds sha = luaCreateFunction(c,c->argv[2]);
        if (sha == NULL) return; /* The error was sent by luaCreateFunction(). */
        addReplyBulkCBuffer(c,sha,40);
    } else if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"kill")) {
        scriptKill(c, 1);
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
            addReplyError(c,"Use SCRIPT DEBUG YES/SYNC/NO");
            return;
        }
    } else {
        addReplySubcommandSyntaxError(c);
    }
}

unsigned long evalMemory() {
    return luaMemory(lctx.lua);
}

dict* evalScriptsDict() {
    return lctx.lua_scripts;
}

unsigned long evalScriptsMemory() {
    return lctx.lua_scripts_mem +
            dictSize(lctx.lua_scripts) * (sizeof(dictEntry) + sizeof(luaScript)) +
            dictSlots(lctx.lua_scripts) * sizeof(dictEntry*);
}

/* ---------------------------------------------------------------------------
 * LDB: Redis Lua debugging facilities
 * ------------------------------------------------------------------------- */

/* Initialize Lua debugger data structures. */
void ldbInit(void) {
    ldb.conn = NULL;
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

int ldbIsEnabled(){
    return ldb.active && ldb.step;
}

/* Enable debug mode of Lua scripts for this client. */
void ldbEnable(client *c) {
    c->flags |= CLIENT_LUA_DEBUG;
    ldbFlushLog(ldb.logs);
    ldb.conn = c->conn;
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
 * ldb.maxlen. The first time the limit is reached a hint is generated
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
    if (connWrite(ldb.conn,proto,sdslen(proto)) == -1) {
        /* Avoid warning. We don't check the return value of write()
         * since the next read() will catch the I/O error and will
         * close the debugging session. */
    }
    sdsfree(proto);
}

/* Start a debugging session before calling EVAL implementation.
 * The technique we use is to capture the client socket file descriptor,
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
        pid_t cp = redisFork(CHILD_TYPE_LDB);
        if (cp == -1) {
            addReplyErrorFormat(c,"Fork() failed: can't run EVAL in debugging mode: %s", strerror(errno));
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
    connBlock(ldb.conn);
    connSendTimeout(ldb.conn,5000);
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
        writeToClient(c,0);
        serverLog(LL_WARNING,"Lua debugging session child exiting");
        exitFromChild(0);
    } else {
        serverLog(LL_WARNING,
            "Redis synchronous debugging eval session ended");
    }

    /* Otherwise let's restore client's state. */
    connNonBlock(ldb.conn);
    connSendTimeout(ldb.conn,0);

    /* Close the client connection after sending the final EVAL reply
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

/* Return the number of children we still did not receive termination
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
sds *ldbReplParseCommand(int *argcp, char** err) {
    static char* protocol_error = "protocol error";
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
    p = strstr(p,"\r\n"); if (!p) goto keep_reading;
    *p = '\0'; p += 2;
    *argcp = atoi(plen);
    if (*argcp <= 0 || *argcp > 1024) goto protoerr;

    /* Parse each argument. */
    argv = zmalloc(sizeof(sds)*(*argcp));
    argc = 0;
    while(argc < *argcp) {
        /* reached the end but there should be more data to read */
        if (*p == '\0') goto keep_reading;

        if (*p != '$') goto protoerr;
        plen = p+1; /* Bulk string len pointer. */
        p = strstr(p,"\r\n"); if (!p) goto keep_reading;
        *p = '\0'; p += 2;
        int slen = atoi(plen); /* Length of this arg. */
        if (slen <= 0 || slen > 1024) goto protoerr;
        if ((size_t)(p + slen + 2 - copy) > sdslen(copy) ) goto keep_reading;
        argv[argc++] = sdsnewlen(p,slen);
        p += slen; /* Skip the already parsed argument. */
        if (p[0] != '\r' || p[1] != '\n') goto protoerr;
        p += 2; /* Skip \r\n. */
    }
    sdsfree(copy);
    return argv;

protoerr:
    *err = protocol_error;
keep_reading:
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
 * the amount of context (lines before/after) is specified via the
 * 'context' argument. */
void ldbList(int around, int context) {
    int j;

    for (j = 1; j <= ldb.lines; j++) {
        if (around != 0 && abs(around-j) > context) continue;
        ldbLogSourceLine(j);
    }
}

/* Append a human readable representation of the Lua value at position 'idx'
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
 * currently on the top of the stack. The element is not popped nor modified.
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
char *ldbRedisProtocolToHuman_Set(sds *o, char *reply);
char *ldbRedisProtocolToHuman_Map(sds *o, char *reply);
char *ldbRedisProtocolToHuman_Null(sds *o, char *reply);
char *ldbRedisProtocolToHuman_Bool(sds *o, char *reply);
char *ldbRedisProtocolToHuman_Double(sds *o, char *reply);

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
    case '~': p = ldbRedisProtocolToHuman_Set(o,reply); break;
    case '%': p = ldbRedisProtocolToHuman_Map(o,reply); break;
    case '_': p = ldbRedisProtocolToHuman_Null(o,reply); break;
    case '#': p = ldbRedisProtocolToHuman_Bool(o,reply); break;
    case ',': p = ldbRedisProtocolToHuman_Double(o,reply); break;
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

char *ldbRedisProtocolToHuman_Set(sds *o, char *reply) {
    char *p = strchr(reply+1,'\r');
    long long mbulklen;
    int j = 0;

    string2ll(reply+1,p-reply-1,&mbulklen);
    p += 2;
    *o = sdscatlen(*o,"~(",2);
    for (j = 0; j < mbulklen; j++) {
        p = ldbRedisProtocolToHuman(o,p);
        if (j != mbulklen-1) *o = sdscatlen(*o,",",1);
    }
    *o = sdscatlen(*o,")",1);
    return p;
}

char *ldbRedisProtocolToHuman_Map(sds *o, char *reply) {
    char *p = strchr(reply+1,'\r');
    long long mbulklen;
    int j = 0;

    string2ll(reply+1,p-reply-1,&mbulklen);
    p += 2;
    *o = sdscatlen(*o,"{",1);
    for (j = 0; j < mbulklen; j++) {
        p = ldbRedisProtocolToHuman(o,p);
        *o = sdscatlen(*o," => ",4);
        p = ldbRedisProtocolToHuman(o,p);
        if (j != mbulklen-1) *o = sdscatlen(*o,",",1);
    }
    *o = sdscatlen(*o,"}",1);
    return p;
}

char *ldbRedisProtocolToHuman_Null(sds *o, char *reply) {
    char *p = strchr(reply+1,'\r');
    *o = sdscatlen(*o,"(null)",6);
    return p+2;
}

char *ldbRedisProtocolToHuman_Bool(sds *o, char *reply) {
    char *p = strchr(reply+1,'\r');
    if (reply[1] == 't')
        *o = sdscatlen(*o,"#true",5);
    else
        *o = sdscatlen(*o,"#false",6);
    return p+2;
}

char *ldbRedisProtocolToHuman_Double(sds *o, char *reply) {
    char *p = strchr(reply+1,'\r');
    *o = sdscatlen(*o,"(double) ",9);
    *o = sdscatlen(*o,reply+1,p-reply-1);
    return p+2;
}

/* Log a Redis reply as debugger output, in a human readable format.
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
            sdsfree(expr);
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
    int j;

    if (!lua_checkstack(lua, argc + 1)) {
        /* Increase the Lua stack if needed to make sure there is enough room
         * to push 'argc + 1' elements to the stack. On failure, return error.
         * Notice that we need, in worst case, 'argc + 1' elements because we push all the arguments
         * given by the user (without the first argument) and we also push the 'redis' global table and
         * 'redis.call' function so:
         * (1 (redis table)) + (1 (redis.call function)) + (argc - 1 (all arguments without the first)) = argc + 1*/
        ldbLogRedisReply("max lua stack reached");
        return;
    }

    lua_getglobal(lua,"redis");
    lua_pushstring(lua,"call");
    lua_gettable(lua,-2);       /* Stack: redis, redis.call */
    for (j = 1; j < argc; j++)
        lua_pushlstring(lua,argv[j],sdslen(argv[j]));
    ldb.step = 1;               /* Force redis.call() to log. */
    lua_pcall(lua,argc-1,1,0);  /* Stack: redis, result */
    ldb.step = 0;               /* Disable logging. */
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

/* Implements the debugger "maxlen" command. It just queries or sets the
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
    char* err = NULL;

    /* We continue processing commands until a command that should return
     * to the Lua interpreter is found. */
    while(1) {
        while((argv = ldbReplParseCommand(&argc, &err)) == NULL) {
            char buf[1024];
            if (err) {
                luaPushError(lua, err);
                luaError(lua);
            }
            int nread = connRead(ldb.conn,buf,sizeof(buf));
            if (nread <= 0) {
                /* Make sure the script runs without user input since the
                 * client is no longer connected. */
                ldb.step = 0;
                ldb.bpcount = 0;
                return C_ERR;
            }
            ldb.cbuf = sdscatlen(ldb.cbuf,buf,nread);
            /* after 1M we will exit with an error
             * so that the client will not blow the memory
             */
            if (sdslen(ldb.cbuf) > 1<<20) {
                sdsfree(ldb.cbuf);
                ldb.cbuf = sdsempty();
                luaPushError(lua, "max client buffer reached");
                luaError(lua);
            }
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
ldbLog(sdsnew("[c]ontinue           Run till next breakpoint."));
ldbLog(sdsnew("[l]ist               List source code around current line."));
ldbLog(sdsnew("[l]ist [line]        List source code around [line]."));
ldbLog(sdsnew("                     line = 0 means: current position."));
ldbLog(sdsnew("[l]ist [line] [ctx]  In this form [ctx] specifies how many lines"));
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
ldbLog(sdsnew("[e]val <code>        Execute some Lua code (in a different callframe)."));
ldbLog(sdsnew("[r]edis <cmd>        Execute a Redis command."));
ldbLog(sdsnew("[m]axlen [len]       Trim logged Redis replies and Lua var dumps to len."));
ldbLog(sdsnew("                     Specifying zero as <len> means unlimited."));
ldbLog(sdsnew("[a]bort              Stop the execution of the script. In sync"));
ldbLog(sdsnew("                     mode dataset changes will be retained."));
ldbLog(sdsnew(""));
ldbLog(sdsnew("Debugger functions you can call from Lua scripts:"));
ldbLog(sdsnew("redis.debug()        Produce logs in the debugger console."));
ldbLog(sdsnew("redis.breakpoint()   Stop execution like if there was a breakpoint in the"));
ldbLog(sdsnew("                     next line of code."));
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
            luaPushError(lua, "script aborted for user request");
            luaError(lua);
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
    scriptRunCtx* rctx = luaGetFromRegistry(lua, REGISTRY_RUN_CTX_NAME);
    lua_getstack(lua,0,ar);
    lua_getinfo(lua,"Sl",ar);
    ldb.currentline = ar->currentline;

    int bp = ldbIsBreakpoint(ldb.currentline) || ldb.luabp;
    int timeout = 0;

    /* Events outside our script are not interesting. */
    if(strstr(ar->short_src,"user_script") == NULL) return;

    /* Check if a timeout occurred. */
    if (ar->event == LUA_HOOKCOUNT && ldb.step == 0 && bp == 0) {
        mstime_t elapsed = elapsedMs(rctx->start_time);
        mstime_t timelimit = server.busy_reply_threshold ?
                             server.busy_reply_threshold : 5000;
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
            luaPushError(lua, "timeout during Lua debugging with client closing connection");
            luaError(lua);
        }
        rctx->start_time = getMonotonicUs();
        rctx->snapshot_time = mstime();
    }
}
