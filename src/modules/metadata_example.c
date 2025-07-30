/* Hellodict -- An example of modules dictionary API
 *
 * This module implements a volatile key-value store on top of the
 * dictionary exported by the Redis modules API.
 *
 * -----------------------------------------------------------------------------
 *
 * Copyright (c) 2018-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include "../redismodule.h"
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#ifndef MODULE_ID
#define MODULE_ID 0
#endif
static size_t module_id=MODULE_ID;



/* m-id.GET <key>
 *
 * Return the value of the specified key, or a null reply if the key
 * is not defined. */

static int CMD_get (RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) return RedisModule_WrongArity(ctx);
	size_t obj_id;
    int status = (size_t) RedisModule_GetModuleMetadata(ctx,argv[1], (void **)&obj_id);
	char rep[64];
	sprintf(rep, "%s 0x%lx %d",
			status == REDISMODULE_OK && (obj_id & 0x7) == module_id ? "OK" : "FAIL",
			obj_id, status);
	return RedisModule_ReplyWithCString(ctx, rep);
										
}

static int setVal(RedisModuleCtx *ctx, int type, const char *event, RedisModuleString *key) {
	(void) type;
	(void) event;
    size_t obj_id = (rand() * 16) | module_id;
	RedisModule_SetModuleMetadata(ctx, key, (void *)obj_id);
    return REDISMODULE_OK;
}
	


/* This function must be present on each Redis module. It is used in order to
 * register the commands into the Redis server. */
int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
	(void)argv;
	static char module_name[32];
	
	if (argc != 0) 
		return REDISMODULE_ERR;
	
	if (module_id > 7)
		return REDISMODULE_ERR;
	sprintf(module_name, "m-%1.1lu", module_id);
    if (RedisModule_Init(ctx,module_name,1,REDISMODULE_APIVER_1)
        == REDISMODULE_ERR) return REDISMODULE_ERR;	
	RedisMetadataMethods metadata_methods = {
		.
		.default_value = -1,
        .serializeMetadata = NULL, 
        .deserializeMetadata = NULL,
		.freeMetadata = NULL,
		.defragMetadata = NULL};
	if (RedisModule_RegisterMetadataMethods(ctx, &metadata_methods)
		!= METADATA_MODULE_OK) return REDISMODULE_ERR;
	static char cmd[64];
	
	sprintf(cmd, "%s.get", module_name);
    if (RedisModule_CreateCommand(ctx,cmd, CMD_get,"readonly",1,1,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;	

    if (RedisModule_SubscribeToKeyspaceEvents(ctx,REDISMODULE_NOTIFY_ALL, setVal) == REDISMODULE_ERR)
        return REDISMODULE_ERR;	
		


    return REDISMODULE_OK;
}
