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

#include "functions.h"
#include "sds.h"
#include "dict.h"
#include "adlist.h"
#include "atomicvar.h"

static size_t engine_cache_memory = 0;

/* Forward declaration */
static void engineFunctionDispose(dict *d, void *obj);

struct functionsCtx {
    dict *functions;    /* Function name -> Function object that can be used to run the function */
    size_t cache_memory /* Overhead memory (structs, dictionaries, ..) used by all the functions */;
};

dictType engineDictType = {
        dictSdsCaseHash,       /* hash function */
        dictSdsDup,            /* key dup */
        NULL,                  /* val dup */
        dictSdsKeyCaseCompare, /* key compare */
        dictSdsDestructor,     /* key destructor */
        NULL,                  /* val destructor */
        NULL                   /* allow to expand */
};

dictType functionDictType = {
        dictSdsHash,          /* hash function */
        dictSdsDup,           /* key dup */
        NULL,                 /* val dup */
        dictSdsKeyCompare,    /* key compare */
        dictSdsDestructor,    /* key destructor */
        engineFunctionDispose,/* val destructor */
        NULL                  /* allow to expand */
};

/* Dictionary of engines */
static dict *engines = NULL;

/* Functions Ctx.
 * Contains the dictionary that map a function name to
 * function object and the cache memory used by all the functions */
static functionsCtx *functions_ctx = NULL;

static size_t functionMallocSize(functionInfo *fi) {
    return zmalloc_size(fi) + sdsZmallocSize(fi->name)
            + (fi->desc ? sdsZmallocSize(fi->desc) : 0)
            + sdsZmallocSize(fi->code)
            + fi->ei->engine->get_function_memory_overhead(fi->function);
}

/* Dispose function memory */
static void engineFunctionDispose(dict *d, void *obj) {
    UNUSED(d);
    functionInfo *fi = obj;
    sdsfree(fi->code);
    sdsfree(fi->name);
    if (fi->desc) {
        sdsfree(fi->desc);
    }
    engine *engine = fi->ei->engine;
    engine->free_function(engine->engine_ctx, fi->function);
    zfree(fi);
}

/* Free function memory and detele it from the functions dictionary */
static void engineFunctionFree(functionInfo *fi, functionsCtx *functions) {
    functions->cache_memory -= functionMallocSize(fi);

    dictDelete(functions->functions, fi->name);
}

/* Clear all the functions from the given functions ctx */
void functionsCtxClear(functionsCtx *functions_ctx) {
    dictEmpty(functions_ctx->functions, NULL);
    functions_ctx->cache_memory = 0;
}

/* Free the given functions ctx */
void functionsCtxFree(functionsCtx *functions_ctx) {
    functionsCtxClear(functions_ctx);
    dictRelease(functions_ctx->functions);
    zfree(functions_ctx);
}

/* Swap the current functions ctx with the given one.
 * Free the old functions ctx. */
void functionsCtxSwapWithCurrent(functionsCtx *new_functions_ctx) {
    functionsCtxFree(functions_ctx);
    functions_ctx = new_functions_ctx;
}

/* return the current functions ctx */
functionsCtx* functionsCtxGetCurrent() {
    return functions_ctx;
}

/* Create a new functions ctx */
functionsCtx* functionsCtxCreate() {
    functionsCtx *ret = zmalloc(sizeof(functionsCtx));
    ret->functions = dictCreate(&functionDictType);
    ret->cache_memory = 0;
    return ret;
}

/*
 * Register a function info to functions dictionary
 * 1. Set the function client
 * 2. Add function to functions dictionary
 * 3. update cache memory
 */
static void engineFunctionRegister(functionInfo *fi, functionsCtx *functions) {
    int res = dictAdd(functions->functions, fi->name, fi);
    serverAssert(res == DICT_OK);

    functions->cache_memory += functionMallocSize(fi);
}

/*
 * Creating a function info object and register it.
 * Return the created object
 */
static functionInfo* engineFunctionCreate(sds name, void *function, engineInfo *ei,
        sds desc, sds code, functionsCtx *functions)
{

    functionInfo *fi = zmalloc(sizeof(*fi));
    *fi = (functionInfo ) {
        .name = sdsdup(name),
        .function = function,
        .ei = ei,
        .code = sdsdup(code),
        .desc = desc ? sdsdup(desc) : NULL,
    };

    engineFunctionRegister(fi, functions);

    return fi;
}

/* Register an engine, should be called once by the engine on startup and give the following:
 *
 * - engine_name - name of the engine to register
 * - engine_ctx - the engine ctx that should be used by Redis to interact with the engine */
int functionsRegisterEngine(const char *engine_name, engine *engine) {
    sds engine_name_sds = sdsnew(engine_name);
    if (dictFetchValue(engines, engine_name_sds)) {
        serverLog(LL_WARNING, "Same engine was registered twice");
        sdsfree(engine_name_sds);
        return C_ERR;
    }

    client *c = createClient(NULL);
    c->flags |= (CLIENT_DENY_BLOCKING | CLIENT_SCRIPT);
    engineInfo *ei = zmalloc(sizeof(*ei));
    *ei = (engineInfo ) { .name = engine_name_sds, .engine = engine, .c = c,};

    dictAdd(engines, engine_name_sds, ei);

    engine_cache_memory += zmalloc_size(ei) + sdsZmallocSize(ei->name) +
            zmalloc_size(engine) +
            engine->get_engine_memory_overhead(engine->engine_ctx);

    return C_OK;
}

/*
 * FUNCTION STATS
 */
void functionStatsCommand(client *c) {
    if (scriptIsRunning() && scriptIsEval()) {
        addReplyErrorObject(c, shared.slowevalerr);
        return;
    }

    addReplyMapLen(c, 2);

    addReplyBulkCString(c, "running_script");
    if (!scriptIsRunning()) {
        addReplyNull(c);
    } else {
        addReplyMapLen(c, 3);
        addReplyBulkCString(c, "name");
        addReplyBulkCString(c, scriptCurrFunction());
        addReplyBulkCString(c, "command");
        client *script_client = scriptGetCaller();
        addReplyArrayLen(c, script_client->argc);
        for (int i = 0 ; i < script_client->argc ; ++i) {
            addReplyBulkCBuffer(c, script_client->argv[i]->ptr, sdslen(script_client->argv[i]->ptr));
        }
        addReplyBulkCString(c, "duration_ms");
        addReplyLongLong(c, scriptRunDuration());
    }

    addReplyBulkCString(c, "engines");
    addReplyArrayLen(c, dictSize(engines));
    dictIterator *iter = dictGetIterator(engines);
    dictEntry *entry = NULL;
    while ((entry = dictNext(iter))) {
        engineInfo *ei = dictGetVal(entry);
        addReplyBulkCString(c, ei->name);
    }
    dictReleaseIterator(iter);
}

/*
 * FUNCTION LIST
 */
void functionListCommand(client *c) {
    /* general information on all the functions */
    addReplyArrayLen(c, dictSize(functions_ctx->functions));
    dictIterator *iter = dictGetIterator(functions_ctx->functions);
    dictEntry *entry = NULL;
    while ((entry = dictNext(iter))) {
        functionInfo *fi = dictGetVal(entry);
        addReplyMapLen(c, 3);
        addReplyBulkCString(c, "name");
        addReplyBulkCBuffer(c, fi->name, sdslen(fi->name));
        addReplyBulkCString(c, "engine");
        addReplyBulkCBuffer(c, fi->ei->name, sdslen(fi->ei->name));
        addReplyBulkCString(c, "description");
        if (fi->desc) {
            addReplyBulkCBuffer(c, fi->desc, sdslen(fi->desc));
        } else {
            addReplyNull(c);
        }
    }
    dictReleaseIterator(iter);
}

/*
 * FUNCTION INFO <FUNCTION NAME> [WITHCODE]
 */
void functionInfoCommand(client *c) {
    if (c->argc > 4) {
        addReplyErrorFormat(c,"wrong number of arguments for '%s' command or subcommand", c->cmd->name);
        return;
    }
    /* dedicated information on specific function */
    robj *function_name = c->argv[2];
    int with_code = 0;
    if (c->argc == 4) {
        robj *with_code_arg = c->argv[3];
        if (!strcasecmp(with_code_arg->ptr, "withcode")) {
            with_code = 1;
        }
    }

    functionInfo *fi = dictFetchValue(functions_ctx->functions, function_name->ptr);
    if (!fi) {
        addReplyError(c, "Function does not exists");
        return;
    }
    addReplyMapLen(c, with_code? 4 : 3);
    addReplyBulkCString(c, "name");
    addReplyBulkCBuffer(c, fi->name, sdslen(fi->name));
    addReplyBulkCString(c, "engine");
    addReplyBulkCBuffer(c, fi->ei->name, sdslen(fi->ei->name));
    addReplyBulkCString(c, "description");
    if (fi->desc) {
        addReplyBulkCBuffer(c, fi->desc, sdslen(fi->desc));
    } else {
        addReplyNull(c);
    }
    if (with_code) {
        addReplyBulkCString(c, "code");
        addReplyBulkCBuffer(c, fi->code, sdslen(fi->code));
    }
}

/*
 * FUNCTION DELETE <FUNCTION NAME>
 */
void functionDeleteCommand(client *c) {
    if (server.masterhost && server.repl_slave_ro && !(c->flags & CLIENT_MASTER)) {
        addReplyError(c, "Can not delete a function on a read only replica");
        return;
    }

    robj *function_name = c->argv[2];
    functionInfo *fi = dictFetchValue(functions_ctx->functions, function_name->ptr);
    if (!fi) {
        addReplyError(c, "Function not found");
        return;
    }

    engineFunctionFree(fi, functions_ctx);
    /* Indicate that the command changed the data so it will be replicated and
     * counted as a data change (for persistence configuration) */
    server.dirty++;
    addReply(c, shared.ok);
}

void functionKillCommand(client *c) {
    scriptKill(c, 0);
}

static void fcallCommandGeneric(client *c, int ro) {
    robj *function_name = c->argv[1];
    functionInfo *fi = dictFetchValue(functions_ctx->functions, function_name->ptr);
    if (!fi) {
        addReplyError(c, "Function not found");
        return;
    }
    engine *engine = fi->ei->engine;

    long long numkeys;
    /* Get the number of arguments that are keys */
    if (getLongLongFromObject(c->argv[2], &numkeys) != C_OK) {
        addReplyError(c, "Bad number of keys provided");
        return;
    }
    if (numkeys > (c->argc - 3)) {
        addReplyError(c, "Number of keys can't be greater than number of args");
        return;
    } else if (numkeys < 0) {
        addReplyError(c, "Number of keys can't be negative");
        return;
    }

    scriptRunCtx run_ctx;

    scriptPrepareForRun(&run_ctx, fi->ei->c, c, fi->name);
    if (ro) {
        run_ctx.flags |= SCRIPT_READ_ONLY;
    }
    engine->call(&run_ctx, engine->engine_ctx, fi->function, c->argv + 3, numkeys,
                 c->argv + 3 + numkeys, c->argc - 3 - numkeys);
    scriptResetRun(&run_ctx);
}

/*
 * FCALL <FUNCTION NAME> nkeys <key1 .. keyn> <arg1 .. argn>
 */
void fcallCommand(client *c) {
    fcallCommandGeneric(c, 0);
}

/*
 * FCALL_RO <FUNCTION NAME> nkeys <key1 .. keyn> <arg1 .. argn>
 */
void fcallroCommand(client *c) {
    fcallCommandGeneric(c, 1);
}

void functionFlushCommand(client *c) {
    if (c->argc > 3) {
        addReplySubcommandSyntaxError(c);
        return;
    }
    int async = 0;
    if (c->argc == 3 && !strcasecmp(c->argv[2]->ptr,"sync")) {
        async = 0;
    } else if (c->argc == 3 && !strcasecmp(c->argv[2]->ptr,"async")) {
        async = 1;
    } else if (c->argc == 2) {
        async = server.lazyfree_lazy_user_flush ? 1 : 0;
    } else {
        addReplyError(c,"FUNCTION FLUSH only supports SYNC|ASYNC option");
        return;
    }

    if (async) {
        functionsCtx *old_f_ctx = functions_ctx;
        functions_ctx = functionsCtxCreate();
        freeFunctionsAsync(old_f_ctx);
    } else {
        functionsCtxClear(functions_ctx);
    }
    /* Indicate that the command changed the data so it will be replicated and
     * counted as a data change (for persistence configuration) */
    server.dirty++;
    addReply(c,shared.ok);
}

void functionHelpCommand(client *c) {
    const char *help[] = {
"CREATE <ENGINE NAME> <FUNCTION NAME> [REPLACE] [DESC <FUNCTION DESCRIPTION>] <FUNCTION CODE>",
"    Create a new function with the given function name and code.",
"DELETE <FUNCTION NAME>",
"    Delete the given function.",
"INFO <FUNCTION NAME> [WITHCODE]",
"    For each function, print the following information about the function:",
"    * Function name",
"    * The engine used to run the function",
"    * Function description",
"    * Function code (only if WITHCODE is given)",
"LIST",
"    Return general information on all the functions:",
"    * Function name",
"    * The engine used to run the function",
"    * Function description",
"STATS",
"    Return information about the current function running:",
"    * Function name",
"    * Command used to run the function",
"    * Duration in MS that the function is running",
"    If not function is running, return nil",
"    In addition, returns a list of available engines.",
"KILL",
"    Kill the current running function.",
"FLUSH [ASYNC|SYNC]",
"    Delete all the functions.",
"    When called without the optional mode argument, the behavior is determined by the",
"    lazyfree-lazy-user-flush configuration directive. Valid modes are:",
"    * ASYNC: Asynchronously flush the functions.",
"    * SYNC: Synchronously flush the functions.",
NULL };
    addReplyHelp(c, help);
}

/* Compile and save the given function, return C_OK on success and C_ERR on failure.
 * In case on failure the err out param is set with relevant error message */
int functionsCreateWithFunctionCtx(sds function_name,sds engine_name, sds desc, sds code,
                                   int replace, sds* err, functionsCtx *functions) {
    engineInfo *ei = dictFetchValue(engines, engine_name);
    if (!ei) {
        *err = sdsnew("Engine not found");
        return C_ERR;
    }
    engine *engine = ei->engine;

    functionInfo *fi = dictFetchValue(functions->functions, function_name);
    if (fi && !replace) {
        *err = sdsnew("Function already exists");
        return C_ERR;
    }

    void *function = engine->create(engine->engine_ctx, code, err);
    if (*err) {
        return C_ERR;
    }

    if (fi) {
        /* free the already existing function as we are going to replace it */
        engineFunctionFree(fi, functions);
    }

    engineFunctionCreate(function_name, function, ei, desc, code, functions);

    return C_OK;
}

/*
 * FUNCTION CREATE <ENGINE NAME> <FUNCTION NAME>
 *             [REPLACE] [DESC <FUNCTION DESCRIPTION>] <FUNCTION CODE>
 *
 * ENGINE NAME     - name of the engine to use the run the function
 * FUNCTION NAME   - name to use to invoke the function
 * REPLACE         - optional, replace existing function
 * DESCRIPTION     - optional, function description
 * FUNCTION CODE   - function code to pass to the engine
 */
void functionCreateCommand(client *c) {

    if (server.masterhost && server.repl_slave_ro && !(c->flags & CLIENT_MASTER)) {
        addReplyError(c, "Can not create a function on a read only replica");
        return;
    }

    robj *engine_name = c->argv[2];
    robj *function_name = c->argv[3];

    int replace = 0;
    int argc_pos = 4;
    sds desc = NULL;
    while (argc_pos < c->argc - 1) {
        robj *next_arg = c->argv[argc_pos++];
        if (!strcasecmp(next_arg->ptr, "replace")) {
            replace = 1;
            continue;
        }
        if (!strcasecmp(next_arg->ptr, "description")) {
            if (argc_pos >= c->argc) {
                addReplyError(c, "Bad function description");
                return;
            }
            desc = c->argv[argc_pos++]->ptr;
            continue;
        }
    }

    if (argc_pos >= c->argc) {
        addReplyError(c, "Function code is missing");
        return;
    }

    robj *code = c->argv[argc_pos];
    sds err = NULL;
    if (functionsCreateWithFunctionCtx(function_name->ptr, engine_name->ptr,
                                       desc, code->ptr, replace, &err, functions_ctx) != C_OK)
    {
        addReplyErrorSds(c, err);
        return;
    }
    /* Indicate that the command changed the data so it will be replicated and
     * counted as a data change (for persistence configuration) */
    server.dirty++;
    addReply(c, shared.ok);
}

/* Return memory usage of all the engines combine */
unsigned long functionsMemory() {
    dictIterator *iter = dictGetIterator(engines);
    dictEntry *entry = NULL;
    size_t engines_nemory = 0;
    while ((entry = dictNext(iter))) {
        engineInfo *ei = dictGetVal(entry);
        engine *engine = ei->engine;
        engines_nemory += engine->get_used_memory(engine->engine_ctx);
    }
    dictReleaseIterator(iter);

    return engines_nemory;
}

/* Return memory overhead of all the engines combine */
unsigned long functionsMemoryOverhead() {
    size_t memory_overhead = dictSize(engines) * sizeof(dictEntry) +
            dictSlots(engines) * sizeof(dictEntry*);
    memory_overhead += dictSize(functions_ctx->functions) * sizeof(dictEntry) +
            dictSlots(functions_ctx->functions) * sizeof(dictEntry*) + sizeof(functionsCtx);
    memory_overhead += functions_ctx->cache_memory;
    memory_overhead += engine_cache_memory;

    return memory_overhead;
}

/* Returns the number of functions */
unsigned long functionsNum() {
    return dictSize(functions_ctx->functions);
}

dict* functionsGet() {
    return functions_ctx->functions;
}

size_t functionsLen(functionsCtx *functions_ctx) {
    return dictSize(functions_ctx->functions);
}

/* Initialize engine data structures.
 * Should be called once on server initialization */
int functionsInit() {
    engines = dictCreate(&engineDictType);
    functions_ctx = functionsCtxCreate();

    if (luaEngineInitEngine() != C_OK) {
        return C_ERR;
    }

    return C_OK;
}
