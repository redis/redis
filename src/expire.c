/* Implementation of EXPIRE (keys with fixed time to live).
 *
 * ----------------------------------------------------------------------------
 *
 * Copyright (c) 2009-2016, Salvatore Sanfilippo <antirez at gmail dot com>
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

#include "server.h"

/*-----------------------------------------------------------------------------
 * Incremental collection of expired keys.
 *
 * When keys are accessed they are expired on-access. However we need a
 * mechanism in order to ensure keys are eventually removed when expired even
 * if no access is performed on them.
 *
 * In order to accomplish this every key with an expire is represented in
 * two data structures:
 *
 * 1. The main dictionary of keys, server.db[x]->dict, is an hash table that
 *    represents the keyspace of a given Redis database. The keys stored
 *    in the hash table are redisKey structures (typedef 'rkey'). When
 *    a key has an expire set, the key->flags have the KEY_FLAG_EXPIRE set,
 *    and the key->expire is populated with the milliseconds unix time at
 *    which the key will no longer be valid.
 *
 * 2. Redis also takes a radix tree that is composed only of keys that have
 *    an expire set, lexicographically sorted by the expire time. Basically
 *    each key in the radix tree is composed as follows:
 *
 *    [8 bytes expire unix time][8 bytes key object pointer]
 *
 *    Such tree is stored in server.db[x]->expire.
 *
 * The first field, the unix time, is the same stored in the key->expire of
 * the corresponding key in the hash table, however it is stored in big endian
 * so that sorting the time lexicographically in the tree, will also make the
 * tree sorted by numerical expire time (from the smallest unix time to the
 * greatest one).
 *
 * Then we store the key pointer, this time in native endianess, because how
 * it is sorted does not matter, being after the unix time. If Redis is running
 * as a 32 bit system, the last 4 bytes of the pointer are just zeroes, so
 * we can assume a 16 bytes key in every architecture. Note that from the
 * pointer we can retrieve the key name, lookup it in the main dictionary, and
 * delete the key.
 *
 * On the other hand, when we modify the expire time of some key, we need to
 * update the tree accordingly. At every expire cycle, what we need to do is
 * conceptually very simple: we run the tree and expire keys as long as we
 * find keys that are already logically expired (expire time > current time).
 *
 *----------------------------------------------------------------------------*/

#define EXPIRE_KEY_LEN 16  /* Key length in the radix tree of expires. */

/* Populate the buffer 'buf', that should be at least EXPIRE_KEY_LEN bytes,
 * with the key to store such key in the expires radix tree. See the comment
 * above to see the format. */
void encodeExpireKey(unsigned char *buf, rkey *key) {
    uint64_t expire = htonu64(key->expire);
    uint64_t ptr = (uint64_t) key; /* The pointer may be 32 bit, cast to 64. */
    memcpy(buf,&expire,sizeof(expire));
    memcpy(buf+8,&ptr,sizeof(ptr));
}

/* This is the reverse of encodeExpireKey(): given the key will return a
 * pointer to an rkey and the expire value. */
void decodeExpireKey(unsigned char *buf, uint64_t *expireptr, rkey **keyptrptr) {
    uint64_t expire;
    uint64_t keyptr;
    memcpy(&expire,buf,sizeof(expire));
    expire = ntohu64(expire);
    memcpy(&keyptr,buf+8,sizeof(keyptr));
    *expireptr = expire;
    *keyptrptr = (rkey*)(unsigned long)keyptr;
}

/* Populate the expires radix tree with the specified key. */
void addExpireToTree(redisDb *db, rkey *key) {
    unsigned char expirekey[EXPIRE_KEY_LEN];
    encodeExpireKey(expirekey,key);
    int retval = raxTryInsert(db->expires,expirekey,EXPIRE_KEY_LEN,NULL,NULL);
    serverAssert(retval != 0);
}

/* Remove the specified key from the expires radix tree. */
void removeExpireFromTree(redisDb *db, rkey *key) {
    unsigned char expirekey[EXPIRE_KEY_LEN];
    encodeExpireKey(expirekey,key);
    int retval = raxRemove(db->expires,expirekey,EXPIRE_KEY_LEN,NULL);
    serverAssert(retval != 0);
}

/* Delete a key that is found expired by the expiration cycle. We need to
 * propagate the key too, send the notification event, and take a few
 * stats. */
void deleteExpiredKey(redisDb *db, rkey *key) {
    robj *keyname = createStringObject(key->name,key->len);

    propagateExpire(db,keyname,server.lazyfree_lazy_expire);
    if (server.lazyfree_lazy_expire)
        dbAsyncDelete(db,keyname);
    else
        dbSyncDelete(db,keyname);
    notifyKeyspaceEvent(NOTIFY_EXPIRED,
        "expired",keyname,db->id);
    decrRefCount(keyname);
    server.stat_expiredkeys++;
}

/* Helper function for the expireSlaveKeys() function.
 * This function will try to expire the key that is stored in the hash table
 * entry 'de' of the 'expires' hash table of a Redis database.
 *
 * If the key is found to be expired, it is removed from the database and
 * 1 is returned. Otherwise no operation is performed and 0 is returned.
 *
 * When a key is expired, server.stat_expiredkeys is incremented.
 *
 * The parameter 'now' is the current time in milliseconds as is passed
 * to the function to avoid too many gettimeofday() syscalls. */
int activeExpireCycleTryExpire(redisDb *db, rkey *key, long long now) {
    long long t = key->expire;
    if (now > t) {
        deleteExpiredKey(db,key);
        return 1;
    } else {
        return 0;
    }
}

/* Try to expire a few timed out keys. The algorithm used is adaptive and
 * will use few CPU cycles if there are few expiring keys, otherwise
 * it will get more aggressive to avoid that too much memory is used by
 * keys that can be removed from the keyspace.
 *
 * No more than CRON_DBS_PER_CALL databases are tested at every
 * iteration.
 *
 * This kind of call is used when Redis detects that timelimit_exit is
 * true, so there is more work to do, and we do it more incrementally from
 * the beforeSleep() function of the event loop.
 *
 * Expire cycle type:
 *
 * If type is ACTIVE_EXPIRE_CYCLE_FAST the function will try to run a
 * "fast" expire cycle that takes no longer than EXPIRE_FAST_CYCLE_DURATION
 * microseconds, and is not repeated again before the same amount of time.
 *
 * If type is ACTIVE_EXPIRE_CYCLE_SLOW, that normal expire cycle is
 * executed, where the time limit is a percentage of the REDIS_HZ period
 * as specified by the ACTIVE_EXPIRE_CYCLE_SLOW_TIME_PERC define. */

void activeExpireCycle(int type) {
    /* This function has some global state in order to continue the work
     * incrementally across calls. */
    static unsigned int current_db = 0; /* Last DB tested. */
    static int timelimit_exit = 0;      /* Time limit hit in previous call? */
    static long long last_fast_cycle = 0; /* When last fast cycle ran. */

    int j;
    unsigned long iteration = 0;
    int dbs_per_call = CRON_DBS_PER_CALL;
    long long start = ustime(), timelimit, elapsed;

    /* When clients are paused the dataset should be static not just from the
     * POV of clients not being able to write, but also from the POV of
     * expires and evictions of keys not being performed. */
    if (clientsArePaused()) return;

    if (type == ACTIVE_EXPIRE_CYCLE_FAST) {
        /* Don't start a fast cycle if the previous cycle did not exit
         * for time limit. Also don't repeat a fast cycle for the same period
         * as the fast cycle total duration itself. */
        if (!timelimit_exit) return;
        if (start < last_fast_cycle + ACTIVE_EXPIRE_CYCLE_FAST_DURATION*2) return;
        last_fast_cycle = start;
    }

    /* We usually should test CRON_DBS_PER_CALL per iteration, with
     * two exceptions:
     *
     * 1) Don't test more DBs than we have.
     * 2) If last time we hit the time limit, we want to scan all DBs
     * in this iteration, as there is work to do in some DB and we don't want
     * expired keys to use memory for too much time. */
    if (dbs_per_call > server.dbnum || timelimit_exit)
        dbs_per_call = server.dbnum;

    /* We can use at max ACTIVE_EXPIRE_CYCLE_SLOW_TIME_PERC percentage of
     * CPU time per iteration. Since this function gets called with a
     * frequency of server.hz times per second, the following is the max
     * amount of microseconds we can spend in this function. */
    timelimit = 1000000*ACTIVE_EXPIRE_CYCLE_SLOW_TIME_PERC/server.hz/100;
    timelimit_exit = 0;
    if (timelimit <= 0) timelimit = 1;

    /* If it's a fast cycle, override the time limit with our fixed
     * time limit (defaults to 1 millisecond). */
    if (type == ACTIVE_EXPIRE_CYCLE_FAST)
        timelimit = ACTIVE_EXPIRE_CYCLE_FAST_DURATION; /* in microseconds. */

    for (j = 0; j < dbs_per_call && timelimit_exit == 0; j++) {
        redisDb *db = server.db+(current_db % server.dbnum);

        /* Increment the DB now so we are sure if we run out of time
         * in the current DB we'll restart from the next. This allows to
         * distribute the time evenly across DBs. */
        current_db++;

        /* If there is nothing to expire try next DB ASAP, avoiding the
         * cost of seeking the radix tree iterator. */
        if (raxSize(db->expires) == 0) continue;

        /* The main collection cycle. Run the tree and expire keys that
         * are found to be already logically expired. */
        long long now = mstime();
        raxIterator ri;
        raxStart(&ri,db->expires);
        raxSeek(&ri,"^",NULL,0);

        /* Enter the loop expiring keys for this database. Inside this
         * loop there are two stop conditions:
         *
         * 1. The time limit.
         * 2. The loop will exit if in this DB there are no more keys
         *    that are logically expired.
         *
         * Moreover the loop naturally terminates when there are no longer
         * elements in the radix tree. */
        while(raxNext(&ri)) {
            rkey *key;
            uint64_t expire;
            decodeExpireKey(ri.key,&expire,&key);

            /* First stop condition: no keys to expire here. */
            if (expire >= (uint64_t)now) break;

            printf("DEL %.*s -> %llu\n", (int)key->len, key->name, expire);
            deleteExpiredKey(db,key);

            /* Second stop condition: the time limit. */
            iteration++;
            if ((iteration & 0xff) == 0) {
                now = ustime();
                elapsed = now-start;
                now /= 1000; /* Convert back now to milliseconds. */
                if (elapsed > timelimit) {
                    timelimit_exit = 1;
                    printf("LIMIT (%llu) type:%d [elapsed=%llu]\n", timelimit, type, elapsed);
                    server.stat_expired_time_cap_reached_count++;
                    break;
                }
            }
            /* Reseek the iterator: the node we were on is now
             * deleted. */
            raxSeek(&ri,"^",NULL,0);
        }
        raxStop(&ri);
    }

    elapsed = ustime()-start;
    latencyAddSampleIfNeeded("expire-cycle",elapsed/1000);
}

/*-----------------------------------------------------------------------------
 * Expires of keys created in writable slaves
 *
 * Normally slaves do not process expires: they wait the masters to synthesize
 * DEL operations in order to retain consistency. However writable slaves are
 * an exception: if a key is created in the slave and an expire is assigned
 * to it, we need a way to expire such a key, since the master does not know
 * anything about such a key.
 *
 * In order to do so, we track keys created in the slave side with an expire
 * set, and call the expireSlaveKeys() function from time to time in order to
 * reclaim the keys if they already expired.
 *
 * Note that the use case we are trying to cover here, is a popular one where
 * slaves are put in writable mode in order to compute slow operations in
 * the slave side that are mostly useful to actually read data in a more
 * processed way. Think at sets intersections in a tmp key, with an expire so
 * that it is also used as a cache to avoid intersecting every time.
 *
 * This implementation is currently not perfect but a lot better than leaking
 * the keys as implemented in 3.2.
 *----------------------------------------------------------------------------*/

/* The dictionary where we remember key names and database ID of keys we may
 * want to expire from the slave. Since this function is not often used we
 * don't even care to initialize the database at startup. We'll do it once
 * the feature is used the first time, that is, when the function
 * rememberSlaveKeyWithExpire() is called.
 *
 * The dictionary has an SDS string representing the key as the hash table
 * key, while the value is a 64 bit unsigned integer with the bits corresponding
 * to the DB where the keys may exist set to 1. Currently the keys created
 * with a DB id > 63 are not expired, but a trivial fix is to set the bitmap
 * to the max 64 bit unsigned value when we know there is a key with a DB
 * ID greater than 63, and check all the configured DBs in such a case. */
dict *slaveKeysWithExpire = NULL;

/* Check the set of keys created by the master with an expire set in order to
 * check if they should be evicted. */
void expireSlaveKeys(void) {
    if (slaveKeysWithExpire == NULL ||
        dictSize(slaveKeysWithExpire) == 0) return;

    int cycles = 0, noexpire = 0;
    mstime_t start = mstime();
    while(1) {
        dictEntry *de = dictGetRandomKey(slaveKeysWithExpire);
        sds keyname = dictGetKey(de);
        uint64_t dbids = dictGetUnsignedIntegerVal(de);
        uint64_t new_dbids = 0;

        /* Check the key against every database corresponding to the
         * bits set in the value bitmap. */
        int dbid = 0;
        while(dbids && dbid < server.dbnum) {
            if ((dbids & 1) != 0) {
                redisDb *db = server.db+dbid;
                rkey *key = dictFetchValue(db->dict,keyname);
                if (!(key->flags & KEY_FLAG_EXPIRE)) key = NULL;
                int expired = 0;

                if (key &&
                    activeExpireCycleTryExpire(server.db+dbid,key,start))
                {
                    expired = 1;
                }

                /* If the key was not expired in this DB, we need to set the
                 * corresponding bit in the new bitmap we set as value.
                 * At the end of the loop if the bitmap is zero, it means we
                 * no longer need to keep track of this key. */
                if (key && !expired) {
                    noexpire++;
                    new_dbids |= (uint64_t)1 << dbid;
                }
            }
            dbid++;
            dbids >>= 1;
        }

        /* Set the new bitmap as value of the key, in the dictionary
         * of keys with an expire set directly in the writable slave. Otherwise
         * if the bitmap is zero, we no longer need to keep track of it. */
        if (new_dbids)
            dictSetUnsignedIntegerVal(de,new_dbids);
        else
            dictDelete(slaveKeysWithExpire,keyname);

        /* Stop conditions: found 3 keys we cna't expire in a row or
         * time limit was reached. */
        cycles++;
        if (noexpire > 3) break;
        if ((cycles % 64) == 0 && mstime()-start > 1) break;
        if (dictSize(slaveKeysWithExpire) == 0) break;
    }
}

/* Track keys that received an EXPIRE or similar command in the context
 * of a writable slave. */
void rememberSlaveKeyWithExpire(redisDb *db, rkey *key) {
    if (slaveKeysWithExpire == NULL) {
        static dictType dt = {
            dictSdsHash,                /* lookup hash function */
            dictSdsHash,                /* stored hash function */
            NULL,                       /* key dup */
            NULL,                       /* val dup */
            dictSdsKeyCompare,          /* loopkup key compare */
            dictSdsKeyCompare,          /* stored key compare */
            dictSdsDestructor,          /* key destructor */
            NULL                        /* val destructor */
        };
        slaveKeysWithExpire = dictCreate(&dt,NULL);
    }
    if (db->id > 63) return;

    sds skey = sdsnewlen(key->name,key->len);
    dictEntry *de = dictAddOrFind(slaveKeysWithExpire,skey);
    /* If the entry was already there, free the SDS string we used to lookup.
     * Note that we don't care to take those keys in sync with the
     * main DB. The keys will be removed by expireSlaveKeys() as it scans to
     * find keys to remove. */
    if (de->key != skey) {
        sdsfree(skey);
    } else {
        dictSetUnsignedIntegerVal(de,0);
    }

    uint64_t dbids = dictGetUnsignedIntegerVal(de);
    dbids |= (uint64_t)1 << db->id;
    dictSetUnsignedIntegerVal(de,dbids);
}

/* Return the number of keys we are tracking. */
size_t getSlaveKeyWithExpireCount(void) {
    if (slaveKeysWithExpire == NULL) return 0;
    return dictSize(slaveKeysWithExpire);
}

/* Remove the keys in the hash table. We need to do that when data is
 * flushed from the server. We may receive new keys from the master with
 * the same name/db and it is no longer a good idea to expire them.
 *
 * Note: technically we should handle the case of a single DB being flushed
 * but it is not worth it since anyway race conditions using the same set
 * of key names in a wriatable slave and in its master will lead to
 * inconsistencies. This is just a best-effort thing we do. */
void flushSlaveKeysWithExpireList(void) {
    if (slaveKeysWithExpire) {
        dictRelease(slaveKeysWithExpire);
        slaveKeysWithExpire = NULL;
    }
}

/*-----------------------------------------------------------------------------
 * Expires API
 *----------------------------------------------------------------------------*/

/* Remove the expire from the key making it persistent. */
int removeExpire(redisDb *db, rkey *key) {
    if (!(key->flags & KEY_FLAG_EXPIRE)) return 0;
    removeExpireFromTree(db,key);
    key->flags &= ~KEY_FLAG_EXPIRE;
    key->expire = 0; /* Not needed but better to leave the object clean. */
    return 1;
}

/* Set an expire to the specified key. If the expire is set in the context
 * of an user calling a command 'c' is the client, otherwise 'c' is set
 * to NULL. The 'when' parameter is the absolute unix time in milliseconds
 * after which the key will no longer be considered valid. */
void setExpire(client *c, redisDb *db, rkey *key, long long when) {
    /* Reuse the sds from the main dict in the expire dict */
    if (key->flags & KEY_FLAG_EXPIRE) removeExpireFromTree(db,key);
    key->flags |= KEY_FLAG_EXPIRE;
    key->expire = when;
    addExpireToTree(db,key);

    int writable_slave = server.masterhost && server.repl_slave_ro == 0;
    if (c && writable_slave && !(c->flags & CLIENT_MASTER))
        rememberSlaveKeyWithExpire(db,key);
}

/* Return the expire time of the specified key, or -1 if no expire
 * is associated with this key (i.e. the key is non volatile) */
long long getExpire(rkey *key) {
    return (key->flags & KEY_FLAG_EXPIRE) ? key->expire : -1;
}

/* Propagate expires into slaves and the AOF file.
 * When a key expires in the master, a DEL operation for this key is sent
 * to all the slaves and the AOF file if enabled.
 *
 * This way the key expiry is centralized in one place, and since both
 * AOF and the master->slave link guarantee operation ordering, everything
 * will be consistent even if we allow write operations against expiring
 * keys. */
void propagateExpire(redisDb *db, robj *key, int lazy) {
    robj *argv[2];

    argv[0] = lazy ? shared.unlink : shared.del;
    argv[1] = key;
    incrRefCount(argv[0]);
    incrRefCount(argv[1]);

    if (server.aof_state != AOF_OFF)
        feedAppendOnlyFile(server.delCommand,db->id,argv,2);
    replicationFeedSlaves(server.slaves,db->id,argv,2);

    decrRefCount(argv[0]);
    decrRefCount(argv[1]);
}

/* Check if the key is expired. */
int keyIsExpired(rkey *key) {
    mstime_t when = getExpire(key);

    if (when < 0) return 0; /* No expire for this key */

    /* Don't expire anything while loading. It will be done later. */
    if (server.loading) return 0;

    /* If we are in the context of a Lua script, we pretend that time is
     * blocked to when the Lua script started. This way a key can expire
     * only the first time it is accessed and not in the middle of the
     * script execution, making propagation to slaves / AOF consistent.
     * See issue #1525 on Github for more information. */
    mstime_t now = server.lua_caller ? server.lua_time_start : mstime();

    return now > when;
}

/* This function is called when we are going to perform some operation
 * in a given key, but such key may be already logically expired even if
 * it still exists in the database. The main way this function is called
 * is via lookupKey*() family of functions.
 *
 * The behavior of the function depends on the replication role of the
 * instance, because slave instances do not expire keys, they wait
 * for DELs from the master for consistency matters. However even
 * slaves will try to have a coherent return value for the function,
 * so that read commands executed in the slave side will be able to
 * behave like if the key is expired even if still present (because the
 * master has yet to propagate the DEL).
 *
 * In masters as a side effect of finding a key which is expired, such
 * key will be evicted from the database. Also this may trigger the
 * propagation of a DEL/UNLINK command in AOF / replication stream.
 *
 * The return value of the function is 0 if the key is still valid,
 * otherwise the function returns 1 if the key is expired. */
int expireIfNeeded(redisDb *db, robj *keyname, rkey *key) {
    if (!keyIsExpired(key)) return 0;

    /* If we are running in the context of a slave, instead of
     * evicting the expired key from the database, we return ASAP:
     * the slave key expiration is controlled by the master that will
     * send us synthesized DEL operations for expired keys.
     *
     * Still we try to return the right information to the caller,
     * that is, 0 if we think the key should be still valid, 1 if
     * we think the key is expired at this time. */
    if (server.masterhost != NULL) return 1;

    /* Delete the key */
    server.stat_expiredkeys++;
    propagateExpire(db,keyname,server.lazyfree_lazy_expire);
    notifyKeyspaceEvent(NOTIFY_EXPIRED,
        "expired",keyname,db->id);
    return server.lazyfree_lazy_expire ? dbAsyncDelete(db,keyname) :
                                         dbSyncDelete(db,keyname);
}

/* Sometimes we have just the name of the key, because we have still to
 * lookup it. In such cases this function is more handy compared to
 * expireIfNeeded(): just a wrapper performing the lookup first. */
int expireIfNeededByName(redisDb *db, robj *keyname) {
    rkey *key;
    robj *val = lookupKey(db,keyname,&key,LOOKUP_NOTOUCH);
    if (!val) return 0;
    return expireIfNeeded(db,keyname,key);
}



/*-----------------------------------------------------------------------------
 * Expires Commands
 *----------------------------------------------------------------------------*/

/* This is the generic command implementation for EXPIRE, PEXPIRE, EXPIREAT
 * and PEXPIREAT. Because the commad second argument may be relative or absolute
 * the "basetime" argument is used to signal what the base time is (either 0
 * for *AT variants of the command, or the current time for relative expires).
 *
 * unit is either UNIT_SECONDS or UNIT_MILLISECONDS, and is only used for
 * the argv[2] parameter. The basetime is always specified in milliseconds. */
void expireGenericCommand(client *c, long long basetime, int unit) {
    robj *keyname = c->argv[1], *param = c->argv[2];
    rkey *key;
    long long when; /* unix time in milliseconds when the key will expire. */

    if (getLongLongFromObjectOrReply(c, param, &when, NULL) != C_OK)
        return;

    if (unit == UNIT_SECONDS) when *= 1000;
    when += basetime;

    /* No key, return zero. */
    if (lookupKeyWrite(c->db,keyname,&key) == NULL) {
        addReply(c,shared.czero);
        return;
    }

    /* EXPIRE with negative TTL, or EXPIREAT with a timestamp into the past
     * should never be executed as a DEL when load the AOF or in the context
     * of a slave instance.
     *
     * Instead we take the other branch of the IF statement setting an expire
     * (possibly in the past) and wait for an explicit DEL from the master. */
    if (when <= mstime() && !server.loading && !server.masterhost) {
        robj *aux;

        int deleted = server.lazyfree_lazy_expire ?
                        dbAsyncDelete(c->db,keyname) :
                        dbSyncDelete(c->db,keyname);
        serverAssertWithInfo(c,keyname,deleted);
        server.dirty++;

        /* Replicate/AOF this as an explicit DEL or UNLINK. */
        aux = server.lazyfree_lazy_expire ? shared.unlink : shared.del;
        rewriteClientCommandVector(c,2,aux,keyname);
        signalModifiedKey(c->db,keyname);
        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",keyname,c->db->id);
        addReply(c, shared.cone);
        return;
    } else {
        setExpire(c,c->db,key,when);
        addReply(c,shared.cone);
        signalModifiedKey(c->db,keyname);
        notifyKeyspaceEvent(NOTIFY_GENERIC,"expire",keyname,c->db->id);
        server.dirty++;
        return;
    }
}

/* EXPIRE key seconds */
void expireCommand(client *c) {
    expireGenericCommand(c,mstime(),UNIT_SECONDS);
}

/* EXPIREAT key time */
void expireatCommand(client *c) {
    expireGenericCommand(c,0,UNIT_SECONDS);
}

/* PEXPIRE key milliseconds */
void pexpireCommand(client *c) {
    expireGenericCommand(c,mstime(),UNIT_MILLISECONDS);
}

/* PEXPIREAT key ms_time */
void pexpireatCommand(client *c) {
    expireGenericCommand(c,0,UNIT_MILLISECONDS);
}

/* Implements TTL and PTTL */
void ttlGenericCommand(client *c, int output_ms) {
    long long expire, ttl = -1;
    rkey *key;

    /* If the key does not exist at all, return -2 */
    if (lookupKeyReadWithFlags(c->db,c->argv[1],&key,LOOKUP_NOTOUCH) == NULL) {
        addReplyLongLong(c,-2);
        return;
    }
    /* The key exists. Return -1 if it has no expire, or the actual
     * TTL value otherwise. */
    expire = getExpire(key);
    if (expire != -1) {
        ttl = expire-mstime();
        if (ttl < 0) ttl = 0;
    }
    if (ttl == -1) {
        addReplyLongLong(c,-1);
    } else {
        addReplyLongLong(c,output_ms ? ttl : ((ttl+500)/1000));
    }
}

/* TTL key */
void ttlCommand(client *c) {
    ttlGenericCommand(c, 0);
}

/* PTTL key */
void pttlCommand(client *c) {
    ttlGenericCommand(c, 1);
}

/* PERSIST key */
void persistCommand(client *c) {
    rkey *key;
    if (lookupKeyWrite(c->db,c->argv[1],&key)) {
        if (removeExpire(c->db,key)) {
            addReply(c,shared.cone);
            server.dirty++;
        } else {
            addReply(c,shared.czero);
        }
    } else {
        addReply(c,shared.czero);
    }
}

/* TOUCH key1 [key2 key3 ... keyN] */
void touchCommand(client *c) {
    int touched = 0;
    for (int j = 1; j < c->argc; j++)
        if (lookupKeyRead(c->db,c->argv[j],NULL) != NULL) touched++;
    addReplyLongLong(c,touched);
}

