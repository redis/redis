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

/* ----------------------------- swaps result ----------------------------- */
/* Prepare the getKeyRequestsResult struct to hold numswaps, either by using
 * the pre-allocated swaps or by allocating a new array on the heap.
 *
 * This function must be called at least once before starting to populate
 * the result, and can be called repeatedly to enlarge the result array.
 */
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

/* Note that key&subkeys ownership moved */
void getKeyRequestsAppendResult(getKeyRequestsResult *result, robj *key,
        int num_subkeys, robj **subkeys) {
    if (result->num == result->size) {
        int newsize = result->size + 
            (result->size > 8192 ? 8192 : result->size);
        getKeyRequestsPrepareResult(result, newsize);
    }

    keyRequest *key_request = &result->key_requests[result->num++];
    key_request->key = key;
    key_request->num_subkeys = num_subkeys;
    key_request->subkeys = subkeys;
}

void releaseKeyRequests(getKeyRequestsResult *result) {
    int i, j;
    for (i = 0; i < result->num; i++) {
        keyRequest *key_request = result->key_requests + i;
        if (key_request->key) decrRefCount(key_request->key);
        for (j = 0; j < key_request->num_subkeys; j++) {
            if (key_request->subkeys[j])
                decrRefCount(key_request->subkeys[j]);
        }
        if (key_request->subkeys) zfree(key_request->subkeys);
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
static void getSingleCmdKeyRequsts(client *c, getKeyRequestsResult *result) {
    struct redisCommand *cmd = c->cmd;

    if (cmd->getkeyrequests_proc == NULL) {
        int i, numkeys;
        getKeysResult keys = GETKEYS_RESULT_INIT;
        /* whole key swaping, swaps defined by command arity. */
        numkeys = getKeysFromCommand(cmd, c->argv, c->argc, &keys);
        getKeyRequestsPrepareResult(result, result->num+numkeys);
        for (i = 0; i < numkeys; i++) {
            robj *key = c->argv[keys.keys[i]];
            incrRefCount(key);
            getKeyRequestsAppendResult(result, key, 0, NULL);
        }
        getKeysFreeResult(&keys); 
    } else if (cmd->flags & CMD_MODULE) {
        // TODO support module
    } else {
        cmd->getkeyrequests_proc(cmd, c->argv, c->argc, result);
    }
}

void getKeyRequests(client *c, getKeyRequestsResult *result) {
    getKeyRequestsPrepareResult(result, MAX_KEYREQUESTS_BUFFER);

    if ((c->flags & CLIENT_MULTI) && 
            !(c->flags & (CLIENT_DIRTY_CAS | CLIENT_DIRTY_EXEC)) &&
            c->cmd->proc == execCommand) {
        /* if current is EXEC, we get swaps for all queue commands. */
        int i;
        robj **orig_argv;
        int orig_argc;
        struct redisCommand *orig_cmd;

        orig_argv = c->argv;
        orig_argc = c->argc;
        orig_cmd = c->cmd;
        for (i = 0; i < c->mstate.count; i++) {
            c->argc = c->mstate.commands[i].argc;
            c->argv = c->mstate.commands[i].argv;
            c->cmd = c->mstate.commands[i].cmd;
            getSingleCmdKeyRequsts(c, result);
        }
        c->argv = orig_argv;
        c->argc = orig_argc;
        c->cmd = orig_cmd;
    } else {
        getSingleCmdKeyRequsts(c, result);
    }
}

int getKeyRequestsNone(struct redisCommand *cmd, robj **argv, int argc,
        getKeyRequestsResult *result) {
    UNUSED(cmd);
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(result);
    return 0;
}

/* Used by flushdb/flushall to get global scs(similar to table lock). */
int getKeyRequestsGlobal(struct redisCommand *cmd, robj **argv, int argc,
        getKeyRequestsResult *result) {
    UNUSED(cmd);
    UNUSED(argc);
    UNUSED(argv);
    getKeyRequestsAppendResult(result, NULL, 0, NULL);
    return 0;
}

/* `rksdel` `rksget` are fake commands used only to provide flags for swap_ana,
 * use `touch` command to expire key actively instead. */
void rksdelCommand(client *c) {
    addReply(c, shared.ok);
}

void rksgetCommand(client *c) {
    addReply(c, shared.ok);
}

