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

/* look up on which scs client should block. client should block on uppper
 * level scs if it's not empty. */
swappingClients *lookupSwappingClients(client *c, robj *key, robj *subkey) {
    dictEntry *de;
    robj *e;
    moduleValue *mv;

    serverAssert(c);
    if (key == NULL) {
        /* There are three levels of scs: global > key > subkey. if key is NULL,
         * global scs is returned. */
        return server.scs;
    }

    serverAssert(sdsEncodedObject(key));
    serverAssert(subkey == NULL || sdsEncodedObject(subkey));

    de = dictFind(c->db->evict, key->ptr);
    if (de == NULL) {
        return NULL;
    }

    e = dictGetVal(de);
    switch (e->type) {
    case OBJ_MODULE:
        mv = e->ptr;
        return moduleLookupSwappingClients(mv, c, key, subkey);
    case OBJ_STRING:
    case OBJ_HASH:
        return lookupSwappingClientsWk(c->db, key);
    case OBJ_LIST:
    case OBJ_SET:
    case OBJ_ZSET:
    case OBJ_STREAM:
        return e->ptr;
    default:
        serverPanic("Unexpected encoding.");
        return NULL;
    }
}

void setupSwappingClients(client *c, robj *key, robj *subkey, swappingClients *scs) {
    dictEntry *de;
    robj *e;
    moduleValue *mv;

    /* No need to setup global scs again. */
    if (key == NULL) return;

    serverAssert(scs == NULL || scs->db == c->db);
    serverAssert(sdsEncodedObject(key));
    serverAssert(subkey == NULL || sdsEncodedObject(subkey));

    if ((de = dictFind(c->db->evict, key->ptr)) == NULL) {
        de = dictFind(c->db->dict, key->ptr);
    }

    if (de == NULL) {
        /* key deleted, no clients should try to block on this key. */
        serverAssert(scs == NULL);
        return;
    }

    e = dictGetVal(de);

    switch (e->type) {
    case OBJ_MODULE:
        mv = e->ptr;
        moduleSetupSwappingClients(mv, c, key, subkey, scs);
        break;
    case OBJ_STRING:
    case OBJ_HASH:
        setupSwappingClientsWk(c->db, key, scs);
        break;
    case OBJ_LIST:
    case OBJ_SET:
    case OBJ_ZSET:
    case OBJ_STREAM:
        e->ptr = scs;
        e->scs = 1;
        break;
    default:
        serverPanic("Unexpected encoding.");
        break;
    }
}

void getEvictionSwaps(client *c, robj *key, getSwapsResult *result) {
    dictEntry *de = dictFind(c->db->dict, key->ptr);
    robj *o = dictGetVal(de);
    moduleValue *mv = o->ptr;

    getSwapsPrepareResult(result, MAX_SWAPS_BUFFER);
    switch(o->type) {
    case OBJ_MODULE:
        moduleGetDataSwaps(mv, c, key, DATA_SWAPS_EVICTION, result);
        break;
    case OBJ_STRING:
    case OBJ_HASH:
        getDataSwapsWk(key, DATA_SWAPS_EVICTION, result);
        break;
    case OBJ_LIST:
    case OBJ_SET:
    case OBJ_ZSET:
    default:
        break;
    }
}

void getExpireSwaps(client *c, robj *key, getSwapsResult *result) {
    dictEntry *de;
    robj *val;

    getSwapsPrepareResult(result, MAX_SWAPS_BUFFER);

    if ((de = dictFind(c->db->dict, key->ptr)) == NULL) {
        de = dictFind(c->db->evict, key->ptr);
    }

    if (de == NULL) return;

    val = dictGetVal(de);
    moduleValue *mv = val->ptr;

    switch(val->type) {
    case OBJ_MODULE:
        moduleGetDataSwaps(mv, c, key, DATA_SWAPS_EXPIRE, result);
        break;
    case OBJ_STRING:
    case OBJ_HASH:
        getDataSwapsWk(key, DATA_SWAPS_EXPIRE, result);
        break;
    case OBJ_LIST:
    case OBJ_SET:
    case OBJ_ZSET:
    case OBJ_STREAM:
        result->numswaps = 0;
        break;
    default:
        serverPanic("Unexpected encoding.");
        break;
    }
}

#define SWAPCB_TYPE_NATIVE 0
#define SWAPCB_TYPE_MODULE 1

int swapAna(client *c, robj *key, robj *subkey, int *action, char **rawkey,
        char **rawval, int *cb_type, dataSwapFinishedCallback *cb, void **pd) {
    dictEntry *de;
    robj *val;
    moduleValue *mv;
    
    *action = SWAP_NOP;
    *rawkey = NULL;
    *rawval = NULL;
    *cb = NULL;
    *pd = NULL;

    if (key == NULL) return 0;

    if ((de = dictFind(c->db->dict, key->ptr)) == NULL &&
            (de = dictFind(c->db->evict, key->ptr)) == NULL) {
        /* key not exist, no need to swap. */
        return 0;
    }

    val = dictGetVal(de);
    serverAssert(val != NULL);

    switch(val->type) {
    case OBJ_MODULE:
        mv = val->ptr;
        *cb_type = SWAPCB_TYPE_MODULE;
        return moduleSwapAna(mv, c, key, subkey, action, rawkey, rawval, cb, pd);
    case OBJ_STRING:
    case OBJ_HASH:
        *cb_type = SWAPCB_TYPE_NATIVE;
        return swapAnaWk(c->cmd, c->db, key, action, rawkey, rawval, cb, pd);
    case OBJ_LIST:
    case OBJ_SET:
    case OBJ_ZSET:
    case OBJ_STREAM:
        return 0;
    default:
        serverPanic("Unexpected encoding.");
        break;
    }

    return 0;
}

void dataSwapFinished(client *c, int action, char *rawkey, char *rawval,
        int cb_type, dataSwapFinishedCallback cb, void *pd) {
    switch (cb_type) {
    case SWAPCB_TYPE_NATIVE:
        cb(c->db, action, rawkey, rawval, pd);
        break;
    case SWAPCB_TYPE_MODULE:
        moduleSwapFinished(c, action, rawkey, rawval, cb, pd);
        break;
    default:
        serverPanic("Unexpected swap cb type.");
        break;
    }
}

/* NOTE that result.{key,subkey} are ONLY REFS to client argv (since client
 * outlives getKeysResult if no swap action happend. key, subkey will be 
 * copied (using incrRefCount) when async swap acutally proceed. */
static void getSingleCmdSwaps(client *c, getSwapsResult *result) {
    struct redisCommand *cmd = c->cmd;

    if (cmd->getswaps_proc == NULL) {
        int i, numkeys;
        getKeysResult keys = GETKEYS_RESULT_INIT;
        /* whole key swaping, swaps defined by command arity. */
        numkeys = getKeysFromCommand(cmd, c->argv, c->argc, &keys);
        getSwapsPrepareResult(result, result->numswaps+numkeys);
        for (i = 0; i < numkeys; i++) {
            robj *key = c->argv[keys.keys[i]];
            incrRefCount(key);
            getSwapsAppendResult(result, key, NULL, NULL);
        }
        getKeysFreeResult(&keys); 
    } else if (cmd->flags & CMD_MODULE) {
        moduleGetCommandSwaps(cmd, c->argv, c->argc, result);
    } else {
        cmd->getswaps_proc(cmd, c->argv, c->argc, result);
    }
}

void getSwaps(client *c, getSwapsResult *result) {
    getSwapsPrepareResult(result, MAX_SWAPS_BUFFER);

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
            getSingleCmdSwaps(c, result);
        }
        c->argv = orig_argv;
        c->argc = orig_argc;
        c->cmd = orig_cmd;
    } else {
        getSingleCmdSwaps(c, result);
    }
}

/* Note value, evict are MOVEed into cv */
compVal *compValNew(int type, void *cvv, robj *cve) {
    compVal *cv = zmalloc(sizeof(compVal));

    serverAssert((type == COMP_TYPE_OBJ && cvv != NULL && cve == NULL) ||
            (type == COMP_TYPE_RAW && cvv == NULL && cve != NULL));

    cv->type = type;
    cv->value = cvv;
    cv->evict = cve;

    return cv;
}

void compValFree(compVal *cv) {
    if (!cv) return;

    switch (cv->type) {
    case COMP_TYPE_RAW:
        sdsfree(cv->value);
        break;
    case COMP_TYPE_OBJ:
        decrRefCount(cv->value);
        break;
    default:
        break;
    }
    zfree(cv);
}

compVal *getComplementSwaps(redisDb *db, robj *key, int mode, getSwapsResult *result, complementObjectFunc *comp, void **pd) {
    void *comp_val;
    int comp_type;
    robj *dup = NULL, *cve, *cvv;
    moduleValue *mv;
    compVal *cv = NULL;
    client *c = server.dummy_clients[db->id];
    robj *o = lookupKey(db, key, LOOKUP_NOTOUCH);
    robj *e = lookupEvict(db, key);

    getSwapsPrepareResult(result, MAX_SWAPS_BUFFER);

    if (e == NULL) return NULL;

    switch (e->type) {
    case OBJ_MODULE:
        mv = e->ptr;
        comp_val = moduleGetComplementSwaps(mv, c, key, mode, &comp_type, result, comp, pd);
        if (comp_type == COMP_TYPE_OBJ) {
            dup = createModuleObject(mv->type, comp_val);
            cvv = dup, cve = NULL;
        } else {
            cvv = comp_val, cve = e;
        }
        cv = compValNew(comp_type, cvv, cve);
        break;
    case OBJ_STRING:
    case OBJ_HASH:
        comp_val = getComplementSwapsWk(db, key, mode, &comp_type, result, comp, pd);
        serverAssert(comp_type == COMP_TYPE_RAW);
        cv = compValNew(comp_type, comp_val, e);
        break;
    case OBJ_LIST:
    case OBJ_SET:
    case OBJ_ZSET:
    case OBJ_STREAM:
        break;
    }

    if (o && dup) dup->lru = o->lru; /* also reserve the right LFU from db.dict */
    return cv;
}

int complementRaw(void **raw, sds rawkey, sds rawval, complementObjectFunc comp, void *pd) {
	serverAssert(*raw == NULL);
	return comp(raw, rawkey, rawval, pd);
}

int complementObj(robj *dup, sds rawkey, sds rawval, complementObjectFunc comp, void *pd) {
    moduleValue *mv;
    switch (dup->type) {
    case OBJ_MODULE:
        mv = dup->ptr;
        return comp(&mv->value, rawkey, rawval, pd);
    case OBJ_STRING:
    case OBJ_LIST:
    case OBJ_HASH:
    case OBJ_SET:
    case OBJ_ZSET:
    case OBJ_STREAM:
        return comp(&dup->ptr, rawkey, rawval, pd);
    }
    return 0;
}

int complementCompVal(compVal *cv, sds rawkey, sds rawval, complementObjectFunc comp, void *pd) {
    switch (cv->type) {
    case COMP_TYPE_OBJ:
        return complementObj(cv->value, rawkey, rawval, comp, pd);
    case COMP_TYPE_RAW:
        return complementRaw(&cv->value, rawkey, rawval, comp, pd); 
    }
    return 0;
}

