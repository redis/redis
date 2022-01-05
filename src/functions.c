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

typedef enum {
    restorePolicy_Flush, restorePolicy_Append, restorePolicy_Replace
} restorePolicy;

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
        dictSdsCaseHash,      /* hash function */
        dictSdsDup,           /* key dup */
        NULL,                 /* val dup */
        dictSdsKeyCaseCompare,/* key compare */
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
    if (!obj) {
        return;
    }
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

void functionsCtxClearCurrent(int async) {
    if (async) {
        functionsCtx *old_f_ctx = functions_ctx;
        functions_ctx = functionsCtxCreate();
        freeFunctionsAsync(old_f_ctx);
    } else {
        functionsCtxClear(functions_ctx);
    }
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

/*
 * FUNCTION DUMP
 *
 * Returns a binary payload representing all the functions.
 * Can be loaded using FUNCTION RESTORE
 *
 * The payload structure is the same as on RDB. Each function
 * is saved separately with the following information:
 * * Function name
 * * Engine name
 * * Function description
 * * Function code
 * RDB_OPCODE_FUNCTION is saved before each function to present
 * that the payload is a function.
 * RDB version and crc64 is saved at the end of the payload.
 * The RDB version is saved for backward compatibility.
 * crc64 is saved so we can verify the payload content.
 */
void functionDumpCommand(client *c) {
    unsigned char buf[2];
    uint64_t crc;
    rio payload;
    rioInitWithBuffer(&payload, sdsempty());

    rdbSaveFunctions(&payload);

    /* RDB version */
    buf[0] = RDB_VERSION & 0xff;
    buf[1] = (RDB_VERSION >> 8) & 0xff;
    payload.io.buffer.ptr = sdscatlen(payload.io.buffer.ptr, buf, 2);

    /* CRC64 */
    crc = crc64(0, (unsigned char*) payload.io.buffer.ptr,
                sdslen(payload.io.buffer.ptr));
    memrev64ifbe(&crc);
    payload.io.buffer.ptr = sdscatlen(payload.io.buffer.ptr, &crc, 8);

    addReplyBulkSds(c, payload.io.buffer.ptr);
}

/*
 * FUNCTION RESTORE <payload> [FLUSH|APPEND|REPLACE]
 *
 * Restore the functions represented by the give payload.
 * Restore policy to can be given to control how to handle existing functions (default APPEND):
 * * FLUSH: delete all existing functions.
 * * APPEND: appends the restored functions to the existing functions. On collision, abort.
 * * REPLACE: appends the restored functions to the existing functions.
 *   On collision, replace the old function with the new function.
 */
void functionRestoreCommand(client *c) {
    if (c->argc > 4) {
        addReplySubcommandSyntaxError(c);
        return;
    }

    restorePolicy restore_replicy = restorePolicy_Append; /* default policy: APPEND */
    sds data = c->argv[2]->ptr;
    size_t data_len = sdslen(data);
    rio payload;
    dictIterator *iter = NULL;
    sds err = NULL;

    if (c->argc == 4) {
        const char *restore_policy_str = c->argv[3]->ptr;
        if (!strcasecmp(restore_policy_str, "append")) {
            restore_replicy = restorePolicy_Append;
        } else if (!strcasecmp(restore_policy_str, "replace")) {
            restore_replicy = restorePolicy_Replace;
        } else if (!strcasecmp(restore_policy_str, "flush")) {
            restore_replicy = restorePolicy_Flush;
        } else {
            addReplyError(c, "Wrong restore policy given, value should be either FLUSH, APPEND or REPLACE.");
            return;
        }
    }

    uint16_t rdbver;
    if (verifyDumpPayload((unsigned char*)data, data_len, &rdbver) != C_OK) {
        addReplyError(c, "DUMP payload version or checksum are wrong");
        return;
    }

    functionsCtx *f_ctx = functionsCtxCreate();
    rioInitWithBuffer(&payload, data);

    /* Read until reaching last 10 bytes that should contain RDB version and checksum. */
    while (data_len - payload.io.buffer.pos > 10) {
        int type;
        if ((type = rdbLoadType(&payload)) == -1) {
            err = sdsnew("can not read data type");
            goto load_error;
        }
        if (type != RDB_OPCODE_FUNCTION) {
            err = sdsnew("given type is not a function");
            goto load_error;
        }
        if (rdbFunctionLoad(&payload, rdbver, f_ctx, RDBFLAGS_NONE, &err) != C_OK) {
            if (!err) {
                err = sdsnew("failed loading the given functions payload");
            }
            goto load_error;
        }
    }

    if (restore_replicy == restorePolicy_Flush) {
        functionsCtxSwapWithCurrent(f_ctx);
        f_ctx = NULL; /* avoid releasing the f_ctx in the end */
    } else {
        if (restore_replicy == restorePolicy_Append) {
            /* First make sure there is only new functions */
            iter = dictGetIterator(f_ctx->functions);
            dictEntry *entry = NULL;
            while ((entry = dictNext(iter))) {
                functionInfo *fi = dictGetVal(entry);
                if (dictFetchValue(functions_ctx->functions, fi->name)) {
                    /* function already exists, failed the restore. */
                    err = sdscatfmt(sdsempty(), "Function %s already exists", fi->name);
                    goto load_error;
                }
            }
            dictReleaseIterator(iter);
        }
        iter = dictGetIterator(f_ctx->functions);
        dictEntry *entry = NULL;
        while ((entry = dictNext(iter))) {
            functionInfo *fi = dictGetVal(entry);
            dictReplace(functions_ctx->functions, fi->name, fi);
            dictSetVal(f_ctx->functions, entry, NULL); /* make sure value will not be disposed */
        }
    }

    /* Indicate that the command changed the data so it will be replicated and
     * counted as a data change (for persistence configuration) */
    server.dirty++;

load_error:
    if (err) {
        addReplyErrorSds(c, err);
    } else {
        addReply(c, shared.ok);
    }
    if (iter) {
        dictReleaseIterator(iter);
    }
    if (f_ctx) {
        functionsCtxFree(f_ctx);
    }
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

    functionsCtxClearCurrent(async);

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
"DUMP",
"    Returns a serialized payload representing the current functions, can be restored using FUNCTION RESTORE command",
"RESTORE <PAYLOAD> [FLUSH|APPEND|REPLACE]",
"    Restore the functions represented by the given payload, it is possible to give a restore policy to",
"    control how to handle existing functions (default APPEND):",
"    * FLUSH: delete all existing functions.",
"    * APPEND: appends the restored functions to the existing functions. On collision, abort.",
"    * REPLACE: appends the restored functions to the existing functions, On collision, replace the old",
"      function with the new function.",
NULL };
    addReplyHelp(c, help);
}

/* Verify that the function name is of the format: [a-zA-Z0-9_][a-zA-Z0-9_]? */
static int functionsVerifyName(sds name) {
    if (sdslen(name) == 0) {
        return C_ERR;
    }
    for (size_t i = 0 ; i < sdslen(name) ; ++i) {
        char curr_char = name[i];
        if ((curr_char >= 'a' && curr_char <= 'z') ||
            (curr_char >= 'A' && curr_char <= 'Z') ||
            (curr_char >= '0' && curr_char <= '9') ||
            (curr_char == '_'))
        {
            continue;
        }
        return C_ERR;
    }
    return C_OK;
}

/* Compile and save the given function, return C_OK on success and C_ERR on failure.
 * In case on failure the err out param is set with relevant error message */
int functionsCreateWithFunctionCtx(sds function_name,sds engine_name, sds desc, sds code,
                                   int replace, sds* err, functionsCtx *functions) {
    if (functionsVerifyName(function_name)) {
        *err = sdsnew("Function names can only contain letters and numbers and must be at least one character long");
        return C_ERR;
    }

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
