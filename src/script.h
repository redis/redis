/*
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of the Redis Source Available License 2.0
 * (RSALv2) or the Server Side Public License v1 (SSPLv1).
 */

#ifndef __SCRIPT_H_
#define __SCRIPT_H_

/*
 * Script.c unit provides an API for functions and eval 
 * to interact with Redis. Interaction includes mostly
 * executing commands, but also functionalities like calling
 * Redis back on long scripts or check if the script was killed.
 *
 * The interaction is done using a scriptRunCtx object that
 * need to be created by the user and initialized using scriptPrepareForRun.
 *
 * Detailed list of functionalities expose by the unit:
 * 1. Calling commands (including all the validation checks such as
 *    acl, cluster, read only run, ...)
 * 2. Set Resp
 * 3. Set Replication method (AOF/REPLICATION/NONE)
 * 4. Call Redis back to on long running scripts to allow Redis reply
 *    to clients and perform script kill
 */

/*
 * scriptInterrupt function will return one of those value,
 *
 * - SCRIPT_KILL - kill the current running script.
 * - SCRIPT_CONTINUE - keep running the current script.
 */
#define SCRIPT_KILL 1
#define SCRIPT_CONTINUE 2

/* runCtx flags */
#define SCRIPT_WRITE_DIRTY            (1ULL<<0) /* indicate that the current script already performed a write command */
#define SCRIPT_TIMEDOUT               (1ULL<<3) /* indicate that the current script timedout */
#define SCRIPT_KILLED                 (1ULL<<4) /* indicate that the current script was marked to be killed */
#define SCRIPT_READ_ONLY              (1ULL<<5) /* indicate that the current script should only perform read commands */
#define SCRIPT_ALLOW_OOM              (1ULL<<6) /* indicate to allow any command even if OOM reached */
#define SCRIPT_EVAL_MODE              (1ULL<<7) /* Indicate that the current script called from legacy Lua */
#define SCRIPT_ALLOW_CROSS_SLOT       (1ULL<<8) /* Indicate that the current script may access keys from multiple slots */
typedef struct scriptRunCtx scriptRunCtx;

struct scriptRunCtx {
    const char *funcname;
    client *c;
    client *original_client;
    int flags;
    int repl_flags;
    monotime start_time;
    int slot;
};

/* Scripts flags */
#define SCRIPT_FLAG_NO_WRITES        (1ULL<<0)
#define SCRIPT_FLAG_ALLOW_OOM        (1ULL<<1)
#define SCRIPT_FLAG_ALLOW_STALE      (1ULL<<2)
#define SCRIPT_FLAG_NO_CLUSTER       (1ULL<<3)
#define SCRIPT_FLAG_EVAL_COMPAT_MODE (1ULL<<4) /* EVAL Script backwards compatible behavior, no shebang provided */
#define SCRIPT_FLAG_ALLOW_CROSS_SLOT (1ULL<<5)

/* Defines a script flags */
typedef struct scriptFlag {
    uint64_t flag;
    const char *str;
} scriptFlag;

extern scriptFlag scripts_flags_def[];

void luaEnvInit(void);
lua_State *createLuaState(void);
uint64_t scriptFlagsToCmdFlags(uint64_t cmd_flags, uint64_t script_flags);
int scriptPrepareForRun(scriptRunCtx *r_ctx, client *engine_client, client *caller, const char *funcname, uint64_t script_flags, int ro);
void scriptResetRun(scriptRunCtx *r_ctx);
int scriptSetResp(scriptRunCtx *r_ctx, int resp);
int scriptSetRepl(scriptRunCtx *r_ctx, int repl);
void scriptCall(scriptRunCtx *r_ctx, sds *err);
int scriptInterrupt(scriptRunCtx *r_ctx);
void scriptKill(client *c, int is_eval);
int scriptIsRunning(void);
const char* scriptCurrFunction(void);
int scriptIsEval(void);
int scriptIsTimedout(void);
client* scriptGetClient(void);
client* scriptGetCaller(void);
long long scriptRunDuration(void);

#endif /* __SCRIPT_H_ */
