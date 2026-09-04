/*
 * memcached.c -- memcached text protocol support.
 *
 * Overview
 * --------
 * When `memcached-port` is non-zero we open a second TCP listener. Clients
 * accepted on it are flagged CLIENT_MEMCACHED, pinned to database 0, and their
 * input is parsed here instead of by the RESP parser in networking.c. The
 * keyspace is shared: an item stored with `set` is an ordinary Redis string
 * and can be read back with GET, and the other way round.
 *
 * Commands are not routed through the Redis command table. memcached's
 * semantics differ from Redis' in enough places (unsigned wrapping incr, decr
 * clamped at zero, append/prepend that refuse to create a key, expiry values
 * that flip from relative to absolute at 30 days) that expressing them as
 * translations of Redis commands would be more code, not less. Instead the
 * handlers below work on the keyspace directly and then explicitly do what
 * call() would have done for them: bump server.dirty, fire keyspace
 * notifications, signal WATCH and client-side caching, and propagate an
 * equivalent *Redis* command to replicas and the AOF through alsoPropagate().
 * Each command runs inside an execution unit so propagation is batched exactly
 * as it is for a normal command. This is the same pattern the active expire
 * cycle uses to delete keys from outside call().
 *
 * Deliberate limitations, all of them repeated in redis.conf:
 *
 * - The 32 bit item flags live in a side table (redisDb.mcflags) keyed by key
 *   name. They are NOT stored in the RDB, NOT written to the AOF and NOT
 *   replicated. A replica, or a server that reloaded its dataset, serves the
 *   values but reports flags of 0. This follows DragonflyDB, which likewise
 *   keeps mc_flags in a table beside the main one rather than in the value.
 * - There is no cas. `gets`/`gats` answer with a cas token of 0 so that
 *   clients which always issue `gets` keep working, and `cas` itself is
 *   rejected.
 * - No binary protocol, no meta protocol, no UDP.
 * - Standalone only; the server refuses to start with both memcached-port and
 *   cluster-enabled set.
 * - No authentication on the memcached port, which is why the server also
 *   refuses to start with both the port and requirepass/ACLs configured
 *   unless `memcached-insecure-allow-noauth yes` says that is intended.
 */

#include "server.h"

/* ------------------------------------------------------------------------
 * Fixed replies
 * ------------------------------------------------------------------------ */

#define MC_STORED           "STORED\r\n"
#define MC_NOT_STORED       "NOT_STORED\r\n"
#define MC_DELETED          "DELETED\r\n"
#define MC_NOT_FOUND        "NOT_FOUND\r\n"
#define MC_TOUCHED          "TOUCHED\r\n"
#define MC_END              "END\r\n"
#define MC_OK               "OK\r\n"
#define MC_RESET            "RESET\r\n"
#define MC_ERROR            "ERROR\r\n"

#define MC_ERR_BADFMT       "CLIENT_ERROR bad command line format\r\n"
#define MC_ERR_BADCHUNK     "CLIENT_ERROR bad data chunk\r\n"
#define MC_ERR_BADDELTA     "CLIENT_ERROR invalid numeric delta argument\r\n"
#define MC_ERR_NONNUMERIC   "CLIENT_ERROR cannot increment or decrement non-numeric value\r\n"
#define MC_ERR_NOCAS        "CLIENT_ERROR cas is not supported\r\n"
#define MC_ERR_NODELAY      "CLIENT_ERROR delayed flush_all is not supported\r\n"
#define MC_ERR_TOOLARGE     "SERVER_ERROR object too large for cache\r\n"
#define MC_ERR_OOM          "SERVER_ERROR out of memory storing object\r\n"
#define MC_ERR_READONLY     "SERVER_ERROR read only replica\r\n"
#define MC_ERR_LOADING      "SERVER_ERROR loading the dataset in memory\r\n"
#define MC_ERR_WRONGTYPE    "SERVER_ERROR key holds a value that is not a string\r\n"

/* Version reported by `version`. Clients gate features on this string, so we
 * report a plain memcached version number; the real server version is
 * available as `STAT version` in the `stats` output. */
#define MC_ADVERTISED_VERSION "1.6.0"

/* ------------------------------------------------------------------------
 * Tokenizing
 * ------------------------------------------------------------------------ */

#define MC_TOKENS_STACK 8

typedef struct mcToken {
    const char *ptr;
    size_t len;
} mcToken;

typedef struct mcTokens {
    mcToken *v;
    int count;
    int cap;
    mcToken stack[MC_TOKENS_STACK];
} mcTokens;

static void mcTokensInit(mcTokens *t) {
    t->v = t->stack;
    t->count = 0;
    t->cap = MC_TOKENS_STACK;
}

static void mcTokensFree(mcTokens *t) {
    if (t->v != t->stack) zfree(t->v);
}

static void mcTokensPush(mcTokens *t, const char *p, size_t len) {
    if (t->count == t->cap) {
        int newcap = t->cap * 2;
        if (t->v == t->stack) {
            t->v = zmalloc(sizeof(mcToken) * newcap);
            memcpy(t->v, t->stack, sizeof(mcToken) * t->cap);
        } else {
            t->v = zrealloc(t->v, sizeof(mcToken) * newcap);
        }
        t->cap = newcap;
    }
    t->v[t->count].ptr = p;
    t->v[t->count].len = len;
    t->count++;
}

/* Split a command line on runs of spaces. Collapsing runs is more forgiving
 * than memcached's tokenizer and never changes the meaning of a well formed
 * command. */
static void mcTokenize(const char *line, size_t len, mcTokens *t) {
    size_t i = 0;
    mcTokensInit(t);
    while (i < len) {
        while (i < len && line[i] == ' ') i++;
        if (i == len) break;
        size_t start = i;
        while (i < len && line[i] != ' ') i++;
        mcTokensPush(t, line + start, i - start);
    }
}

static int mcTokEq(const mcToken *t, const char *s) {
    size_t len = strlen(s);
    return t->len == len && memcmp(t->ptr, s, len) == 0;
}

/* ------------------------------------------------------------------------
 * Number parsing
 *
 * Strict on purpose: memcached rejects "12x" and " 12" with a client error
 * rather than silently taking the leading digits.
 * ------------------------------------------------------------------------ */

static int mcTokU64(const mcToken *t, uint64_t *out) {
    if (t->len == 0 || t->len > 20) return 0;
    uint64_t v = 0;
    for (size_t i = 0; i < t->len; i++) {
        char ch = t->ptr[i];
        if (ch < '0' || ch > '9') return 0;
        uint64_t digit = (uint64_t)(ch - '0');
        if (v > (UINT64_MAX - digit) / 10) return 0; /* overflow */
        v = v * 10 + digit;
    }
    *out = v;
    return 1;
}

static int mcTokU32(const mcToken *t, uint32_t *out) {
    uint64_t v;
    if (!mcTokU64(t, &v) || v > UINT32_MAX) return 0;
    *out = (uint32_t)v;
    return 1;
}

static int mcTokI64(const mcToken *t, int64_t *out) {
    mcToken body = *t;
    int negative = 0;
    if (body.len > 0 && (body.ptr[0] == '-' || body.ptr[0] == '+')) {
        negative = body.ptr[0] == '-';
        body.ptr++;
        body.len--;
    }
    uint64_t v;
    if (!mcTokU64(&body, &v)) return 0;
    if (negative) {
        if (v > (uint64_t)INT64_MAX + 1) return 0;
        *out = (v == (uint64_t)INT64_MAX + 1) ? INT64_MIN : -(int64_t)v;
    } else {
        if (v > (uint64_t)INT64_MAX) return 0;
        *out = (int64_t)v;
    }
    return 1;
}

/* Parse a stored value the way memcached's incr/decr does: an unsigned decimal
 * number, optionally followed by spaces. The trailing spaces matter because
 * memcached is allowed to leave a decremented item space padded, so items
 * written by a real memcached may come back looking like "7  ". */
static int mcParseStoredNumber(const char *s, size_t len, uint64_t *out) {
    size_t i = 0;
    uint64_t v = 0;
    int digits = 0;

    while (i < len && s[i] >= '0' && s[i] <= '9') {
        uint64_t digit = (uint64_t)(s[i] - '0');
        if (v > (UINT64_MAX - digit) / 10) return 0;
        v = v * 10 + digit;
        digits++;
        i++;
    }
    if (!digits) return 0;
    while (i < len && s[i] == ' ') i++;
    if (i != len) return 0;
    *out = v;
    return 1;
}

/* ------------------------------------------------------------------------
 * Keys and expiry
 * ------------------------------------------------------------------------ */

static int mcKeyValid(const mcToken *t) {
    if (t->len == 0 || t->len > MEMCACHED_MAX_KEY_LEN) return 0;
    for (size_t i = 0; i < t->len; i++) {
        unsigned char ch = (unsigned char)t->ptr[i];
        if (ch <= 32 || ch == 127) return 0;
    }
    return 1;
}

#define MC_EXPIRE_NONE 0 /* Item never expires. */
#define MC_EXPIRE_PAST 1 /* Item is already expired. */
#define MC_EXPIRE_AT   2 /* *when_ms holds an absolute ms timestamp. */

/* Convert a memcached exptime into one of the three cases above.
 *
 * memcached's rules: 0 means "never"; a negative value means "immediately
 * expired"; a value at or below 30 days is a delta from now; anything larger
 * is an absolute unix timestamp. */
static int mcExpireResolve(int64_t exptime, long long *when_ms) {
    if (exptime == 0) return MC_EXPIRE_NONE;
    if (exptime < 0) return MC_EXPIRE_PAST;

    long long when;
    if (exptime <= MEMCACHED_REALTIME_MAXDELTA) {
        when = commandTimeSnapshot() + exptime * 1000LL;
    } else {
        when = exptime * 1000LL;
        if (when <= commandTimeSnapshot()) return MC_EXPIRE_PAST;
    }
    *when_ms = when;
    return MC_EXPIRE_AT;
}

/* ------------------------------------------------------------------------
 * Item flags side table
 *
 * A dict of key name -> uint32 flags hanging off the database, so that it
 * follows the keyspace through SWAPDB. db.c keeps it in step with the
 * keyspace: entries are dropped when a key is deleted or overwritten, and the
 * whole table is emptied on FLUSHDB/FLUSHALL. The dict is created on demand,
 * so a server that never sees a memcached item pays nothing for it.
 * ------------------------------------------------------------------------ */

static dictType mcFlagsDictType = {
    dictSdsHash,           /* hash function */
    NULL,                  /* key dup */
    NULL,                  /* val dup */
    dictSdsKeyCompare,     /* key compare */
    dictSdsDestructor,     /* key destructor */
    NULL,                  /* val destructor, values are inline integers */
    NULL,                  /* allow to resize */
};

void memcachedFlagsFree(redisDb *db) {
    if (db->mcflags == NULL) return;
    dictRelease(db->mcflags);
    db->mcflags = NULL;
}

void memcachedFlagsEmpty(redisDb *db) {
    if (db->mcflags == NULL) return;
    dictEmpty(db->mcflags, NULL);
}

void memcachedFlagsRemove(redisDb *db, robj *key) {
    if (db->mcflags == NULL || dictSize(db->mcflags) == 0) return;
    dictDelete(db->mcflags, key->ptr);
}

static uint32_t mcFlagsGet(redisDb *db, robj *key) {
    if (db->mcflags == NULL) return 0;
    dictEntry *de = dictFind(db->mcflags, key->ptr);
    return de ? (uint32_t)dictGetUnsignedIntegerVal(de) : 0;
}

static void mcFlagsSet(redisDb *db, robj *key, uint32_t flags) {
    if (flags == 0) {
        /* Zero is the default, so there is nothing to remember. We still drop
         * any existing entry, since a key can be re-stored with new flags. */
        memcachedFlagsRemove(db, key);
        return;
    }
    if (db->mcflags == NULL) db->mcflags = dictCreate(&mcFlagsDictType);

    dictEntry *de = dictFind(db->mcflags, key->ptr);
    if (de == NULL) de = dictAddRaw(db->mcflags, sdsdup(key->ptr), NULL);
    dictSetUnsignedIntegerVal(de, flags);
}

/* ------------------------------------------------------------------------
 * Replies
 * ------------------------------------------------------------------------ */

/* Everything a single memcached command needs while it runs. */
typedef struct mcRequest {
    client *c;
    mcTokens *tokens;
    int noreply;
} mcRequest;

static void mcOut(mcRequest *r, const char *s) {
    if (r->noreply) return;
    addReplyProto(r->c, s, strlen(s));
}

static void mcOutSds(mcRequest *r, sds s) {
    if (r->noreply) {
        sdsfree(s);
        return;
    }
    addReplySds(r->c, s);
}

/* Strip a trailing "noreply" token and record it on the request. Once set,
 * every reply from this command is suppressed, errors included, which is what
 * memcached does. */
static void mcTakeNoreply(mcRequest *r) {
    mcTokens *t = r->tokens;
    if (t->count > 1 && mcTokEq(&t->v[t->count - 1], "noreply")) {
        r->noreply = 1;
        t->count--;
    }
}

/* ------------------------------------------------------------------------
 * Guards shared by the mutating commands
 * ------------------------------------------------------------------------ */

/* Returns 1 if the command may proceed, otherwise emits the memcached error
 * that best matches the condition and returns 0. */
static int mcWriteAllowed(mcRequest *r) {
    if (server.loading) {
        mcOut(r, MC_ERR_LOADING);
        return 0;
    }
    if (server.masterhost && server.repl_slave_ro) {
        mcOut(r, MC_ERR_READONLY);
        return 0;
    }
    if (server.maxmemory && performEvictions() == EVICT_FAIL) {
        mcOut(r, MC_ERR_OOM);
        return 0;
    }
    return 1;
}

static int mcReadAllowed(mcRequest *r) {
    if (server.loading) {
        mcOut(r, MC_ERR_LOADING);
        return 0;
    }
    return 1;
}

/* memcached has no notion of types. Rather than assert, or silently treat a
 * list as a miss, we surface the mismatch as a server error. */
static int mcIsString(mcRequest *r, kvobj *o) {
    if (o == NULL || o->type == OBJ_STRING) return 1;
    mcOut(r, MC_ERR_WRONGTYPE);
    return 0;
}

/* ------------------------------------------------------------------------
 * Keyspace helpers
 *
 * Each of these does the full set of things a Redis command implementation
 * would do, since we are not going through call().
 * ------------------------------------------------------------------------ */

/* Install `val` at `key` with the given expiry and item flags, notify, and
 * propagate the equivalent SET.
 *
 * Takes ownership of one reference to `val`: like setKey(), the caller must
 * not decrRefCount() it afterwards. */
static void mcStoreItem(client *c, robj *key, robj *val, uint32_t flags,
                        int expire_kind, long long when_ms, const char *event)
{
    robj *stored = val;
    setKey(c, c->db, key, &stored,
           expire_kind == MC_EXPIRE_AT ? SETKEY_KEEPTTL : 0);

    if (expire_kind == MC_EXPIRE_AT)
        stored = setExpire(c, c->db, key, when_ms);

    mcFlagsSet(c->db, key, flags);

    server.dirty++;
    notifyKeyspaceEvent(NOTIFY_STRING, event, key, c->db->id);
    if (expire_kind == MC_EXPIRE_AT)
        notifyKeyspaceEvent(NOTIFY_GENERIC, "expire", key, c->db->id);

    /* Replicas and the AOF see a plain SET. Item flags are not part of it,
     * which is exactly why they do not survive replication or a reload. */
    if (expire_kind == MC_EXPIRE_AT) {
        robj *when_obj = createStringObjectFromLongLong(when_ms);
        robj *argv[5] = {shared.set, key, stored, shared.pxat, when_obj};
        alsoPropagate(c->db->id, argv, 5, PROPAGATE_AOF | PROPAGATE_REPL);
        decrRefCount(when_obj);
    } else {
        robj *argv[3] = {shared.set, key, stored};
        alsoPropagate(c->db->id, argv, 3, PROPAGATE_AOF | PROPAGATE_REPL);
    }
}

/* Delete `key` if present, notify, and propagate a DEL. Returns 1 if a key
 * was actually removed. */
static int mcDeleteItem(client *c, robj *key) {
    int deleted = server.lazyfree_lazy_user_del ? dbAsyncDelete(c->db, key)
                                                : dbSyncDelete(c->db, key);
    if (!deleted) return 0;

    keyModified(c, c->db, key, NULL, 1);
    notifyKeyspaceEvent(NOTIFY_GENERIC, "del", key, c->db->id);
    server.dirty++;

    robj *argv[2] = {shared.del, key};
    alsoPropagate(c->db->id, argv, 2, PROPAGATE_AOF | PROPAGATE_REPL);
    return 1;
}

/* Apply a resolved expiry to a key that is known to exist, as `touch`, `gat`
 * and `gats` do. A past expiry deletes the item, and an expiry of "none"
 * clears any TTL the key had. */
static void mcApplyExpire(client *c, robj *key, int expire_kind,
                          long long when_ms)
{
    if (expire_kind == MC_EXPIRE_PAST) {
        mcDeleteItem(c, key);
        return;
    }

    if (expire_kind == MC_EXPIRE_AT) {
        setExpire(c, c->db, key, when_ms);
        server.dirty++;
        keyModified(c, c->db, key, NULL, 1);
        notifyKeyspaceEvent(NOTIFY_GENERIC, "expire", key, c->db->id);

        robj *when_obj = createStringObjectFromLongLong(when_ms);
        robj *argv[3] = {shared.pexpireat, key, when_obj};
        alsoPropagate(c->db->id, argv, 3, PROPAGATE_AOF | PROPAGATE_REPL);
        decrRefCount(when_obj);
        return;
    }

    if (removeExpire(c->db, key)) {
        server.dirty++;
        keyModified(c, c->db, key, NULL, 1);
        notifyKeyspaceEvent(NOTIFY_GENERIC, "persist", key, c->db->id);

        robj *argv[2] = {shared.persist, key};
        alsoPropagate(c->db->id, argv, 2, PROPAGATE_AOF | PROPAGATE_REPL);
    }
}

/* ------------------------------------------------------------------------
 * set / add / replace / append / prepend
 * ------------------------------------------------------------------------ */

#define MC_STORE_SET     0
#define MC_STORE_ADD     1
#define MC_STORE_REPLACE 2
#define MC_STORE_APPEND  3
#define MC_STORE_PREPEND 4

/* `key` and `value` are copies owned by this function. */
static void mcStoreCommand(mcRequest *r, int mode, robj *key, robj *value,
                           uint32_t flags, int64_t exptime)
{
    client *c = r->c;

    server.mc_stats.cmd_set++;

    if (!mcWriteAllowed(r)) goto cleanup;

    long long when_ms = 0;
    int expire_kind = mcExpireResolve(exptime, &when_ms);

    kvobj *old = lookupKeyWrite(c->db, key);
    if (!mcIsString(r, old)) goto cleanup;

    if (mode == MC_STORE_ADD && old != NULL) {
        mcOut(r, MC_NOT_STORED);
        goto cleanup;
    }
    if (old == NULL && (mode == MC_STORE_REPLACE || mode == MC_STORE_APPEND ||
                        mode == MC_STORE_PREPEND))
    {
        mcOut(r, MC_NOT_STORED);
        goto cleanup;
    }

    if (mode == MC_STORE_APPEND || mode == MC_STORE_PREPEND) {
        /* memcached ignores the flags and exptime sent with append/prepend:
         * the existing item keeps both of its own. */
        robj *decoded = getDecodedObject(old);
        size_t oldlen = sdslen(decoded->ptr);
        size_t addlen = sdslen(value->ptr);

        if (oldlen + addlen > MEMCACHED_MAX_ITEM_SIZE) {
            decrRefCount(decoded);
            mcOut(r, MC_ERR_TOOLARGE);
            goto cleanup;
        }

        sds merged = sdsnewlen(NULL, 0);
        merged = sdsMakeRoomFor(merged, oldlen + addlen);
        if (mode == MC_STORE_APPEND) {
            merged = sdscatlen(merged, decoded->ptr, oldlen);
            merged = sdscatlen(merged, value->ptr, addlen);
        } else {
            merged = sdscatlen(merged, value->ptr, addlen);
            merged = sdscatlen(merged, decoded->ptr, oldlen);
        }
        decrRefCount(decoded);

        uint32_t kept_flags = mcFlagsGet(c->db, key);
        long long kept_ttl = getExpire(c->db, key->ptr, old);
        robj *newval = tryObjectEncoding(createObject(OBJ_STRING, merged));

        mcStoreItem(c, key, newval, kept_flags,
                    kept_ttl == -1 ? MC_EXPIRE_NONE : MC_EXPIRE_AT, kept_ttl,
                    mode == MC_STORE_APPEND ? "append" : "prepend");
        server.mc_stats.total_items++;
        mcOut(r, MC_STORED);
        goto cleanup;
    }

    if (expire_kind == MC_EXPIRE_PAST) {
        /* memcached accepts the store and the item is immediately invalid, so
         * what an observer sees is simply the key not being there. */
        mcDeleteItem(c, key);
        memcachedFlagsRemove(c->db, key);
        server.stat_expiredkeys++;
        server.mc_stats.total_items++;
        mcOut(r, MC_STORED);
        goto cleanup;
    }

    /* tryObjectEncoding takes ownership and returns an object we own, and
     * mcStoreItem takes that ownership over in turn. */
    mcStoreItem(c, key, tryObjectEncoding(value), flags, expire_kind, when_ms,
                "set");
    value = NULL;
    server.mc_stats.total_items++;
    mcOut(r, MC_STORED);

cleanup:
    if (value) decrRefCount(value);
    decrRefCount(key);
}

/* ------------------------------------------------------------------------
 * get / gets / gat / gats
 * ------------------------------------------------------------------------ */

/* `first_key` is the index of the first key token. When `touch_exptime` is
 * non-NULL this is a gat/gats and every returned item is re-expired. */
static void mcGetCommand(mcRequest *r, int first_key, int with_cas,
                         const int64_t *touch_exptime)
{
    client *c = r->c;
    mcTokens *t = r->tokens;

    if (first_key >= t->count) {
        mcOut(r, MC_ERR_BADFMT);
        return;
    }
    if (!mcReadAllowed(r)) return;

    long long when_ms = 0;
    int expire_kind = MC_EXPIRE_NONE;
    if (touch_exptime) {
        if (!mcWriteAllowed(r)) return;
        expire_kind = mcExpireResolve(*touch_exptime, &when_ms);
    }

    for (int i = first_key; i < t->count; i++) {
        if (!mcKeyValid(&t->v[i])) {
            mcOut(r, MC_ERR_BADFMT);
            return;
        }

        robj *key = createStringObject(t->v[i].ptr, t->v[i].len);
        server.mc_stats.cmd_get++;
        if (touch_exptime) server.mc_stats.cmd_touch++;

        kvobj *o = touch_exptime ? lookupKeyWrite(c->db, key)
                                 : lookupKeyRead(c->db, key);
        if (o == NULL) {
            server.mc_stats.get_misses++;
            if (touch_exptime) server.mc_stats.touch_misses++;
            decrRefCount(key);
            continue;
        }
        if (o->type != OBJ_STRING) {
            decrRefCount(key);
            /* A SERVER_ERROR terminates the response; we do not send END. */
            mcOut(r, MC_ERR_WRONGTYPE);
            return;
        }

        server.mc_stats.get_hits++;
        if (touch_exptime) server.mc_stats.touch_hits++;

        robj *decoded = getDecodedObject(o);
        size_t len = sdslen(decoded->ptr);
        uint32_t flags = mcFlagsGet(c->db, key);

        /* We have no cas support, so every item reports a token of 0. */
        sds header = sdscatprintf(sdsempty(),
                                  with_cas ? "VALUE %.*s %u %zu 0\r\n"
                                           : "VALUE %.*s %u %zu\r\n",
                                  (int)t->v[i].len, t->v[i].ptr, flags, len);
        addReplySds(c, header);
        addReplyProto(c, decoded->ptr, len);
        addReplyProto(c, "\r\n", 2);
        decrRefCount(decoded);

        /* gat/gats apply the new expiry after the value has been queued. */
        if (touch_exptime) mcApplyExpire(c, key, expire_kind, when_ms);

        decrRefCount(key);
    }

    mcOut(r, MC_END);
}

/* ------------------------------------------------------------------------
 * delete / touch
 * ------------------------------------------------------------------------ */

static void mcDeleteCommand(mcRequest *r) {
    client *c = r->c;
    mcTokens *t = r->tokens;

    /* memcached once accepted "delete <key> <time>". The time is long gone,
     * but clients in the wild still send a literal 0. */
    if (t->count < 2 || t->count > 3) {
        mcOut(r, MC_ERR_BADFMT);
        return;
    }
    if (t->count == 3) {
        int64_t legacy;
        if (!mcTokI64(&t->v[2], &legacy) || legacy != 0) {
            mcOut(r, "CLIENT_ERROR bad command line format."
                     "  Usage: delete <key> [noreply]\r\n");
            return;
        }
    }
    if (!mcKeyValid(&t->v[1])) {
        mcOut(r, MC_ERR_BADFMT);
        return;
    }
    if (!mcWriteAllowed(r)) return;

    robj *key = createStringObject(t->v[1].ptr, t->v[1].len);
    kvobj *o = lookupKeyWrite(c->db, key);
    if (o == NULL) {
        server.mc_stats.delete_misses++;
        mcOut(r, MC_NOT_FOUND);
    } else if (!mcIsString(r, o)) {
        /* Deliberately refuse rather than silently dropping a collection a
         * Redis client owns. */
    } else {
        mcDeleteItem(c, key);
        server.mc_stats.delete_hits++;
        mcOut(r, MC_DELETED);
    }
    decrRefCount(key);
}

static void mcTouchCommand(mcRequest *r) {
    client *c = r->c;
    mcTokens *t = r->tokens;

    int64_t exptime;
    if (t->count != 3 || !mcKeyValid(&t->v[1]) ||
        !mcTokI64(&t->v[2], &exptime))
    {
        mcOut(r, MC_ERR_BADFMT);
        return;
    }
    if (!mcWriteAllowed(r)) return;

    server.mc_stats.cmd_touch++;

    robj *key = createStringObject(t->v[1].ptr, t->v[1].len);
    kvobj *o = lookupKeyWrite(c->db, key);
    if (o == NULL) {
        server.mc_stats.touch_misses++;
        mcOut(r, MC_NOT_FOUND);
        decrRefCount(key);
        return;
    }
    if (!mcIsString(r, o)) {
        decrRefCount(key);
        return;
    }

    long long when_ms = 0;
    int expire_kind = mcExpireResolve(exptime, &when_ms);
    mcApplyExpire(c, key, expire_kind, when_ms);

    server.mc_stats.touch_hits++;
    mcOut(r, MC_TOUCHED);
    decrRefCount(key);
}

/* ------------------------------------------------------------------------
 * incr / decr
 *
 * This is memcached's arithmetic, not Redis'. The value is an unsigned 64 bit
 * integer: incr wraps on overflow, decr saturates at zero instead of going
 * negative, and a missing key is NOT_FOUND rather than being created.
 * ------------------------------------------------------------------------ */

static void mcIncrDecrCommand(mcRequest *r, int incr) {
    client *c = r->c;
    mcTokens *t = r->tokens;

    if (t->count != 3 || !mcKeyValid(&t->v[1])) {
        mcOut(r, MC_ERR_BADFMT);
        return;
    }
    uint64_t delta;
    if (!mcTokU64(&t->v[2], &delta)) {
        mcOut(r, MC_ERR_BADDELTA);
        return;
    }
    if (!mcWriteAllowed(r)) return;

    robj *key = createStringObject(t->v[1].ptr, t->v[1].len);
    kvobj *o = lookupKeyWrite(c->db, key);
    if (o == NULL) {
        if (incr) server.mc_stats.incr_misses++;
        else server.mc_stats.decr_misses++;
        mcOut(r, MC_NOT_FOUND);
        decrRefCount(key);
        return;
    }
    if (!mcIsString(r, o)) {
        decrRefCount(key);
        return;
    }

    robj *decoded = getDecodedObject(o);
    uint64_t current;
    int numeric = mcParseStoredNumber(decoded->ptr, sdslen(decoded->ptr),
                                      &current);
    decrRefCount(decoded);
    if (!numeric) {
        mcOut(r, MC_ERR_NONNUMERIC);
        decrRefCount(key);
        return;
    }

    uint64_t result;
    if (incr) {
        result = current + delta; /* Unsigned wraparound is intended. */
        server.mc_stats.incr_hits++;
    } else {
        result = current < delta ? 0 : current - delta;
        server.mc_stats.decr_hits++;
    }

    char numbuf[32];
    int numlen = snprintf(numbuf, sizeof(numbuf), "%llu",
                          (unsigned long long)result);

    /* The item keeps both its flags and its expiry. */
    uint32_t kept_flags = mcFlagsGet(c->db, key);
    long long kept_ttl = getExpire(c->db, key->ptr, o);

    mcStoreItem(c, key, tryObjectEncoding(createStringObject(numbuf, numlen)),
                kept_flags, kept_ttl == -1 ? MC_EXPIRE_NONE : MC_EXPIRE_AT,
                kept_ttl, incr ? "incrby" : "decrby");

    sds reply = sdsnewlen(numbuf, numlen);
    reply = sdscatlen(reply, "\r\n", 2);
    mcOutSds(r, reply);
    decrRefCount(key);
}

/* ------------------------------------------------------------------------
 * flush_all / version / stats
 * ------------------------------------------------------------------------ */

static void mcFlushAllCommand(mcRequest *r) {
    client *c = r->c;
    mcTokens *t = r->tokens;

    if (t->count > 2) {
        mcOut(r, MC_ERR_BADFMT);
        return;
    }
    if (t->count == 2) {
        int64_t delay;
        if (!mcTokI64(&t->v[1], &delay)) {
            mcOut(r, MC_ERR_BADFMT);
            return;
        }
        /* A delayed flush means keeping an "oldest live" watermark that every
         * read has to consult. Rather than silently flushing now and dropping
         * data the client asked us to keep for a while, say we cannot. */
        if (delay != 0) {
            mcOut(r, MC_ERR_NODELAY);
            return;
        }
    }
    if (!mcWriteAllowed(r)) return;

    server.mc_stats.cmd_flush++;

    /* memcached is pinned to database 0, so this is a FLUSHDB and not a
     * FLUSHALL. It does remove the Redis keys in database 0 too, which is the
     * price of sharing one keyspace. */
    long long removed = emptyData(c->db->id, EMPTYDB_NOFUNCTIONS, NULL);
    server.dirty += removed;

    robj *flushdb = createStringObject("FLUSHDB", 7);
    robj *argv[1] = {flushdb};
    alsoPropagate(c->db->id, argv, 1, PROPAGATE_AOF | PROPAGATE_REPL);
    decrRefCount(flushdb);

    mcOut(r, MC_OK);
}

static void mcStatsCommand(mcRequest *r) {
    client *c = r->c;
    mcTokens *t = r->tokens;

    if (t->count > 1) {
        if (mcTokEq(&t->v[1], "reset")) {
            long long conns = server.mc_stats.curr_connections;
            long long total = server.mc_stats.total_connections;
            memset(&server.mc_stats, 0, sizeof(server.mc_stats));
            server.mc_stats.curr_connections = conns;
            server.mc_stats.total_connections = total;
            mcOut(r, MC_RESET);
            return;
        }
        /* `stats items`, `stats slabs` and `stats sizes` all describe
         * memcached's slab allocator, which has no counterpart here.
         * Answering with an empty section is friendlier to tooling than
         * ERROR. */
        mcOut(r, MC_END);
        return;
    }

    mcStats *st = &server.mc_stats;
    sds info = sdscatprintf(sdsempty(),
        "STAT pid %ld\r\n"
        "STAT uptime %lld\r\n"
        "STAT time %lld\r\n"
        "STAT version %s\r\n"
        "STAT pointer_size %d\r\n"
        "STAT curr_connections %lld\r\n"
        "STAT total_connections %lld\r\n"
        "STAT threads %d\r\n",
        (long)getpid(),
        (long long)(server.unixtime - server.stat_starttime),
        (long long)server.unixtime,
        REDIS_VERSION,
        (int)(sizeof(void *) * 8),
        st->curr_connections,
        st->total_connections,
        server.io_threads_num);

    /* get_expired is always 0: lazy expiration does not report back to the
     * lookup whether it dropped a key, so an expired item is indistinguishable
     * from a missing one here and is counted as a plain miss. */
    info = sdscatprintf(info,
        "STAT cmd_get %lld\r\n"
        "STAT cmd_set %lld\r\n"
        "STAT cmd_flush %lld\r\n"
        "STAT cmd_touch %lld\r\n"
        "STAT get_hits %lld\r\n"
        "STAT get_misses %lld\r\n"
        "STAT get_expired 0\r\n"
        "STAT delete_hits %lld\r\n"
        "STAT delete_misses %lld\r\n"
        "STAT incr_hits %lld\r\n"
        "STAT incr_misses %lld\r\n"
        "STAT decr_hits %lld\r\n"
        "STAT decr_misses %lld\r\n"
        "STAT touch_hits %lld\r\n"
        "STAT touch_misses %lld\r\n",
        st->cmd_get, st->cmd_set, st->cmd_flush, st->cmd_touch,
        st->get_hits, st->get_misses,
        st->delete_hits, st->delete_misses,
        st->incr_hits, st->incr_misses,
        st->decr_hits, st->decr_misses,
        st->touch_hits, st->touch_misses);

    /* curr_items counts every key in database 0, memcached items and Redis
     * keys alike, because they are one keyspace. `bytes` is the process' used
     * memory rather than the summed size of stored items, which is what
     * memcached's slab accounting would report. */
    info = sdscatprintf(info,
        "STAT curr_items %lld\r\n"
        "STAT total_items %lld\r\n"
        "STAT bytes %zu\r\n"
        "STAT limit_maxbytes %lld\r\n"
        "STAT evictions %lld\r\n"
        "STAT reclaimed %lld\r\n"
        "END\r\n",
        (long long)kvstoreSize(c->db->keys),
        st->total_items,
        zmalloc_used_memory(),
        (long long)server.maxmemory,
        server.stat_evictedkeys,
        server.stat_expiredkeys);

    mcOutSds(r, info);
}

/* ------------------------------------------------------------------------
 * Dispatch
 * ------------------------------------------------------------------------ */

#define MC_RES_DONE     0 /* Command handled, c->qb_pos has been advanced. */
#define MC_RES_NEEDMORE 1 /* Incomplete, retry when more input arrives. */

#define MC_STORE_CAS   -1 /* Recognized so we can skip its data block. */

/* Recognize the commands that carry a data block. */
static int mcStoreMode(const mcToken *t, int *mode) {
    if (mcTokEq(t, "set"))     { *mode = MC_STORE_SET;     return 1; }
    if (mcTokEq(t, "add"))     { *mode = MC_STORE_ADD;     return 1; }
    if (mcTokEq(t, "replace")) { *mode = MC_STORE_REPLACE; return 1; }
    if (mcTokEq(t, "append"))  { *mode = MC_STORE_APPEND;  return 1; }
    if (mcTokEq(t, "prepend")) { *mode = MC_STORE_PREPEND; return 1; }
    if (mcTokEq(t, "cas"))     { *mode = MC_STORE_CAS;     return 1; }
    return 0;
}

/* Handle one command.
 *
 * `line`/`linelen` is a private copy of the command line with its terminator
 * stripped, and `hdrlen` is how many bytes of the query buffer that line
 * occupied. `avail` is how much of the query buffer is readable from qb_pos.
 *
 * On MC_RES_DONE this function has advanced c->qb_pos past everything it
 * consumed, and it always does so *before* touching the keyspace, so that no
 * pointer into the query buffer is live while a command runs. */
static int mcHandleLine(client *c, const char *line, size_t linelen,
                        size_t hdrlen, size_t avail)
{
    mcTokens tokens;
    mcRequest r = {.c = c, .tokens = &tokens, .noreply = 0};
    int result = MC_RES_DONE;

    mcTokenize(line, linelen, &tokens);

    if (tokens.count == 0) {
        c->qb_pos += hdrlen;
        addReplyProto(c, MC_ERROR, strlen(MC_ERROR));
        goto done;
    }

    int mode;
    if (mcStoreMode(&tokens.v[0], &mode)) {
        /* <command> <key> <flags> <exptime> <bytes> [<cas>] [noreply]\r\n
         * <data block>\r\n */
        mcTakeNoreply(&r);

        uint32_t flags;
        int64_t exptime;
        uint64_t bytes;
        int want = (mode == MC_STORE_CAS) ? 6 : 5;
        int header_ok = tokens.count == want &&
                        mcKeyValid(&tokens.v[1]) &&
                        mcTokU32(&tokens.v[2], &flags) &&
                        mcTokI64(&tokens.v[3], &exptime) &&
                        mcTokU64(&tokens.v[4], &bytes);
        if (!header_ok) {
            /* We do not know how long the data block is, so we cannot skip
             * it. memcached behaves the same way: the block that follows is
             * then read as commands and answered with ERROR. */
            c->qb_pos += hdrlen;
            mcOut(&r, MC_ERR_BADFMT);
            goto done;
        }

        if (bytes > MEMCACHED_MAX_ITEM_SIZE) {
            /* Never buffer an oversized item. Report the error now and drop
             * the data block as it arrives. */
            c->qb_pos += hdrlen;
            c->mc->swallow = (long long)bytes + 2;
            mcOut(&r, MC_ERR_TOOLARGE);
            goto done;
        }

        size_t need = hdrlen + (size_t)bytes + 2;
        if (avail < need) {
            result = MC_RES_NEEDMORE;
            goto done;
        }

        /* The data block starts right after the command line's terminator. */
        const char *data = c->querybuf + c->qb_pos + hdrlen;
        int chunk_ok = data[bytes] == '\r' && data[bytes + 1] == '\n';

        robj *key = NULL, *value = NULL;
        if (chunk_ok && mode != MC_STORE_CAS) {
            key = createStringObject(tokens.v[1].ptr, tokens.v[1].len);
            value = createStringObject(data, (size_t)bytes);
        }
        c->qb_pos += need; /* No pointer into querybuf may be used past here. */

        if (!chunk_ok) {
            /* The client mis-declared the length. We consumed exactly what it
             * told us to, so the stream stays aligned with its own framing.
             * This error is sent even under noreply: staying silent would
             * leave the client waiting on a store that never happened. */
            addReplyProto(c, MC_ERR_BADCHUNK, strlen(MC_ERR_BADCHUNK));
            goto done;
        }
        if (mode == MC_STORE_CAS) {
            mcOut(&r, MC_ERR_NOCAS);
            goto done;
        }

        mcStoreCommand(&r, mode, key, value, flags, exptime);
        goto done;
    }

    /* Everything else is a single line. */
    c->qb_pos += hdrlen;

    if (mcTokEq(&tokens.v[0], "get")) {
        mcGetCommand(&r, 1, 0, NULL);
    } else if (mcTokEq(&tokens.v[0], "gets")) {
        mcGetCommand(&r, 1, 1, NULL);
    } else if (mcTokEq(&tokens.v[0], "gat") || mcTokEq(&tokens.v[0], "gats")) {
        int with_cas = tokens.v[0].len == 4;
        int64_t exptime;
        if (tokens.count < 3 || !mcTokI64(&tokens.v[1], &exptime))
            mcOut(&r, MC_ERR_BADFMT);
        else
            mcGetCommand(&r, 2, with_cas, &exptime);
    } else if (mcTokEq(&tokens.v[0], "delete")) {
        mcTakeNoreply(&r);
        mcDeleteCommand(&r);
    } else if (mcTokEq(&tokens.v[0], "touch")) {
        mcTakeNoreply(&r);
        mcTouchCommand(&r);
    } else if (mcTokEq(&tokens.v[0], "incr")) {
        mcTakeNoreply(&r);
        mcIncrDecrCommand(&r, 1);
    } else if (mcTokEq(&tokens.v[0], "decr")) {
        mcTakeNoreply(&r);
        mcIncrDecrCommand(&r, 0);
    } else if (mcTokEq(&tokens.v[0], "flush_all")) {
        mcTakeNoreply(&r);
        mcFlushAllCommand(&r);
    } else if (mcTokEq(&tokens.v[0], "version")) {
        mcOut(&r, "VERSION " MC_ADVERTISED_VERSION "\r\n");
    } else if (mcTokEq(&tokens.v[0], "stats")) {
        mcStatsCommand(&r);
    } else if (mcTokEq(&tokens.v[0], "verbosity")) {
        /* Accepted and ignored: some clients send it while setting up a
         * connection and treat an ERROR as a fatal handshake failure. */
        mcTakeNoreply(&r);
        mcOut(&r, MC_OK);
    } else if (mcTokEq(&tokens.v[0], "quit")) {
        /* memcached's quit answers nothing and closes. CLIENT_CLOSE_AFTER_REPLY
         * only takes effect once a reply has been written, so when there is
         * nothing queued we have to close the client ourselves. */
        if (clientHasPendingReplies(c))
            c->flags |= CLIENT_CLOSE_AFTER_REPLY;
        else
            freeClientAsync(c);
    } else {
        addReplyProto(c, MC_ERROR, strlen(MC_ERROR));
    }

done:
    mcTokensFree(&tokens);
    return result;
}

/* ------------------------------------------------------------------------
 * Input buffer processing
 * ------------------------------------------------------------------------ */

int memcachedProcessInputBuffer(client *c) {
    mcClient *mc = c->mc;
    char linebuf[512];
    serverAssert(mc != NULL);

    while (c->querybuf && c->qb_pos < sdslen(c->querybuf)) {
        if (c->flags & (CLIENT_CLOSE_AFTER_REPLY | CLIENT_CLOSE_ASAP)) break;

        size_t avail = sdslen(c->querybuf) - c->qb_pos;

        /* Drop the tail of an item we already rejected as too large. */
        if (mc->swallow > 0) {
            size_t drop = (size_t)mc->swallow < avail ? (size_t)mc->swallow
                                                      : avail;
            c->qb_pos += drop;
            mc->swallow -= (long long)drop;
            continue;
        }

        char *buf = c->querybuf + c->qb_pos;
        char *nl = memchr(buf, '\n', avail);
        if (nl == NULL) {
            if (avail > MEMCACHED_MAX_LINE_LEN) {
                addReplyProto(c, MC_ERROR, strlen(MC_ERROR));
                c->flags |= CLIENT_CLOSE_AFTER_REPLY;
            }
            break; /* Wait for the rest of the line. */
        }

        size_t hdrlen = (size_t)(nl - buf) + 1; /* Including the '\n'. */
        if (hdrlen > MEMCACHED_MAX_LINE_LEN) {
            addReplyProto(c, MC_ERROR, strlen(MC_ERROR));
            c->flags |= CLIENT_CLOSE_AFTER_REPLY;
            break;
        }

        size_t linelen = hdrlen - 1;
        if (linelen > 0 && buf[linelen - 1] == '\r') linelen--;

        /* Work from a copy. Handlers keep running after qb_pos moves, and
         * storing can evict, which can move the query buffer under us. */
        char *line = linelen < sizeof(linebuf) ? linebuf : zmalloc(linelen + 1);
        memcpy(line, buf, linelen);
        line[linelen] = '\0';

        client *old_client = server.current_client;
        server.current_client = c;
        enterExecutionUnit(1, 0);

        int res = mcHandleLine(c, line, linelen, hdrlen, avail);

        exitExecutionUnit();
        postExecutionUnitOperations();
        server.current_client = old_client;

        if (line != linebuf) zfree(line);
        if (res == MC_RES_NEEDMORE) break;
    }

    /* Drop what we consumed, so the reusable query buffer can be handed back
     * and so a partially received item does not keep growing the buffer. */
    if (c->qb_pos) {
        sdsrange(c->querybuf, c->qb_pos, -1);
        c->qb_pos = 0;
    }

    if (c->running_tid == IOTHREAD_MAIN_THREAD_ID)
        updateClientMemUsageAndBucket(c);
    return C_OK;
}

/* ------------------------------------------------------------------------
 * Client lifecycle
 * ------------------------------------------------------------------------ */

void memcachedClientCreated(client *c) {
    c->mc = zcalloc(sizeof(mcClient));
    selectDb(c, 0); /* memcached has no SELECT: database 0, always. */
    server.mc_stats.total_connections++;
    server.mc_stats.curr_connections++;
}

void memcachedClientFreed(client *c) {
    if (c->mc == NULL) return;
    zfree(c->mc);
    c->mc = NULL;
    server.mc_stats.curr_connections--;
}

/* ------------------------------------------------------------------------
 * Listener and configuration
 * ------------------------------------------------------------------------ */

void memcachedInit(void) {
    memset(&server.mc_stats, 0, sizeof(server.mc_stats));
}

/* True if this server has any authentication configured, in which case an
 * unauthenticated memcached port would be a way straight around it. */
static int memcachedAuthIsConfigured(void) {
    uint32_t flags;
    atomicGet(DefaultUser->flags, flags);
    if (!(flags & USER_FLAG_NOPASS)) return 1;
    if (flags & USER_FLAG_DISABLED) return 1;
    if (server.requirepass != NULL) return 1;
    if (raxSize(Users) > 1) return 1; /* Any user besides "default". */
    return 0;
}

/* Called once the configuration is fully loaded and before we start
 * listening. Both refusals below are hard: quietly running without the port
 * the operator asked for, and quietly serving an unauthenticated port on a
 * server that has authentication configured, are each worse than not
 * starting at all. */
void memcachedValidateConfigOrExit(void) {
    if (server.memcached_port == 0) return;

    if (server.cluster_enabled) {
        serverLog(LL_WARNING,
            "memcached-port is set but cluster mode is enabled. The memcached "
            "protocol has no way to express slot redirection, so it is "
            "supported in standalone mode only. Unset memcached-port or "
            "disable cluster-enabled.");
        exit(1);
    }

    if (memcachedAuthIsConfigured() && !server.memcached_insecure_allow_noauth) {
        serverLog(LL_WARNING,
            "memcached-port is set on a server that has requirepass or ACL "
            "users configured. The memcached protocol has no authentication, "
            "so this port would hand out unauthenticated access to the same "
            "keyspace. Set 'memcached-insecure-allow-noauth yes' if that is "
            "really what you want, or unset memcached-port.");
        exit(1);
    }
}

static void memcachedAcceptHandler(aeEventLoop *el, int fd, void *privdata,
                                   int mask)
{
    int cport, cfd;
    int max = server.max_new_conns_per_cycle;
    char cip[NET_IP_STR_LEN];
    UNUSED(mask);
    UNUSED(privdata);

    while (max--) {
        cfd = anetTcpAccept(server.neterr, fd, cip, sizeof(cip), &cport);
        if (cfd == ANET_ERR) {
            if (anetAcceptFailureNeedsRetry(errno)) continue;
            if (errno != EWOULDBLOCK)
                serverLog(LL_WARNING,
                          "Accepting memcached client connection: %s",
                          server.neterr);
            return;
        }
        serverLog(LL_VERBOSE, "Accepted memcached client %s:%d", cip, cport);
        acceptCommonHandler(connCreateAccepted(el, connectionTypeTcp(), cfd, NULL),
                            CLIENT_MEMCACHED, cip);
    }
}

void memcachedInitListener(void) {
    if (server.memcached_port == 0) return;

    connListener *listener = &server.memcached_listener;
    listener->bindaddr = server.bindaddr;
    listener->bindaddr_count = server.bindaddr_count;
    listener->port = server.memcached_port;
    listener->ct = connectionByType(CONN_TYPE_SOCKET);

    if (connListen(listener) == C_ERR) {
        serverLog(LL_WARNING,
                  "Failed listening on memcached port %d, aborting.",
                  server.memcached_port);
        exit(1);
    }
    if (createSocketAcceptHandler(listener, memcachedAcceptHandler) != C_OK)
        serverPanic("Unrecoverable error creating memcached listener "
                    "accept handler.");

    serverLog(LL_NOTICE,
              "Listening for the memcached text protocol on port %d.",
              server.memcached_port);
}
