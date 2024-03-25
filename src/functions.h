/*
 * Copyright (c) 2021-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of the Redis Source Available License 2.0
 * (RSALv2) or the Server Side Public License v1 (SSPLv1).
 */

#ifndef __FUNCTIONS_H_
#define __FUNCTIONS_H_

/*
 * functions.c unit provides the Redis Functions API:
 * * FUNCTION LOAD
 * * FUNCTION LIST
 * * FUNCTION CALL (FCALL and FCALL_RO)
 * * FUNCTION DELETE
 * * FUNCTION STATS
 * * FUNCTION KILL
 * * FUNCTION FLUSH
 * * FUNCTION DUMP
 * * FUNCTION RESTORE
 * * FUNCTION HELP
 *
 * Also contains implementation for:
 * * Save/Load function from rdb
 * * Register engines
 */

#include "server.h"
#include "script.h"
#include "redismodule.h"

typedef struct functionLibInfo functionLibInfo;

typedef struct engine {
    /* engine specific context */
    void *engine_ctx;

    /* Create function callback, get the engine_ctx, and function code
     * engine_ctx - opaque struct that was created on engine initialization
     * li - library information that need to be provided and when add functions
     * code - the library code
     * timeout - timeout for the library creation (0 for no timeout)
     * err - description of error (if occurred)
     * returns C_ERR on error and set err to be the error message */
    int (*create)(void *engine_ctx, functionLibInfo *li, sds code, size_t timeout, sds *err);

    /* Invoking a function, r_ctx is an opaque object (from engine POV).
     * The r_ctx should be used by the engine to interaction with Redis,
     * such interaction could be running commands, set resp, or set
     * replication mode
     */
    void (*call)(scriptRunCtx *r_ctx, void *engine_ctx, void *compiled_function,
            robj **keys, size_t nkeys, robj **args, size_t nargs);

    /* get current used memory by the engine */
    size_t (*get_used_memory)(void *engine_ctx);

    /* Return memory overhead for a given function,
     * such memory is not counted as engine memory but as general
     * structs memory that hold different information */
    size_t (*get_function_memory_overhead)(void *compiled_function);

    /* Return memory overhead for engine (struct size holding the engine)*/
    size_t (*get_engine_memory_overhead)(void *engine_ctx);

    /* free the given function */
    void (*free_function)(void *engine_ctx, void *compiled_function);
} engine;

/* Hold information about an engine.
 * Used on rdb.c so it must be declared here. */
typedef struct engineInfo {
    sds name;       /* Name of the engine */
    engine *engine; /* engine callbacks that allows to interact with the engine */
    client *c;      /* Client that is used to run commands */
} engineInfo;

/* Hold information about the specific function.
 * Used on rdb.c so it must be declared here. */
typedef struct functionInfo {
    sds name;            /* Function name */
    void *function;      /* Opaque object that set by the function's engine and allow it
                            to run the function, usually it's the function compiled code. */
    functionLibInfo* li; /* Pointer to the library created the function */
    sds desc;            /* Function description */
    uint64_t f_flags;    /* Function flags */
} functionInfo;

/* Hold information about the specific library.
 * Used on rdb.c so it must be declared here. */
struct functionLibInfo {
    sds name;        /* Library name */
    dict *functions; /* Functions dictionary */
    engineInfo *ei;  /* Pointer to the function engine */
    sds code;        /* Library code */
};

int functionsRegisterEngine(const char *engine_name, engine *engine_ctx);
sds functionsCreateWithLibraryCtx(sds code, int replace, sds* err, functionsLibCtx *lib_ctx, size_t timeout);
unsigned long functionsMemory(void);
unsigned long functionsMemoryOverhead(void);
unsigned long functionsNum(void);
unsigned long functionsLibNum(void);
dict* functionsLibGet(void);
size_t functionsLibCtxFunctionsLen(functionsLibCtx *functions_ctx);
functionsLibCtx* functionsLibCtxGetCurrent(void);
functionsLibCtx* functionsLibCtxCreate(void);
void functionsLibCtxClearCurrent(int async);
void functionsLibCtxFree(functionsLibCtx *lib_ctx);
void functionsLibCtxClear(functionsLibCtx *lib_ctx);
void functionsLibCtxSwapWithCurrent(functionsLibCtx *lib_ctx);

int functionLibCreateFunction(sds name, void *function, functionLibInfo *li, sds desc, uint64_t f_flags, sds *err);

int luaEngineInitEngine(void);
int functionsInit(void);

#endif /* __FUNCTIONS_H_ */
