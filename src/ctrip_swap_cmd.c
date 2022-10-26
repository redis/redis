/* Copyright (c) 2021, ctrip.com
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

#include "ctrip_swap.h"
#include <math.h>
/* ----------------------------- swaps result ----------------------------- */
/* Prepare the getKeyRequestsResult struct to hold numswaps, either by using
 * the pre-allocated swaps or by allocating a new array on the heap.
 *
 * This function must be called at least once before starting to populate
 * the result, and can be called repeatedly to enlarge the result array.
 */
zrangespec* zrangespecdup(zrangespec* src) {
    zrangespec* dst = zmalloc(sizeof(zrangespec));
    dst->minex = src->minex;
    dst->min = src->min;
    dst->maxex = src->maxex;
    dst->max = src->max;
    return dst;
}

zlexrangespec* zlexrangespecdup(zlexrangespec* src) {
    zlexrangespec* dst = zmalloc(sizeof(zlexrangespec));
    dst->minex = src->minex;
    if (src->min != shared.minstring &&
        src->min != shared.maxstring) {
        dst->min = sdsdup(src->min);
    } else {
        dst->min = src->min;
    }

    dst->maxex = src->maxex;
    if (src->max != shared.minstring &&
        src->max != shared.maxstring) {
        dst->max = sdsdup(src->max);
    } else {
        dst->max = src->max;
    }
    return dst;
}

void copyKeyRequest(keyRequest *dst, keyRequest *src) {
    if (src->key) incrRefCount(src->key);
    dst->key = src->key;

    dst->level = src->level;
    dst->cmd_intention = src->cmd_intention;
    dst->cmd_intention_flags = src->cmd_intention_flags;
    dst->dbid = src->dbid;
    dst->type = src->type;

    switch (src->type) {
    case KEYREQUEST_TYPE_KEY:
        break;
    case KEYREQUEST_TYPE_SUBKEY:
        if (src->b.num_subkeys > 0)  {
            dst->b.subkeys = zmalloc(sizeof(robj*)*src->b.num_subkeys);
            for (int i = 0; i < src->b.num_subkeys; i++) {
                if (src->b.subkeys[i]) incrRefCount(src->b.subkeys[i]);
                dst->b.subkeys[i] = src->b.subkeys[i];
            }
        }
        dst->b.num_subkeys = src->b.num_subkeys;
        break;
    case KEYREQUEST_TYPE_RANGE:
        dst->l.num_ranges = src->l.num_ranges;
        dst->l.ranges = zmalloc(src->l.num_ranges*sizeof(struct range));
        memcpy(dst->l.ranges,src->l.ranges,src->l.num_ranges*sizeof(struct range));
        break;
    case KEYREQUEST_TYPE_SCORE:
        dst->zs.rangespec = zrangespecdup(src->zs.rangespec);
        dst->zs.reverse = src->zs.reverse;
        dst->zs.limit = src->zs.limit;
        break;
    case KEYREQUEST_TYPE_LEX:
        dst->zl.rangespec = zlexrangespecdup(src->zl.rangespec);
        dst->zl.reverse = src->zl.reverse;
        dst->zl.limit = src->zl.limit;
        break;
    default:
        break;
    }

    dst->list_arg_rewrite[0] = src->list_arg_rewrite[0];
    dst->list_arg_rewrite[1] = src->list_arg_rewrite[1];
}

void moveKeyRequest(keyRequest *dst, keyRequest *src) {
    dst->key = src->key;
    src->key = NULL;
    dst->level = src->level;
    dst->cmd_intention = src->cmd_intention;
    dst->cmd_intention_flags = src->cmd_intention_flags;
    dst->dbid = src->dbid;
    dst->type = src->type;

    switch (src->type) {
    case KEYREQUEST_TYPE_KEY:
        break;
    case KEYREQUEST_TYPE_SUBKEY:
        dst->b.subkeys = src->b.subkeys;
        src->b.subkeys = NULL;
        dst->b.num_subkeys = src->b.num_subkeys;
        src->b.num_subkeys = 0;
        break;
    case KEYREQUEST_TYPE_RANGE:
        dst->l.num_ranges = src->l.num_ranges;
        dst->l.ranges = src->l.ranges;
        src->l.ranges = NULL;
        break;
    case KEYREQUEST_TYPE_SCORE:
        dst->zs.rangespec = src->zs.rangespec;
        src->zs.rangespec = NULL;
        dst->zs.reverse = src->zs.reverse;
        dst->zs.limit = src->zs.limit;
        break;
    case KEYREQUEST_TYPE_LEX:
        dst->zl.rangespec = src->zl.rangespec;
        src->zl.rangespec = NULL;
        dst->zl.reverse = src->zl.reverse;
        dst->zl.limit = src->zl.limit;
        break;
    default:
        break;
    }

    dst->list_arg_rewrite[0] = src->list_arg_rewrite[0];
    dst->list_arg_rewrite[1] = src->list_arg_rewrite[1];
}

void keyRequestDeinit(keyRequest *key_request) {
    if (key_request == NULL) return;
    if (key_request->key) decrRefCount(key_request->key);
    key_request->key = NULL;

    switch (key_request->type) {
    case KEYREQUEST_TYPE_KEY:
        break;
    case KEYREQUEST_TYPE_SUBKEY:
        for (int i = 0; i < key_request->b.num_subkeys; i++) {
            if (key_request->b.subkeys[i])
                decrRefCount(key_request->b.subkeys[i]);
            key_request->b.subkeys[i] = NULL;
        }
        zfree(key_request->b.subkeys);
        key_request->b.subkeys = NULL;
        key_request->b.num_subkeys = 0;
        break;
    case KEYREQUEST_TYPE_RANGE:
        zfree(key_request->l.ranges);
        key_request->l.ranges = NULL;
        key_request->l.num_ranges = 0;
        break;
    case KEYREQUEST_TYPE_SCORE:
        if (key_request->zs.rangespec != NULL) {
            zfree(key_request->zs.rangespec);
            key_request->zs.rangespec = NULL;
        }
        break;
    case KEYREQUEST_TYPE_LEX:
        if (key_request->zl.rangespec != NULL) {
            zslFreeLexRange(key_request->zl.rangespec);
            zfree(key_request->zl.rangespec);
            key_request->zl.rangespec= NULL;
        }
        break;
    default:
        break;
    }
}

void getKeyRequestsPrepareResult(getKeyRequestsResult *result, int num) {
	/* GETKEYS_RESULT_INIT initializes keys to NULL, point it to the
     * pre-allocated stack buffer here. */
	if (!result->key_requests) {
		serverAssert(!result->num);
		result->key_requests = result->buffer;
	}

	/* Resize if necessary */
	if (num > result->size) {
		if (result->key_requests != result->buffer) {
			/* We're not using a static buffer, just (re)alloc */
			result->key_requests = zrealloc(result->key_requests,
                    num*sizeof(keyRequest));
		} else {
			/* We are using a static buffer, copy its contents */
			result->key_requests = zmalloc(num*sizeof(keyRequest));
			if (result->num) {
				memcpy(result->key_requests,result->buffer,
                        result->num*sizeof(keyRequest));
            }
		}
		result->size = num;
	}
}

int expandKeyRequests(getKeyRequestsResult* result) {
    if (result->num == result->size) {
        int newsize = result->size + 
            (result->size > 8192 ? 8192 : result->size);
        getKeyRequestsPrepareResult(result, newsize);
        return 1;
    }
    return 0;
}

keyRequest *getKeyRequestsAppendCommonResult(getKeyRequestsResult *result,
        int level, robj *key, int cmd_intention, int cmd_intention_flags,
        int dbid) {
    if (result->num == result->size) {
        int newsize = result->size + 
            (result->size > 8192 ? 8192 : result->size);
        getKeyRequestsPrepareResult(result, newsize);
    }

    keyRequest *key_request = &result->key_requests[result->num++];
    key_request->level = level;
    key_request->key = key;
    key_request->cmd_intention = cmd_intention;
    key_request->cmd_intention_flags = cmd_intention_flags;
    key_request->dbid = dbid;
    argRewriteRequestInit(key_request->list_arg_rewrite+0);
    argRewriteRequestInit(key_request->list_arg_rewrite+1);
    return key_request;
}

void getKeyRequestsAppendScoreResult(getKeyRequestsResult *result, int level,
        robj *key, int reverse, zrangespec* rangespec, int limit, int cmd_intention,
        int cmd_intention_flags, int dbid) {
    expandKeyRequests(result);    
    keyRequest *key_request = &result->key_requests[result->num++];
    key_request->level = level;
    key_request->key = key;
    key_request->type = KEYREQUEST_TYPE_SCORE;
    key_request->zs.reverse = reverse;
    key_request->zs.rangespec = rangespec;
    key_request->zs.limit = limit;
    key_request->cmd_intention = cmd_intention;
    key_request->cmd_intention_flags = cmd_intention_flags;
    key_request->dbid = dbid;
}

void getKeyRequestsAppendLexeResult(getKeyRequestsResult *result, int level,
        robj *key, int reverse, zlexrangespec* rangespec, int limit, int cmd_intention,
        int cmd_intention_flags, int dbid) {
    expandKeyRequests(result);    
    keyRequest *key_request = &result->key_requests[result->num++];
    key_request->level = level;
    key_request->key = key;
    key_request->type = KEYREQUEST_TYPE_LEX;
    key_request->zl.reverse = reverse;
    key_request->zl.rangespec = rangespec; 
    key_request->zl.limit = limit;
    key_request->cmd_intention = cmd_intention;
    key_request->cmd_intention_flags = cmd_intention_flags;
    key_request->dbid = dbid;
}

/* Note that key&subkeys ownership moved */
void getKeyRequestsAppendSubkeyResult(getKeyRequestsResult *result, int level,
        robj *key, int num_subkeys, robj **subkeys, int cmd_intention,
        int cmd_intention_flags, int dbid) {
    keyRequest *key_request = getKeyRequestsAppendCommonResult(result,level,
            key,cmd_intention,cmd_intention_flags,dbid);

    key_request->type = KEYREQUEST_TYPE_SUBKEY;
    key_request->b.num_subkeys = num_subkeys;
    key_request->b.subkeys = subkeys;
}

void releaseKeyRequests(getKeyRequestsResult *result) {
    for (int i = 0; i < result->num; i++) {
        keyRequest *key_request = result->key_requests + i;
        keyRequestDeinit(key_request);
    }
}

void getKeyRequestsFreeResult(getKeyRequestsResult *result) {
    if (result && result->key_requests != result->buffer) {
        zfree(result->key_requests);
    }
}

/* NOTE that result.{key,subkeys} are ONLY REFS to client argv (since client
 * outlives getKeysResult if no swap action happend. key, subkey will be 
  * copied (using incrRefCount) when async swap acutally proceed. */
static int _getSingleCmdKeyRequests(int dbid, struct redisCommand* cmd,
        robj** argv, int argc, getKeyRequestsResult *result) {
    if (cmd->getkeyrequests_proc == NULL) {
        int i, numkeys;
        getKeysResult keys = GETKEYS_RESULT_INIT;
        /* whole key swaping, swaps defined by command arity. */
        numkeys = getKeysFromCommand(cmd,argv,argc,&keys);
        getKeyRequestsPrepareResult(result,result->num+numkeys);
        for (i = 0; i < numkeys; i++) {
            robj *key = argv[keys.keys[i]];

            incrRefCount(key);
            getKeyRequestsAppendSubkeyResult(result,REQUEST_LEVEL_KEY,key,0,NULL,
                    cmd->intention,cmd->intention_flags,dbid);
        }
        getKeysFreeResult(&keys); 
        return 0;
    } else if (cmd->flags & CMD_MODULE) {
        /* TODO support module */
    } else {
        return cmd->getkeyrequests_proc(dbid,cmd,argv,argc,result);
    }
    return 0;
}

static void getSingleCmdKeyRequests(client *c, getKeyRequestsResult *result) {
    _getSingleCmdKeyRequests(c->db->id,c->cmd,c->argv,c->argc,result);
}

/*TODO support select in multi/exec */
void getKeyRequests(client *c, getKeyRequestsResult *result) {
    getKeyRequestsPrepareResult(result, MAX_KEYREQUESTS_BUFFER);

    if ((c->flags & CLIENT_MULTI) && 
            !(c->flags & (CLIENT_DIRTY_CAS | CLIENT_DIRTY_EXEC)) &&
            (c->cmd->proc == execCommand || isGtidExecCommand(c))) {
        /* if current is EXEC, we get swaps for all queue commands. */
        robj **orig_argv;
        int i, orig_argc;
        redisDb *orig_db;
        struct redisCommand *orig_cmd;

        orig_argv = c->argv;
        orig_argc = c->argc;
        orig_cmd = c->cmd;
        orig_db = c->db;

        if (isGtidExecCommand(c)) {
            long long dbid;
            if (getLongLongFromObject(c->argv[2],&dbid)) return;
            if (dbid < 0 || dbid > server.dbnum)  return;
            c->db = server.db + dbid;
        }

        for (i = 0; i < c->mstate.count; i++) {
            int prev_keyrequest_num = result->num;

            c->argc = c->mstate.commands[i].argc;
            c->argv = c->mstate.commands[i].argv;
            c->cmd = c->mstate.commands[i].cmd;

            getSingleCmdKeyRequests(c, result);

            for (int j = prev_keyrequest_num; j < result->num; j++) {
                result->key_requests[j].list_arg_rewrite[0].mstate_idx = i;
                result->key_requests[j].list_arg_rewrite[1].mstate_idx = i;
            }
        }

        c->argv = orig_argv;
        c->argc = orig_argc;
        c->cmd = orig_cmd;
        c->db = orig_db;
    } else {
        getSingleCmdKeyRequests(c, result);
    }
}

int getKeyRequestsNone(int dbid, struct redisCommand *cmd, robj **argv, int argc,
        getKeyRequestsResult *result) {
    UNUSED(dbid);
    UNUSED(cmd);
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(result);
    return 0;
}

/* Used by flushdb/flushall to get global scs(similar to table lock). */
int getKeyRequestsGlobal(int dbid, struct redisCommand *cmd, robj **argv,
        int argc, getKeyRequestsResult *result) {
    UNUSED(cmd);
    UNUSED(argc);
    UNUSED(argv);
    getKeyRequestsPrepareResult(result,result->num+ 1);
    getKeyRequestsAppendSubkeyResult(result,REQUEST_LEVEL_SVR,NULL,0,NULL,
            cmd->intention,cmd->intention_flags,dbid);
    return 0;
}

int getKeyRequestsMetaScan(int dbid, struct redisCommand *cmd, robj **argv,
        int argc, struct getKeyRequestsResult *result) {
    char randbuf[16] = {0};
    robj *randkey;
    UNUSED(cmd);
    UNUSED(argc);
    UNUSED(argv);
    getRandomHexChars(randbuf,sizeof(randbuf));
    randkey = createStringObject(randbuf,sizeof(randbuf));
    getKeyRequestsPrepareResult(result,result->num+ 1);
    getKeyRequestsAppendSubkeyResult(result,REQUEST_LEVEL_KEY,randkey,0,NULL,
            cmd->intention,cmd->intention_flags,dbid);
    return 0;
}

int getKeyRequestsOneDestKeyMultiSrcKeys(int dbid, struct redisCommand *cmd, robj **argv,
                                         int argc, struct getKeyRequestsResult *result, int dest_key_Index,
                                                 int first_src_key, int last_src_key) {
    UNUSED(cmd);
    if (last_src_key < 0) last_src_key += argc;
    getKeyRequestsPrepareResult(result, result->num + 1 + last_src_key - first_src_key + 1);

    incrRefCount(argv[dest_key_Index]);
    getKeyRequestsAppendSubkeyResult(result,REQUEST_LEVEL_KEY,argv[dest_key_Index], 0, NULL,
                               SWAP_IN, SWAP_IN_DEL, dbid);
    for(int i = first_src_key; i <= last_src_key; i++) {
        incrRefCount(argv[i]);
        getKeyRequestsAppendSubkeyResult(result,REQUEST_LEVEL_KEY,argv[i], 0, NULL,
                                   SWAP_IN,0, dbid);
    }

    return 0;
}

int getKeyRequestsBitop(int dbid, struct redisCommand *cmd, robj **argv,
        int argc, struct getKeyRequestsResult *result) {
    return getKeyRequestsOneDestKeyMultiSrcKeys(dbid, cmd, argv, argc, result, 2, 3, -1);
}

int getKeyRequestsSort(int dbid, struct redisCommand *cmd, robj **argv,
        int argc, struct getKeyRequestsResult *result) {
    int i, j;
    robj *storekey = NULL;

    UNUSED(cmd);

    struct {
        char *name;
        int skip;
    } skiplist[] = {
            {"limit", 2},
            {"get", 1},
            {"by", 1},
            {NULL, 0} /* End of elements. */
    };

    for (i = 2; i < argc; i++) {
        if (!strcasecmp(argv[i]->ptr,"store") && i+1 < argc) {
            /* we don't break after store key found to be sure
             * to process the *last* "STORE" option if multiple
             * ones are provided. This is same behavior as SORT. */
            storekey = argv[i+1];
        }
        for (j = 0; skiplist[j].name != NULL; j++) {
            if (!strcasecmp(argv[i]->ptr,skiplist[j].name)) {
                i += skiplist[j].skip;
                break;
            }
        }
    }

    getKeyRequestsPrepareResult(result,result->num + (storekey ? 2 : 1));
    incrRefCount(argv[1]);
    getKeyRequestsAppendSubkeyResult(result,REQUEST_LEVEL_KEY,argv[1],0,NULL,
                               SWAP_IN, 0, dbid);
    if (storekey) {
        incrRefCount(storekey);
        getKeyRequestsAppendSubkeyResult(result,REQUEST_LEVEL_KEY,storekey,0,NULL,
                                   SWAP_IN, SWAP_IN_DEL, dbid);
    }

    return C_OK;
}

int getKeyRequestsZunionInterDiffGeneric(int dbid, struct redisCommand *cmd, robj **argv, int argc,
        struct getKeyRequestsResult *result, int op) {
    UNUSED(op);
    long long setnum;
    if (getLongLongFromObject(argv[2], &setnum) != C_OK) {
        return C_ERR;
    }
    if (setnum < 1 || setnum + 3 > argc) {
        return C_ERR;
    }

    return getKeyRequestsOneDestKeyMultiSrcKeys(dbid, cmd, argv, argc, result, 1, 3, 2 + setnum);
}

int getKeyRequestsZunionstore(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result) {
    return getKeyRequestsZunionInterDiffGeneric(dbid, cmd, argv, argc, result, SET_OP_UNION);
}

int getKeyRequestsZinterstore(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result) {
    return getKeyRequestsZunionInterDiffGeneric(dbid, cmd, argv, argc, result, SET_OP_INTER);
}
int getKeyRequestsZdiffstore(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result) {
    return getKeyRequestsZunionInterDiffGeneric(dbid, cmd, argv, argc, result, SET_OP_DIFF);
}

#define GETKEYS_RESULT_SUBKEYS_INIT_LEN 8
#define GETKEYS_RESULT_SUBKEYS_LINER_LEN 1024

int getKeyRequestsSingleKeyWithSubkeys(int dbid, struct redisCommand *cmd, robj **argv,
        int argc, struct getKeyRequestsResult *result,
        int key_index, int first_subkey, int last_subkey, int subkey_step) {
    int i, num = 0, capacity = GETKEYS_RESULT_SUBKEYS_INIT_LEN;
    robj *key, **subkeys = NULL;
    UNUSED(cmd);

    subkeys = zmalloc(capacity*sizeof(robj*));
    getKeyRequestsPrepareResult(result,result->num+1);

    key = argv[key_index];
    incrRefCount(key);
    
    if (last_subkey < 0) last_subkey += argc;
    for (i = first_subkey; i <= last_subkey; i += subkey_step) {
        robj *subkey = argv[i];
        if (num >= capacity) {
            if (capacity < GETKEYS_RESULT_SUBKEYS_LINER_LEN)
                capacity *= 2;
            else
                capacity += GETKEYS_RESULT_SUBKEYS_LINER_LEN;

            subkeys = zrealloc(subkeys, capacity*sizeof(robj*));
        }
        incrRefCount(subkey);
        subkeys[num++] = subkey;
    }
    getKeyRequestsAppendSubkeyResult(result,REQUEST_LEVEL_KEY,key,num,subkeys,
            cmd->intention,cmd->intention_flags,dbid);

    return 0;
}

int getKeyRequestsHset(int dbid,struct redisCommand *cmd, robj **argv, int argc,
        struct getKeyRequestsResult *result) {
    return getKeyRequestsSingleKeyWithSubkeys(dbid,cmd,argv,argc,result,1,2,-1,2);
}

int getKeyRequestsHmget(int dbid, struct redisCommand *cmd, robj **argv, int argc,
        struct getKeyRequestsResult *result) {
    return getKeyRequestsSingleKeyWithSubkeys(dbid,cmd,argv,argc,result,1,2,-1,1);
}

int getKeyRequestSmembers(int dbid, struct redisCommand *cmd, robj **argv, int argc,
                          struct getKeyRequestsResult *result) {
    return getKeyRequestsSingleKeyWithSubkeys(dbid,cmd,argv,argc,result,1,2,-1,1);
}

int getKeyRequestSmove(int dbid, struct redisCommand *cmd, robj **argv, int argc,
                       struct getKeyRequestsResult *result) {
    robj** subkeys;

    UNUSED(argc), UNUSED(cmd);

    getKeyRequestsPrepareResult(result, result->num + 2);

    incrRefCount(argv[1]);
    incrRefCount(argv[3]);
    subkeys = zmalloc(sizeof(robj*));
    subkeys[0] = argv[3];
    getKeyRequestsAppendSubkeyResult(result,REQUEST_LEVEL_KEY,argv[1], 1, subkeys,
                               SWAP_IN, SWAP_IN_DEL, dbid);

    incrRefCount(argv[2]);
    incrRefCount(argv[3]);
    subkeys = zmalloc(sizeof(robj*));
    subkeys[0] = argv[3];
    getKeyRequestsAppendSubkeyResult(result,REQUEST_LEVEL_KEY,argv[2], 1, subkeys,
                               SWAP_IN, 0, dbid);

    return 0;
}

int getKeyRequestsSinterstore(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result) {
    return getKeyRequestsOneDestKeyMultiSrcKeys(dbid, cmd, argv, argc, result, 1, 2, -1);
}

/* Key */
void getKeyRequestsSingleKey(getKeyRequestsResult *result,
        robj *key/*ref*/, int cmd_intention, int cmd_intention_flags, int dbid) {
    keyRequest *key_request;
    incrRefCount(key);
    key_request = getKeyRequestsAppendCommonResult(result,
            REQUEST_LEVEL_KEY,key,cmd_intention,cmd_intention_flags,dbid);
    key_request->type = KEYREQUEST_TYPE_KEY;
}

/* Segment */
void getKeyRequestsAppendRangeResult(getKeyRequestsResult *result, int level,
        robj *key, int arg_rewrite0, int arg_rewrite1, int num_ranges,
        range *ranges, int cmd_intention, int cmd_intention_flags, int dbid) {
    keyRequest *key_request = getKeyRequestsAppendCommonResult(result,level,
            key,cmd_intention,cmd_intention_flags,dbid);

    key_request->type = KEYREQUEST_TYPE_RANGE;
    key_request->l.num_ranges = num_ranges;
    key_request->l.ranges = ranges;
    key_request->list_arg_rewrite[0].arg_idx = arg_rewrite0;
    key_request->list_arg_rewrite[1].arg_idx = arg_rewrite1;
}

/* There are no command with more that 2 ranges request. */
#define GETKEYS_RESULT_SEGMENTS_MAX_LEN 2

int getKeyRequestsSingleKeyWithRanges(int dbid, struct redisCommand *cmd, robj **argv,
        int argc, struct getKeyRequestsResult *result, int key_index, 
        int arg_rewrite0, int arg_rewrite1, int num_ranges, ...) {
    va_list ap;
    int i, capacity = GETKEYS_RESULT_SEGMENTS_MAX_LEN;
    robj *key;
    range *ranges = NULL;

    UNUSED(cmd), UNUSED(argc);
    serverAssert(capacity >= num_ranges);

    ranges = zmalloc(capacity*sizeof(range));
    getKeyRequestsPrepareResult(result,result->num+1);

    key = argv[key_index];
    incrRefCount(key);
    
    va_start(ap,num_ranges);
    for (i = 0; i < num_ranges; i++) {
        long start = va_arg(ap,long);
        long end = va_arg(ap,long);
        ranges[i].start = start;
        ranges[i].end = end;
    }
    va_end(ap);
            
    getKeyRequestsAppendRangeResult(result,REQUEST_LEVEL_KEY,key,
            arg_rewrite0,arg_rewrite1,num_ranges,ranges,cmd->intention,
            cmd->intention_flags,dbid);

    return 0;
}

int getKeyRequestsLpop(int dbid, struct redisCommand *cmd, robj **argv,
        int argc, struct getKeyRequestsResult *result) {
    long long count = 1, value;

    if (argc >= 3) {
        if (getLongLongFromObject(argv[2],&value) == C_OK)
            count = value;
    }

    getKeyRequestsSingleKeyWithRanges(dbid,cmd,argv,argc,
            result,1,-1,-1,1/*num_ranges*/,0,count);
    return 0;

}

int getKeyRequestsBlpop(int dbid, struct redisCommand *cmd, robj **argv,
        int argc, struct getKeyRequestsResult *result) {
    for (int i = 1; i < argc-1; i++) {
        getKeyRequestsSingleKeyWithRanges(dbid,cmd,argv,argc,
                result,i,-1,-1,1/*num_ranges*/,0,1);
    }
    return 0;
}

int getKeyRequestsRpop(int dbid, struct redisCommand *cmd, robj **argv,
        int argc, struct getKeyRequestsResult *result) {
    long long count = 1, value;

    if (argc >= 3) {
        if (getLongLongFromObject(argv[2],&value) == C_OK)
            count = value;
    }

    getKeyRequestsSingleKeyWithRanges(dbid,cmd,argv,argc,
            result,1,-1,-1,1/*num_ranges*/,-count,-1);
    return 0;
}

int getKeyRequestsBrpop(int dbid, struct redisCommand *cmd, robj **argv,
        int argc, struct getKeyRequestsResult *result) {
    for (int i = 1; i < argc-1; i++) {
        getKeyRequestsSingleKeyWithRanges(dbid,cmd,argv,argc,
                result,i,-1,-1,1/*num_ranges*/,-1,-1);
    }
    return 0;
}

int getKeyRequestsRpoplpush(int dbid, struct redisCommand *cmd, robj **argv,
        int argc, struct getKeyRequestsResult *result) {
    getKeyRequestsSingleKeyWithRanges(dbid,cmd,argv,argc,
            result,1,-1,-1,1/*num_ranges*/,-1,-1); /* source */
    getKeyRequestsSingleKey(result,argv[2],SWAP_IN,SWAP_IN_META,dbid);
    return 0;
}

int getKeyRequestsLmove(int dbid, struct redisCommand *cmd, robj **argv,
        int argc, struct getKeyRequestsResult *result) {
    long start, end;
    if ((argc != 5/*lmove*/ && argc != 6/*blmove*/) ||
        (strcasecmp(argv[3]->ptr,"left") && strcasecmp(argv[3]->ptr,"right")) ||
        (strcasecmp(argv[4]->ptr,"left") && strcasecmp(argv[4]->ptr,"right"))) {
        return -1;
    }

    if (!strcasecmp(argv[3]->ptr,"left")) {
        start = 0, end = 0;
    } else {
        start = -1, end = -1;
    }
    /* source */
    getKeyRequestsSingleKeyWithRanges(dbid,cmd,argv,argc,
            result,1,-1,-1,1/*num_ranges*/,start,end);
    /* destination */
    getKeyRequestsSingleKey(result,argv[2],SWAP_IN,SWAP_IN_META,dbid);
    return 0;
}

int getKeyRequestsLindex(int dbid, struct redisCommand *cmd, robj **argv,
        int argc, struct getKeyRequestsResult *result) {
    long long index;
    if (getLongLongFromObject(argv[2],&index) != C_OK) return -1;
    getKeyRequestsSingleKeyWithRanges(dbid,cmd,argv,argc,
            result,1,2,-1,1/*num_ranges*/,index,index); 
    return 0;
}

int getKeyRequestsLrange(int dbid, struct redisCommand *cmd, robj **argv,
        int argc, struct getKeyRequestsResult *result) {
    long long start, end;
    if (getLongLongFromObject(argv[2],&start) != C_OK) return -1;
    if (getLongLongFromObject(argv[3],&end) != C_OK) return -1;
    getKeyRequestsSingleKeyWithRanges(dbid,cmd,argv,argc,
            result,1,2,3,1/*num_ranges*/,start,end); 
    return 0;
}

int getKeyRequestsLtrim(int dbid, struct redisCommand *cmd, robj **argv,
        int argc, struct getKeyRequestsResult *result) {
    long long start, stop;
    if (getLongLongFromObject(argv[2],&start) != C_OK) return -1;
    if (getLongLongFromObject(argv[3],&stop) != C_OK) return -1;
    getKeyRequestsSingleKeyWithRanges(dbid,cmd,argv,argc,
            result,1,2,3,1/*num_ranges*/,2,0,start-1,stop+1,-1); 
    return 0;
}
/** zset **/
int getKeyRequestsZAdd(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result) {
    int first_score = 2;
    while(first_score < argc) {
        char *opt = argv[first_score]->ptr;
        if (
            strcasecmp(opt,"nx") != 0 &&
            strcasecmp(opt,"xx") != 0 &&
            strcasecmp(opt,"ch") != 0 &&
            strcasecmp(opt,"incr") != 0 &&
            strcasecmp(opt,"gt") != 0 &&
            strcasecmp(opt,"lt") != 0 
        ) {
            break;
        }
        first_score++;
    }
    return getKeyRequestsSingleKeyWithSubkeys(dbid, cmd, argv, argc, result, 1, first_score + 1, -1, 2);
}

int getKeyRequestsZScore(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result) {
    return getKeyRequestsSingleKeyWithSubkeys(dbid, cmd,argv,argc,result,1,2,-1,1);
}

int getKeyRequestsZincrby(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result) {
    return getKeyRequestsSingleKeyWithSubkeys(dbid, cmd, argv, argc, result, 1, 3, -1, 2);
}

#define ZMIN -1
#define ZMAX 1
int getKeyRequestsZpopGeneric(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result, int flags) {
    UNUSED(cmd), UNUSED(flags);
    getKeyRequestsPrepareResult(result,result->num+ argc - 2);
    for(int i = 1; i < argc - 1; i++) {
        incrRefCount(argv[i]);
        getKeyRequestsAppendSubkeyResult(result, REQUEST_LEVEL_KEY, argv[i], 0, NULL, SWAP_IN, SWAP_IN_DEL, dbid);
    }
    return C_OK;
}

int getKeyRequestsZpopMin(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result) {
    return getKeyRequestsZpopGeneric(dbid, cmd, argv, argc, result, ZMIN);  
}

int getKeyRequestsZpopMax(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result) {
    return getKeyRequestsZpopGeneric(dbid, cmd, argv, argc, result, ZMAX);    
}

int getKeyRequestsZrangestore(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result) {
    UNUSED(cmd), UNUSED(argc);
    getKeyRequestsPrepareResult(result,result->num+ 2);
    for(int i = 1; i < 3; i++) {
        incrRefCount(argv[i]);
        getKeyRequestsAppendSubkeyResult(result, REQUEST_LEVEL_KEY, argv[i], 0, NULL, SWAP_IN, SWAP_IN_DEL, dbid);
    }
    return C_OK;
}


typedef enum {
    ZRANGE_DIRECTION_AUTO = 0,
    ZRANGE_DIRECTION_FORWARD,
    ZRANGE_DIRECTION_REVERSE
} zrange_direction;
typedef enum {
    ZRANGE_AUTO = 0,
    ZRANGE_RANK,
    ZRANGE_SCORE,
    ZRANGE_LEX,
} zrange_type;

int getKeyRequestsZrangeGeneric(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result, zrange_type rangetype, zrange_direction direction) {
    if (argc < 4) return C_ERR;
    robj *minobj ,*maxobj; 
    int argc_start = 1;
    long long opt_offset = 0, opt_limit = 0;
    /* Step 1: Skip the <src> <min> <max> args and parse remaining optional arguments. */
    for (int j=argc_start + 3; j < argc; j++) {
        int leftargs = argc-j-1;
        if (!strcasecmp(argv[j]->ptr,"withscores")) {
            /* opt_withscores = 1; */
        } else if (!strcasecmp(argv[j]->ptr,"limit") && leftargs >= 2) {
            
            if (getLongLongFromObject(argv[j+1], &opt_offset) != C_OK
            || getLongLongFromObject(argv[j+2], &opt_limit) != C_OK) {
                return C_ERR;
            }
            j += 2;
        } else if (direction == ZRANGE_DIRECTION_AUTO &&
                   !strcasecmp(argv[j]->ptr,"rev"))
        {
            direction = ZRANGE_DIRECTION_REVERSE;
        } else if (rangetype == ZRANGE_AUTO &&
                   !strcasecmp(argv[j]->ptr,"bylex"))
        {
            rangetype = ZRANGE_LEX;
        } else if (rangetype == ZRANGE_AUTO &&
                   !strcasecmp(argv[j]->ptr,"byscore"))
        {
            rangetype = ZRANGE_SCORE;
        } else {
            
            return C_ERR;
        }
    }
    if (direction == ZRANGE_DIRECTION_REVERSE) {
        minobj = argv[3];
        maxobj = argv[2];
    } else {
        minobj = argv[2];
        maxobj = argv[3];
    } 
    robj* key = argv[1];
    incrRefCount(key);

    getKeyRequestsPrepareResult(result,result->num+ 1);

    switch (rangetype)
    {
    case ZRANGE_SCORE:
        {
            zrangespec* spec = zmalloc(sizeof(zrangespec));
            /* code */
            if (zslParseRange(minobj, maxobj, spec) != C_OK) {
                zfree(spec);
                return C_ERR;
            }
            getKeyRequestsAppendScoreResult(result, REQUEST_LEVEL_KEY, key, direction == ZRANGE_DIRECTION_REVERSE, spec, opt_offset + opt_limit,cmd->intention, cmd->intention_flags, dbid);
        }
        break;
    case ZRANGE_LEX:
        {
            zlexrangespec* lexrange = zmalloc(sizeof(zlexrangespec));
            if (zslParseLexRange(minobj, maxobj, lexrange) != C_OK) {
                zfree(lexrange);
                return C_ERR;
            }
            getKeyRequestsAppendLexeResult(result, REQUEST_LEVEL_KEY, key, direction == ZRANGE_DIRECTION_REVERSE, lexrange, opt_offset + opt_limit, cmd->intention, cmd->intention_flags, dbid);
        }
        break;
    default:
        getKeyRequestsAppendSubkeyResult(result, REQUEST_LEVEL_KEY, key, 0, NULL, cmd->intention, cmd->intention_flags, dbid);
        break;
    }
    
    return C_OK;
}

int getKeyRequestsZrange(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result) {
    return getKeyRequestsZrangeGeneric(dbid, cmd, argv, argc, result, ZRANGE_AUTO, ZRANGE_DIRECTION_AUTO); 
}

int getKeyRequestsZrangeByScore(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result) {
    return getKeyRequestsZrangeGeneric(dbid, cmd, argv, argc, result, ZRANGE_SCORE, ZRANGE_DIRECTION_FORWARD);
}

int getKeyRequestsZrevrangeByScore(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result) { 
    return getKeyRequestsZrangeGeneric(dbid, cmd, argv, argc, result, ZRANGE_SCORE, ZRANGE_DIRECTION_REVERSE);
}

int getKeyRequestsZremRangeByScore1(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result) {
    if (argc < 4) return C_ERR;
    robj* minobj = argv[2];
    robj* maxobj = argv[3];
    zrangespec* spec = zmalloc(sizeof(zrangespec));
    if (zslParseRange(minobj, maxobj, spec) != C_OK) {
        zfree(spec);
        return C_ERR;
    }
    robj* key = argv[1];
    incrRefCount(key);
    getKeyRequestsPrepareResult(result,result->num+ 1);
    getKeyRequestsAppendScoreResult(result, REQUEST_LEVEL_KEY, key, 0, spec, 0, cmd->intention, cmd->intention_flags, dbid);
    return C_OK;
}

int getKeyRequestsZrangeByLexGeneric(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result, zrange_direction direction) {
    if (argc < 4) return C_ERR;
    zlexrangespec* lexrange = zmalloc(sizeof(zlexrangespec));
    if (zslParseLexRange(argv[2],argv[3], lexrange) != C_OK) {
        zfree(lexrange);
        return C_ERR;
    }
    robj* key = argv[1];
    incrRefCount(key);
    getKeyRequestsPrepareResult(result,result->num+ 1);
    getKeyRequestsAppendLexeResult(result, REQUEST_LEVEL_KEY, key, direction == ZRANGE_DIRECTION_REVERSE, lexrange, 0, cmd->intention, cmd->intention_flags, dbid);
    return C_OK;
}
int getKeyRequestsZrevrangeByLex(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result) {
    return getKeyRequestsZrangeGeneric(dbid, cmd, argv, argc, result, ZRANGE_LEX, ZRANGE_DIRECTION_REVERSE);
}

int getKeyRequestsZrangeByLex(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result) {
    return getKeyRequestsZrangeGeneric(dbid, cmd, argv, argc, result, ZRANGE_LEX, ZRANGE_DIRECTION_FORWARD);
}

int getKeyRequestsZlexCount(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result) {
    return getKeyRequestsZrangeGeneric(dbid, cmd, argv, argc, result, ZRANGE_LEX, ZRANGE_DIRECTION_FORWARD);
}

int getKeyRequestsZremRangeByLex(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result) {
    return getKeyRequestsZrangeGeneric(dbid, cmd, argv, argc, result, ZRANGE_LEX, ZRANGE_DIRECTION_FORWARD);
}
/** geo **/
int getKeyRequestsGeoAdd(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result) {
    int first_score = 2;
    while(first_score < argc) {
        char *opt = argv[first_score]->ptr;
        if (
            strcasecmp(opt,"nx") != 0 &&
            strcasecmp(opt,"xx") != 0 &&
            strcasecmp(opt,"ch") != 0 
        ) {
            break;
        }
        first_score++;
    }
    return getKeyRequestsSingleKeyWithSubkeys(dbid, cmd, argv, argc, result, 1, first_score + 2, -1, 3);
}

int getKeyRequestsGeoDist(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result) {
    return getKeyRequestsSingleKeyWithSubkeys(dbid, cmd, argv, argc, result, 1, 2, -2, 1);
}

int getKeyRequestsGeoHash(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result) {
    return getKeyRequestsSingleKeyWithSubkeys(dbid, cmd, argv, argc, result, 1, 2, -1, 1);
}

int getKeyRequestsGeoRadius(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result) {
    robj* storekey = NULL;
    UNUSED(cmd);
    for(int i =0; i < argc; i++) {
        if (!strcasecmp(argv[i]->ptr, "store") && (i+1) < argc) {
            storekey = argv[i+1];
            i++;
        } else if(!strcasecmp(argv[i]->ptr, "storedist") && (i+1) < argc) {
            storekey = argv[i+1];
            i++;
        }
    }
    getKeyRequestsPrepareResult(result,result->num+ 2);
    incrRefCount(argv[1]);
    getKeyRequestsAppendSubkeyResult(result, REQUEST_LEVEL_KEY, argv[1], 0, NULL, SWAP_IN, 0, dbid);
    if (storekey != NULL) {
        incrRefCount(storekey);
        getKeyRequestsAppendSubkeyResult(result, REQUEST_LEVEL_KEY, storekey, 0, NULL, SWAP_IN, SWAP_IN_DEL, dbid);
    }
    return C_OK;
}

int getKeyRequestsGeoSearchStore(int dbid, struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result) {
    UNUSED(cmd), UNUSED(argc);
    getKeyRequestsPrepareResult(result,result->num+ 2);
    incrRefCount(argv[1]);
    getKeyRequestsAppendSubkeyResult(result, REQUEST_LEVEL_KEY, argv[1], 0, NULL, SWAP_IN, SWAP_IN_DEL, dbid);
    
    incrRefCount(argv[2]);
    getKeyRequestsAppendSubkeyResult(result, REQUEST_LEVEL_KEY, argv[2], 0, NULL, SWAP_IN, 0, dbid);
    return C_OK;
}

static inline void getKeyRequestsGtidArgRewriteAdjust(
        struct getKeyRequestsResult *result, int orig_krs_num, int start_index) {
    for (int i = orig_krs_num; i < result->num; i++) {
        keyRequest *kr = result->key_requests+i;
        if (kr->list_arg_rewrite[0].arg_idx > 0) kr->list_arg_rewrite[0].arg_idx += start_index;
        if (kr->list_arg_rewrite[1].arg_idx > 0) kr->list_arg_rewrite[1].arg_idx += start_index;
    }
}

int getKeyRequestsGtid(int dbid, struct redisCommand *cmd, robj **argv,
        int argc, struct getKeyRequestsResult *result) {
    int start_index, exec_dbid, orig_num;
    struct redisCommand* exec_cmd;
    long long value;

    UNUSED(dbid), UNUSED(cmd);

    if (getLongLongFromObject(argv[2],&value)) return C_ERR;
    if (value < 0 || value > server.dbnum)  return C_ERR;
    exec_dbid = (int)value;

    if (strncmp(argv[3]->ptr, "/*", 2))
        start_index = 3;
    else
        start_index = 4;

    orig_num = result->num;

    exec_cmd = lookupCommandByCString(argv[start_index]->ptr);
    if (_getSingleCmdKeyRequests(exec_dbid,exec_cmd,argv+start_index,
            argc-start_index,result)) return C_ERR;

    getKeyRequestsGtidArgRewriteAdjust(result,orig_num,start_index);
    return C_OK;
}

int getKeyRequestsGtidAuto(int dbid, struct redisCommand *cmd, robj **argv,
        int argc, struct getKeyRequestsResult *result) {
    UNUSED(cmd);
    int orig_num = result->num, start_index = 2;
    struct redisCommand* exec_cmd = lookupCommandByCString(argv[2]->ptr);
    if (_getSingleCmdKeyRequests(dbid,exec_cmd,argv+start_index,argc-start_index,result))
        return C_ERR;
    getKeyRequestsGtidArgRewriteAdjust(result,orig_num,start_index);
    return C_OK;
}

#ifdef REDIS_TEST

void rewriteResetClientCommandCString(client *c, int argc, ...) {
    va_list ap;
    int j;
    robj **argv; /* The new argument vector */

    argv = zmalloc(sizeof(robj*)*argc);
    va_start(ap,argc);
    for (j = 0; j < argc; j++) {
        char *a = va_arg(ap, char*);
        argv[j] = createStringObject(a, strlen(a));
    }
    replaceClientCommandVector(c, argc, argv);
    va_end(ap);
}

void initServerConfig(void);
int swapCmdTest(int argc, char *argv[], int accurate) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(accurate);

    int error = 0;
    client *c;

    TEST("cmd: init") {
        initServerConfig();
        ACLInit();
        server.hz = 10;
        c = createClient(NULL);
        initTestRedisDb();
        selectDb(c,0);
    }

    TEST("cmd: no key") {
        getKeyRequestsResult result = GET_KEYREQUESTS_RESULT_INIT;
        rewriteResetClientCommandCString(c,1,"PING");
        getKeyRequests(c,&result);
        test_assert(result.num == 0);
        releaseKeyRequests(&result);
        getKeyRequestsFreeResult(&result);
    } 

    TEST("cmd: single key") {
        getKeyRequestsResult result = GET_KEYREQUESTS_RESULT_INIT;
        rewriteResetClientCommandCString(c,2,"GET","KEY");
        getKeyRequests(c,&result);
        test_assert(result.num == 1);
        test_assert(!strcmp(result.key_requests[0].key->ptr, "KEY"));
        releaseKeyRequests(&result);
        getKeyRequestsFreeResult(&result);
    }

    TEST("cmd: multiple keys") {
        getKeyRequestsResult result = GET_KEYREQUESTS_RESULT_INIT;
        rewriteResetClientCommandCString(c,3,"MGET","KEY1","KEY2");
        getKeyRequests(c,&result);
        test_assert(result.num == 2);
        test_assert(!strcmp(result.key_requests[0].key->ptr, "KEY1"));
        test_assert(!strcmp(result.key_requests[1].key->ptr, "KEY2"));
        releaseKeyRequests(&result);
        getKeyRequestsFreeResult(&result);
    }

    TEST("cmd: multi/exec") {
        getKeyRequestsResult result = GET_KEYREQUESTS_RESULT_INIT;
        c->flags |= CLIENT_MULTI;
        rewriteResetClientCommandCString(c,1,"PING");
        queueMultiCommand(c);
        rewriteResetClientCommandCString(c,3,"MGET","KEY1","KEY2");
        queueMultiCommand(c);
        rewriteResetClientCommandCString(c,3,"SET","KEY3","VAL3");
        queueMultiCommand(c);
        rewriteResetClientCommandCString(c,1,"EXEC");
        getKeyRequests(c,&result);
        test_assert(result.num == 3);
        test_assert(!strcmp(result.key_requests[0].key->ptr, "KEY1"));
        test_assert(result.key_requests[0].b.subkeys == NULL);
        test_assert(!strcmp(result.key_requests[1].key->ptr, "KEY2"));
        test_assert(result.key_requests[1].b.subkeys == NULL);
        test_assert(!strcmp(result.key_requests[2].key->ptr, "KEY3"));
        test_assert(result.key_requests[2].b.subkeys == NULL);
        test_assert(result.key_requests[2].cmd_intention == SWAP_IN);
        test_assert(result.key_requests[2].cmd_intention_flags == SWAP_IN_OVERWRITE);
        releaseKeyRequests(&result);
        getKeyRequestsFreeResult(&result);
        discardTransaction(c);
    }

    TEST("cmd: hash subkeys") {
        getKeyRequestsResult result = GET_KEYREQUESTS_RESULT_INIT;
        rewriteResetClientCommandCString(c,5,"HMGET","KEY","F1","F2","F3");
        getKeyRequests(c,&result);
        test_assert(result.num == 1);
        test_assert(!strcmp(result.key_requests[0].key->ptr, "KEY"));
        test_assert(result.key_requests[0].b.num_subkeys == 3);
        test_assert(!strcmp(result.key_requests[0].b.subkeys[0]->ptr, "F1"));
        test_assert(!strcmp(result.key_requests[0].b.subkeys[1]->ptr, "F2"));
        test_assert(!strcmp(result.key_requests[0].b.subkeys[2]->ptr, "F3"));
        releaseKeyRequests(&result);
        getKeyRequestsFreeResult(&result);
    }

    TEST("cmd: multi/exec hash subkeys") {
        getKeyRequestsResult result = GET_KEYREQUESTS_RESULT_INIT;
        c->flags |= CLIENT_MULTI;
        rewriteResetClientCommandCString(c,1,"PING");
        queueMultiCommand(c);
        rewriteResetClientCommandCString(c,3,"MGET","KEY1","KEY2");
        queueMultiCommand(c);
        rewriteResetClientCommandCString(c,5,"HMGET","HASH","F1","F2","F3");
        queueMultiCommand(c);
        rewriteResetClientCommandCString(c,1,"EXEC");
        getKeyRequests(c,&result);
        test_assert(result.num == 3);
        test_assert(!strcmp(result.key_requests[0].key->ptr, "KEY1"));
        test_assert(result.key_requests[0].b.subkeys == NULL);
        test_assert(!strcmp(result.key_requests[1].key->ptr, "KEY2"));
        test_assert(result.key_requests[1].b.subkeys == NULL);
        test_assert(!strcmp(result.key_requests[2].key->ptr, "HASH"));
        test_assert(result.key_requests[2].b.num_subkeys == 3);
        test_assert(!strcmp(result.key_requests[2].b.subkeys[0]->ptr, "F1"));
        test_assert(!strcmp(result.key_requests[2].b.subkeys[1]->ptr, "F2"));
        test_assert(!strcmp(result.key_requests[2].b.subkeys[2]->ptr, "F3"));
        releaseKeyRequests(&result);
        getKeyRequestsFreeResult(&result);
        discardTransaction(c);
    }

    TEST("cmd: dispatch swap sequentially for reentrant-key request") {
        getKeyRequestsResult result = GET_KEYREQUESTS_RESULT_INIT;
        rewriteResetClientCommandCString(c,4,"MGET", "K1", "K2", "K1");
        getKeyRequests(c,&result);
        test_assert(result.num == 3);
        test_assert(!strcmp(result.key_requests[0].key->ptr, "K1"));
        test_assert(result.key_requests[0].cmd_intention == SWAP_IN);
        test_assert(result.key_requests[0].cmd_intention_flags == 0);
        test_assert(!strcmp(result.key_requests[1].key->ptr, "K2"));
        test_assert(!strcmp(result.key_requests[2].key->ptr, "K1"));
        releaseKeyRequests(&result);
        getKeyRequestsFreeResult(&result);
    }

    TEST("cmd: dispatch swap sequentially for reentrant-key request (multi)") {
        getKeyRequestsResult result = GET_KEYREQUESTS_RESULT_INIT;
        c->flags |= CLIENT_MULTI;
        rewriteResetClientCommandCString(c,3,"HMGET","HASH", "F1");
        queueMultiCommand(c);
        rewriteResetClientCommandCString(c,2,"DEL","HASH");
        queueMultiCommand(c);
        rewriteResetClientCommandCString(c,1,"EXEC");
        getKeyRequests(c,&result);
        test_assert(result.num == 2);
        test_assert(!strcmp(result.key_requests[0].key->ptr, "HASH"));
        test_assert(result.key_requests[0].b.num_subkeys == 1);
        test_assert(result.key_requests[0].cmd_intention == SWAP_IN);
        test_assert(result.key_requests[0].cmd_intention_flags == 0);
        test_assert(!strcmp(result.key_requests[1].key->ptr, "HASH"));
        test_assert(result.key_requests[1].b.subkeys == NULL);
        test_assert(result.key_requests[1].cmd_intention == SWAP_IN);
        test_assert(result.key_requests[1].cmd_intention_flags == SWAP_IN_DEL_MOCK_VALUE);
        releaseKeyRequests(&result);
        getKeyRequestsFreeResult(&result);
        discardTransaction(c);
    }

    TEST("cmd: dispatch swap sequentially with db/svr request") {
        getKeyRequestsResult result = GET_KEYREQUESTS_RESULT_INIT;
        c->flags |= CLIENT_MULTI;
        rewriteResetClientCommandCString(c,1,"PING");
        queueMultiCommand(c);
        rewriteResetClientCommandCString(c,1,"FLUSHDB");
        queueMultiCommand(c);
        rewriteResetClientCommandCString(c,1,"EXEC");
        getKeyRequests(c,&result);
        test_assert(result.num == 1);
        test_assert(result.key_requests[0].key == NULL);
        test_assert(result.key_requests[0].cmd_intention == SWAP_NOP);
        test_assert(result.key_requests[0].cmd_intention_flags == 0);
        releaseKeyRequests(&result);
        getKeyRequestsFreeResult(&result);
        discardTransaction(c);
    }

    TEST("cmd: dbid, cmd_intention, cmd_intention_flags set properly") {
        getKeyRequestsResult result = GET_KEYREQUESTS_RESULT_INIT;
        selectDb(c,1);
        c->flags |= CLIENT_MULTI;
        rewriteResetClientCommandCString(c,1,"PING");
        queueMultiCommand(c);
        rewriteResetClientCommandCString(c,3,"MGET","KEY1","KEY2");
        queueMultiCommand(c);
        rewriteResetClientCommandCString(c,5,"HDEL","HASH","F1","F2","F3");
        queueMultiCommand(c);
        rewriteResetClientCommandCString(c,1,"FLUSHDB");
        queueMultiCommand(c);
        rewriteResetClientCommandCString(c,1,"EXEC");
        getKeyRequests(c,&result);
        test_assert(result.num == 4);
        test_assert(!strcmp(result.key_requests[0].key->ptr, "KEY1"));
        test_assert(result.key_requests[0].b.subkeys == NULL);
        test_assert(result.key_requests[0].cmd_intention == SWAP_IN);
        test_assert(result.key_requests[0].cmd_intention_flags == 0);
        test_assert(result.key_requests[0].dbid == 1);
        test_assert(!strcmp(result.key_requests[1].key->ptr, "KEY2"));
        test_assert(result.key_requests[1].b.subkeys == NULL);
        test_assert(result.key_requests[1].cmd_intention == SWAP_IN);
        test_assert(result.key_requests[1].cmd_intention_flags == 0);
        test_assert(result.key_requests[1].dbid == 1);
        test_assert(!strcmp(result.key_requests[2].key->ptr, "HASH"));
        test_assert(!strcmp(result.key_requests[2].b.subkeys[0]->ptr, "F1"));
        test_assert(!strcmp(result.key_requests[2].b.subkeys[1]->ptr, "F2"));
        test_assert(!strcmp(result.key_requests[2].b.subkeys[2]->ptr, "F3"));
        test_assert(result.key_requests[2].cmd_intention == SWAP_IN);
        test_assert(result.key_requests[2].cmd_intention_flags == SWAP_IN_DEL);
        test_assert(result.key_requests[2].dbid == 1);
        test_assert(result.key_requests[3].key == NULL);
        test_assert(result.key_requests[3].cmd_intention == SWAP_NOP);
        test_assert(result.key_requests[3].cmd_intention_flags == 0);
        test_assert(result.key_requests[3].dbid == 1);
        releaseKeyRequests(&result);
        getKeyRequestsFreeResult(&result);
        discardTransaction(c);
    }

    TEST("cmd: encode/decode Scorekey") {

    }

    TEST("cmd: gtid") {
        getKeyRequestsResult result = GET_KEYREQUESTS_RESULT_INIT;
        selectDb(c,1);
        c->flags |= CLIENT_MULTI;
        rewriteResetClientCommandCString(c,4,"GTID","A:1","1","PING");
        queueMultiCommand(c);
        rewriteResetClientCommandCString(c,3,"MGET","KEY1","KEY2");
        queueMultiCommand(c);
        rewriteResetClientCommandCString(c,3,"LINDEX","LIST","3");
        queueMultiCommand(c);
        rewriteResetClientCommandCString(c,7,"GTID","A:2","2","/*COMMENT*/","MGET","KEY1","KEY2");
        queueMultiCommand(c);
        rewriteResetClientCommandCString(c,8,"GTID","A:3","3","HDEL","HASH","F1","F2","F3");
        queueMultiCommand(c);
        rewriteResetClientCommandCString(c,6,"GTID","A:4","4","LINDEX","LIST","3");
        queueMultiCommand(c);
        rewriteResetClientCommandCString(c,4,"GTID","A:5","5","FLUSHDB");
        queueMultiCommand(c);
        rewriteResetClientCommandCString(c,5,"GTID.AUTO","/*COMMENT*/","LINDEX","LIST","3");
        queueMultiCommand(c);
        rewriteResetClientCommandCString(c,4,"GTID","A:10","10","EXEC");

        getKeyRequests(c,&result);

        test_assert(result.num == 9);
        test_assert(c->db->id == 1);

        test_assert(!strcmp(result.key_requests[0].key->ptr, "KEY1"));
        test_assert(result.key_requests[0].b.subkeys == NULL);
        test_assert(result.key_requests[0].cmd_intention == SWAP_IN);
        test_assert(result.key_requests[0].cmd_intention_flags == 0);
        test_assert(result.key_requests[0].dbid == 10);
        test_assert(!strcmp(result.key_requests[1].key->ptr, "KEY2"));
        test_assert(result.key_requests[1].b.subkeys == NULL);
        test_assert(result.key_requests[1].cmd_intention == SWAP_IN);
        test_assert(result.key_requests[1].cmd_intention_flags == 0);
        test_assert(result.key_requests[1].dbid == 10);

        test_assert(!strcmp(result.key_requests[2].key->ptr, "LIST"));
        test_assert(result.key_requests[2].cmd_intention == SWAP_IN);
        test_assert(result.key_requests[2].cmd_intention_flags == 0);
        test_assert(result.key_requests[2].dbid == 10);
        test_assert(result.key_requests[2].l.num_ranges == 1);
        test_assert(result.key_requests[2].l.ranges[0].start == 3 && result.key_requests[2].l.ranges[0].end == 3);
        test_assert(result.key_requests[2].list_arg_rewrite[0].mstate_idx == 2);
        test_assert(result.key_requests[2].list_arg_rewrite[0].arg_idx == 2);

        test_assert(!strcmp(result.key_requests[3].key->ptr, "KEY1"));
        test_assert(result.key_requests[3].b.subkeys == NULL);
        test_assert(result.key_requests[3].cmd_intention == SWAP_IN);
        test_assert(result.key_requests[3].cmd_intention_flags == 0);
        test_assert(result.key_requests[3].dbid == 2);
        test_assert(!strcmp(result.key_requests[4].key->ptr, "KEY2"));
        test_assert(result.key_requests[4].b.subkeys == NULL);
        test_assert(result.key_requests[4].cmd_intention == SWAP_IN);
        test_assert(result.key_requests[4].cmd_intention_flags == 0);
        test_assert(result.key_requests[4].dbid == 2);

        test_assert(!strcmp(result.key_requests[5].key->ptr, "HASH"));
        test_assert(!strcmp(result.key_requests[5].b.subkeys[0]->ptr, "F1"));
        test_assert(!strcmp(result.key_requests[5].b.subkeys[1]->ptr, "F2"));
        test_assert(!strcmp(result.key_requests[5].b.subkeys[2]->ptr, "F3"));
        test_assert(result.key_requests[5].dbid == 3);

        test_assert(!strcmp(result.key_requests[6].key->ptr, "LIST"));
        test_assert(result.key_requests[6].cmd_intention == SWAP_IN);
        test_assert(result.key_requests[6].cmd_intention_flags == 0);
        test_assert(result.key_requests[6].dbid == 4);
        test_assert(result.key_requests[6].l.num_ranges == 1);
        test_assert(result.key_requests[6].l.ranges[0].start == 3 && result.key_requests[2].l.ranges[0].end == 3);
        test_assert(result.key_requests[6].list_arg_rewrite[0].mstate_idx == 5);
        test_assert(result.key_requests[6].list_arg_rewrite[0].arg_idx == 5);

        test_assert(result.key_requests[7].dbid == 5);
        test_assert(result.key_requests[7].level == REQUEST_LEVEL_SVR);
        test_assert(result.key_requests[7].key == NULL);

        test_assert(!strcmp(result.key_requests[8].key->ptr, "LIST"));
        test_assert(result.key_requests[8].cmd_intention == SWAP_IN);
        test_assert(result.key_requests[8].cmd_intention_flags == 0);
        test_assert(result.key_requests[8].dbid == 10);
        test_assert(result.key_requests[8].l.num_ranges == 1);
        test_assert(result.key_requests[8].l.ranges[0].start == 3 && result.key_requests[2].l.ranges[0].end == 3);
        test_assert(result.key_requests[8].list_arg_rewrite[0].mstate_idx == 7);
        test_assert(result.key_requests[8].list_arg_rewrite[0].arg_idx == 4);

        releaseKeyRequests(&result);
        getKeyRequestsFreeResult(&result);
        discardTransaction(c);
    }

    return error;
}

#endif

