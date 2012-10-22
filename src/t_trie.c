#include "redis.h"

static robj *trieTypeLookupWriteOrCreate(redisClient *c, robj *key) {
    robj *o = lookupKeyWrite(c->db, key);

    if (o == NULL) {
        o = createTrieObject();
        dbAdd(c->db,key,o);
    } else {
        if (o->type != REDIS_TRIE) {
            addReply(c,shared.wrongtypeerr);
            return NULL;
        }
    }
    return o;
}

static void addTrieFieldToReply(redisClient *c, robj *o, robj *field) {
    trieNode *node;
    robj *val;

    if (o == NULL) {
        addReply(c, shared.nullbulk);
        return;
    }

    node = trieFind(o->ptr, field->ptr, sdslen((sds)field->ptr));
    if (node == NULL) {
        addReply(c, shared.nullbulk);
        return ;
    }
    val = trieGetVal(node);
    addReplyBulk(c, val);
}

void tsetCommand(redisClient *c) {
    robj *o;
    int update = 0;
    const unsigned char *key = c->argv[2]->ptr;

    if ((o = trieTypeLookupWriteOrCreate(c, c->argv[1])) == NULL) return;

    c->argv[3] = tryObjectEncoding(c->argv[3]);
    incrRefCount(c->argv[3]);
    if (!trieAddOrReplace(o->ptr, key, sdslen((sds)key), c->argv[3],
        decrRefCount)) {
        update = 1;
    }
    addReply(c, update ? shared.czero : shared.cone);
    signalModifiedKey(c->db, c->argv[1]);
    server.dirty++;
}

void tsetnxCommand(redisClient *c) {
    robj *o;
    const unsigned char *key = c->argv[2]->ptr;

    if ((o = trieTypeLookupWriteOrCreate(c, c->argv[1])) == NULL) return;

    c->argv[3] = tryObjectEncoding(c->argv[3]);
    if (trieAdd(o->ptr, key, sdslen((sds)key), c->argv[3]) == TRIE_ERR) {
        addReply(c, shared.czero);
    } else {
        incrRefCount(c->argv[3]);
        addReply(c, shared.cone);
        signalModifiedKey(c->db, c->argv[1]);
        server.dirty++;
    }
}

void tmsetCommand(redisClient *c) {
    int i;
    robj *o;

    if ((c->argc % 2) == 1) {
        addReplyError(c,"wrong number of arguments for TMSET");
        return;
    }

    if ((o = trieTypeLookupWriteOrCreate(c, c->argv[1])) == NULL) return;
    for (i = 2; i < c->argc; i += 2) {
        const unsigned char *key = c->argv[i]->ptr;

        c->argv[i+1] = tryObjectEncoding(c->argv[i+1]);
        incrRefCount(c->argv[i+1]);
        trieAddOrReplace(o->ptr, key, sdslen((sds)key), c->argv[i+1],
            decrRefCount);
    }
    addReply(c, shared.ok);
    signalModifiedKey(c->db, c->argv[1]);
    server.dirty++;
}

void tgetCommand(redisClient *c) {
    robj *o;

    if ((o = lookupKeyReadOrReply(c, c->argv[1], shared.nullbulk)) == NULL ||
        checkType(c, o, REDIS_TRIE)) return;

    addTrieFieldToReply(c, o, c->argv[2]);
}

void tmgetCommand(redisClient *c) {
    robj *o;
    int i;

    /* Don't abort when the key cannot be found. Non-existing keys are empty
     * hashes, where TMGET should respond with a series of null bulks. */
    o = lookupKeyRead(c->db, c->argv[1]);
    if (o != NULL && o->type != REDIS_TRIE) {
        addReply(c, shared.wrongtypeerr);
        return;
    }

    addReplyMultiBulkLen(c, c->argc-2);
    for (i = 2; i < c->argc; i++) {
        addTrieFieldToReply(c, o, c->argv[i]);
    }
}

void tincrbyCommand(redisClient *c) {
    long long value, incr, oldvalue;
    robj *o, *new;
    const unsigned char *key = c->argv[2]->ptr;
    trieNode *node;

    if (getLongLongFromObjectOrReply(c,c->argv[3], &incr, NULL) != REDIS_OK) return;
    if ((o = trieTypeLookupWriteOrCreate(c, c->argv[1])) == NULL) return;

    if ((node = trieFind(o->ptr, key, sdslen((sds)key))) != NULL) {
        if (getLongLongFromObjectOrReply(c, trieGetVal(node), &value,
            "trie value is not an integer") != REDIS_OK) {
            return;
        }
    } else {
        value = 0;
    }

    oldvalue = value;
    if ((incr < 0 && oldvalue < 0 && incr < (LLONG_MIN-oldvalue)) ||
        (incr > 0 && oldvalue > 0 && incr > (LLONG_MAX-oldvalue))) {
        addReplyError(c,"increment or decrement would overflow");
        return;
    }
    value += incr;
    new = createStringObjectFromLongLong(value);
    if (node != NULL) {
        trieReplaceVal(node, new, decrRefCount);
    } else {
        trieAdd(o->ptr, key, sdslen((sds)key), new);
    }
    addReplyLongLong(c, value);
    signalModifiedKey(c->db, c->argv[1]);
    server.dirty++;
}

void tincrbyfloatCommand(redisClient *c) {
    double long value, incr;
    robj *o, *new, *aux;
    const unsigned char *key = c->argv[2]->ptr;
    trieNode *node;

    if (getLongDoubleFromObjectOrReply(c, c->argv[3], &incr, NULL) != REDIS_OK) return;
    if ((o = trieTypeLookupWriteOrCreate(c, c->argv[1])) == NULL) return;

    if ((node = trieFind(o->ptr, key, sdslen((sds)key))) != NULL) {
        if (getLongDoubleFromObjectOrReply(c, trieGetVal(node), &value,
            "hash value is not a valid float") != REDIS_OK) {
            return;
        }
    } else {
        value = 0;
    }

    value += incr;
    new = createStringObjectFromLongDouble(value);
    if (node != NULL) {
        trieReplaceVal(node, new, decrRefCount);
    } else {
        trieAdd(o->ptr, key, sdslen((sds)key), new);
    }
    addReplyBulk(c, new);
    signalModifiedKey(c->db, c->argv[1]);
    server.dirty++;

    /* Always replicate HINCRBYFLOAT as an HSET command with the final value
     * in order to make sure that differences in float pricision or formatting
     * will not create differences in replicas or after an AOF restart. */
    aux = createStringObject("TSET", 4);
    rewriteClientCommandArgument(c, 0, aux);
    rewriteClientCommandArgument(c, 3, new);
}

void tdelCommand(redisClient *c) {
    robj *o;
    int j, deleted = 0;

    if ((o = lookupKeyWriteOrReply(c, c->argv[1], shared.czero)) == NULL ||
        checkType(c, o, REDIS_TRIE)) return;

    for (j = 2; j < c->argc; j++) {
        if (trieDelete((trie*)o->ptr, c->argv[j]->ptr,
            sdslen((sds)c->argv[j]->ptr), decrRefCount) == TRIE_OK) {
            deleted++;
            if (trieSize((trie*)o->ptr) == 0) {
                dbDelete(c->db, c->argv[1]);
                break;
            }
        }
    }
    if (deleted) {
        signalModifiedKey(c->db, c->argv[1]);
        server.dirty += deleted;
    }
    addReplyLongLong(c, deleted);
}

void texistsCommand(redisClient *c) {
    robj *o;
    const unsigned char *key = c->argv[2]->ptr;
    trieNode *node;

    if ((o = lookupKeyReadOrReply(c, c->argv[1], shared.czero)) == NULL ||
        checkType(c, o, REDIS_TRIE)) return;

    node = trieFind(o->ptr, key, sdslen((sds)key));

    addReply(c, node != NULL ? shared.cone : shared.czero);
}

void tlenCommand(redisClient *c) {
    robj *o;
    if ((o = lookupKeyReadOrReply(c, c->argv[1], shared.czero)) == NULL ||
        checkType(c, o, REDIS_TRIE)) return;

    addReplyLongLong(c, trieSize((trie *)o->ptr));
}

/* Private structure used by tgetAllCallback() */
typedef struct tgetAllData {
    redisClient *c;
    int flags;
    int count;
} tgetAllData;

/* Callback for trieWalk() used by genericTgetallCommand() */
static int tgetAllCallback(trieNode *node, const unsigned char *key,
    size_t len, void *cbData)
{
    tgetAllData *data = (tgetAllData *)cbData;

    data->count++;
    if (data->flags & REDIS_HASH_KEY) {
        addReplyBulk(data->c, createStringObject((char *)key, len));
    }
    if (data->flags & REDIS_HASH_VALUE) {
        robj *val = trieGetVal(node);
        addReplyBulk(data->c, val);
    }
    return TRIE_OK;
}

static void genericTgetallCommand(redisClient *c, int flags) {
    robj *o;
    int multiplier = 0;
    int len = 0;
    tgetAllData cbData;
    void *replylen;

    if (c->argc > 3) {
        addReplyErrorFormat(c,"wrong number of arguments for '%s' command",
            c->cmd->name);
        return;
    }

    if ((o = lookupKeyReadOrReply(c, c->argv[1], shared.emptymultibulk)) == NULL
        || checkType(c, o, REDIS_TRIE)) return;

    if (flags & REDIS_HASH_KEY) multiplier++;
    if (flags & REDIS_HASH_VALUE) multiplier++;

    if (c->argc > 2) {
        replylen = addDeferredMultiBulkLength(c);
    } else {
        len = trieSize((trie*)o->ptr) * multiplier;
        addReplyMultiBulkLen(c, len);
    }

    cbData.c = c;
    cbData.flags = flags;
    cbData.count = 0;

    if (c->argc > 2) {
        const unsigned char *prefix = c->argv[2]->ptr;

        trieWalkFromPrefix(o->ptr, tgetAllCallback, &cbData, prefix,
            sdslen((sds)prefix));
        setDeferredMultiBulkLength(c, replylen, cbData.count * multiplier);
    } else {
        trieWalk(o->ptr, tgetAllCallback, &cbData);
    }
}

void tkeysCommand(redisClient *c) {
    genericTgetallCommand(c, REDIS_HASH_KEY);
}

void tvalsCommand(redisClient *c) {
    genericTgetallCommand(c, REDIS_HASH_VALUE);
}

void tgetallCommand(redisClient *c) {
    genericTgetallCommand(c, REDIS_HASH_KEY|REDIS_HASH_VALUE);
}
