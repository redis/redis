/*
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Copyright (c) 2024-present, Valkey contributors.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * Portions of this file are available under BSD3 terms; see REDISCONTRIBUTIONS for more information.
 */

/*
 * cluster.c contains the common parts of a clustering
 * implementation, the parts that are shared between
 * any implementation of Redis clustering.
 */

#include "server.h"
#include "cluster.h"
#include "cluster_asm.h"
#include "cluster_slot_stats.h"

#include <ctype.h>

/* -----------------------------------------------------------------------------
 * Key space handling
 * -------------------------------------------------------------------------- */

/* If it can be inferred that the given glob-style pattern, as implemented in
 * stringmatchlen() in util.c, only can match keys belonging to a single slot,
 * that slot is returned. Otherwise -1 is returned. */
int patternHashSlot(char *pattern, int length) {
    int s = -1; /* index of the first '{' */

    for (int i = 0; i < length; i++) {
        if (pattern[i] == '*' || pattern[i] == '?' || pattern[i] == '[') {
            /* Wildcard or character class found. Keys can be in any slot. */
            return -1;
        } else if (pattern[i] == '\\') {
            /* Escaped character. Computing slot in this case is not
             * implemented. We would need a temp buffer. */
            return -1;
        } else if (s == -1 && pattern[i] == '{') {
            /* Opening brace '{' found. */
            s = i;
        } else if (s >= 0 && pattern[i] == '}' && i == s + 1) {
            /* Empty tag '{}' found. The whole key is hashed. Ignore braces. */
            s = -2;
        } else if (s >= 0 && pattern[i] == '}') {
            /* Non-empty tag '{...}' found. Hash what's between braces. */
            return crc16(pattern + s + 1, i - s - 1) & 0x3FFF;
        }
    }

    /* The pattern matches a single key. Hash the whole pattern. */
    return crc16(pattern, length) & 0x3FFF;
}

int getSlotOrReply(client *c, robj *o) {
    long long slot;

    if (getLongLongFromObject(o,&slot) != C_OK ||
        slot < 0 || slot >= CLUSTER_SLOTS)
    {
        addReplyError(c,"Invalid or out of range slot");
        return -1;
    }
    return (int) slot;
}

ConnectionType *connTypeOfCluster(void) {
    if (server.tls_cluster) {
        return connectionTypeTls();
    }

    return connectionTypeTcp();
}

/* -----------------------------------------------------------------------------
 * DUMP, RESTORE and MIGRATE commands
 * -------------------------------------------------------------------------- */

/* Generates a DUMP-format representation of the object 'o', adding it to the
 * io stream pointed by 'rio'. This function can't fail. */
void createDumpPayload(rio *payload, robj *o, robj *key, int dbid, int skip_checksum) {
    unsigned char buf[2];
    uint64_t crc = 0;

    /* Serialize the object in an RDB-like format. It consist of an object type
     * byte followed by the serialized object. This is understood by RESTORE. */
    rioInitWithBuffer(payload,sdsempty());
    serverAssert(rdbSaveObjectType(payload,o));
    serverAssert(rdbSaveObject(payload,o,key,dbid));

    /* Write the footer, this is how it looks like:
     * ----------------+---------------------+---------------+
     * ... RDB payload | 2 bytes RDB version | 8 bytes CRC64 |
     * ----------------+---------------------+---------------+
     * RDB version and CRC are both in little endian.
     */

    /* RDB version */
    buf[0] = RDB_VERSION & 0xff;
    buf[1] = (RDB_VERSION >> 8) & 0xff;
    payload->io.buffer.ptr = sdscatlen(payload->io.buffer.ptr,buf,2);

    /* If crc checksum is disabled, crc is set to 0 and no checksum validation
     * will be performed on RESTORE. */
    if (!skip_checksum) {
        /* CRC64 */
        crc = crc64(0,(unsigned char*)payload->io.buffer.ptr,
                    sdslen(payload->io.buffer.ptr));
        memrev64ifbe(&crc);
    }
    payload->io.buffer.ptr = sdscatlen(payload->io.buffer.ptr,&crc,8);
}

/* Verify that the RDB version of the dump payload matches the one of this Redis
 * instance and that the checksum is ok.
 * If the DUMP payload looks valid C_OK is returned, otherwise C_ERR
 * is returned. If rdbver_ptr is not NULL, its populated with the value read
 * from the input buffer. */
int verifyDumpPayload(unsigned char *p, size_t len, uint16_t *rdbver_ptr) {
    unsigned char *footer;
    uint16_t rdbver;
    uint64_t crc;

    /* At least 2 bytes of RDB version and 8 of CRC64 should be present. */
    if (len < 10) return C_ERR;
    footer = p+(len-10);

    /* Set and verify RDB version. */
    rdbver = (footer[1] << 8) | footer[0];
    if (rdbver_ptr) {
        *rdbver_ptr = rdbver;
    }
    if (rdbver > RDB_VERSION) return C_ERR;

    if (server.skip_checksum_validation)
        return C_OK;

    uint64_t crc_payload;
    memcpy(&crc_payload, footer+2, 8);
    if (crc_payload == 0) /* No checksum. */
        return C_OK;

    /* Verify CRC64 */
    crc = crc64(0,p,len-8);
    memrev64ifbe(&crc);
    return crc == crc_payload ? C_OK : C_ERR;
}

/* DUMP keyname
 * DUMP is actually not used by Redis Cluster but it is the obvious
 * complement of RESTORE and can be useful for different applications. */
void dumpCommand(client *c) {
    kvobj *o;
    rio payload;

    /* Check if the key is here. */
    if ((o = lookupKeyRead(c->db,c->argv[1])) == NULL) {
        addReplyNull(c);
        return;
    }

    /* Create the DUMP encoded representation. */
    createDumpPayload(&payload,o,c->argv[1],c->db->id,0);

    /* Transfer to the client */
    addReplyBulkSds(c,payload.io.buffer.ptr);
    return;
}

/* RESTORE key ttl serialized-value [REPLACE] [ABSTTL] [IDLETIME seconds] [FREQ frequency] */
void restoreCommand(client *c) {
    long long ttl, lfu_freq = -1, lru_idle = -1, lru_clock = -1;
    rio payload;
    int j, type, replace = 0, absttl = 0;
    robj *obj;

    /* Parse additional options */
    for (j = 4; j < c->argc; j++) {
        int additional = c->argc-j-1;
        if (!strcasecmp(c->argv[j]->ptr,"replace")) {
            replace = 1;
        } else if (!strcasecmp(c->argv[j]->ptr,"absttl")) {
            absttl = 1;
        } else if (!strcasecmp(c->argv[j]->ptr,"idletime") && additional >= 1 &&
                   lfu_freq == -1)
        {
            if (getLongLongFromObjectOrReply(c,c->argv[j+1],&lru_idle,NULL)
                != C_OK) return;
            if (lru_idle < 0) {
                addReplyError(c,"Invalid IDLETIME value, must be >= 0");
                return;
            }
            lru_clock = LRU_CLOCK();
            j++; /* Consume additional arg. */
        } else if (!strcasecmp(c->argv[j]->ptr,"freq") && additional >= 1 &&
                   lru_idle == -1)
        {
            if (getLongLongFromObjectOrReply(c,c->argv[j+1],&lfu_freq,NULL)
                != C_OK) return;
            if (lfu_freq < 0 || lfu_freq > 255) {
                addReplyError(c,"Invalid FREQ value, must be >= 0 and <= 255");
                return;
            }
            j++; /* Consume additional arg. */
        } else {
            addReplyErrorObject(c,shared.syntaxerr);
            return;
        }
    }

    /* Make sure this key does not already exist here... */
    robj *key = c->argv[1];
    kvobj *oldval = lookupKeyWrite(c->db,key);
    int oldtype = oldval ? oldval->type : -1;
    if (!replace && oldval) {
        addReplyErrorObject(c,shared.busykeyerr);
        return;
    }

    /* Check if the TTL value makes sense */
    if (getLongLongFromObjectOrReply(c,c->argv[2],&ttl,NULL) != C_OK) {
        return;
    } else if (ttl < 0) {
        addReplyError(c,"Invalid TTL value, must be >= 0");
        return;
    }

    /* Verify RDB version and data checksum. */
    if (verifyDumpPayload(c->argv[3]->ptr,sdslen(c->argv[3]->ptr),NULL) == C_ERR)
    {
        addReplyError(c,"DUMP payload version or checksum are wrong");
        return;
    }

    rioInitWithBuffer(&payload,c->argv[3]->ptr);
    if (((type = rdbLoadObjectType(&payload)) == -1) ||
        ((obj = rdbLoadObject(type,&payload,key->ptr,c->db->id,NULL)) == NULL))
    {
        addReplyError(c,"Bad data format");
        return;
    }

    /* Remove the old key if needed. */
    int deleted = 0;
    if (replace)
        deleted = dbDelete(c->db,key);

    if (ttl && !absttl) ttl+=commandTimeSnapshot();
    if (ttl && checkAlreadyExpired(ttl)) {
        if (deleted) {
            robj *aux = server.lazyfree_lazy_server_del ? shared.unlink : shared.del;
            rewriteClientCommandVector(c, 2, aux, key);
            signalModifiedKey(c,c->db,key);
            notifyKeyspaceEvent(NOTIFY_GENERIC,"del",key,c->db->id);
            server.dirty++;
        }
        decrRefCount(obj);
        addReply(c, shared.ok);
        return;
    }

    /* Create the key and set the TTL if any */
    kvobj *kv = dbAddInternal(c->db, key, &obj, NULL, ttl ? ttl : -1);

    /* If minExpiredField was set, then the object is hash with expiration
     * on fields and need to register it in global HFE DS */
    if (kv->type == OBJ_HASH) {
        uint64_t minExpiredField = hashTypeGetMinExpire(kv, 1);
        if (minExpiredField != EB_EXPIRE_TIME_INVALID)
            estoreAdd(c->db->subexpires, getKeySlot(key->ptr), kv, minExpiredField);
    }

    if (ttl) {
        if (!absttl) {
            /* Propagate TTL as absolute timestamp */
            robj *ttl_obj = createStringObjectFromLongLong(ttl);
            rewriteClientCommandArgument(c,2,ttl_obj);
            decrRefCount(ttl_obj);
            rewriteClientCommandArgument(c,c->argc,shared.absttl);
        }
    }
    objectSetLRUOrLFU(kv, lfu_freq, lru_idle, lru_clock, 1000);
    signalModifiedKey(c,c->db,key);
    notifyKeyspaceEvent(NOTIFY_GENERIC,"restore",key,c->db->id);

    /* If we deleted a key that means REPLACE parameter was passed and the
     * destination key existed. */
    if (deleted) {
        notifyKeyspaceEvent(NOTIFY_OVERWRITTEN, "overwritten", key, c->db->id);
        if (oldtype != kv->type) {
            notifyKeyspaceEvent(NOTIFY_TYPE_CHANGED, "type_changed", key, c->db->id);
        }
    }
    addReply(c,shared.ok);
    server.dirty++;
}
/* MIGRATE socket cache implementation.
 *
 * We take a map between host:ip and a TCP socket that we used to connect
 * to this instance in recent time.
 * This sockets are closed when the max number we cache is reached, and also
 * in serverCron() when they are around for more than a few seconds. */
#define MIGRATE_SOCKET_CACHE_ITEMS 64 /* max num of items in the cache. */
#define MIGRATE_SOCKET_CACHE_TTL 10 /* close cached sockets after 10 sec. */

typedef struct migrateCachedSocket {
    connection *conn;
    long last_dbid;
    time_t last_use_time;
} migrateCachedSocket;

/* Return a migrateCachedSocket containing a TCP socket connected with the
 * target instance, possibly returning a cached one.
 *
 * This function is responsible of sending errors to the client if a
 * connection can't be established. In this case -1 is returned.
 * Otherwise on success the socket is returned, and the caller should not
 * attempt to free it after usage.
 *
 * If the caller detects an error while using the socket, migrateCloseSocket()
 * should be called so that the connection will be created from scratch
 * the next time. */
migrateCachedSocket* migrateGetSocket(client *c, robj *host, robj *port, long timeout) {
    connection *conn;
    sds name = sdsempty();
    migrateCachedSocket *cs;

    /* Check if we have an already cached socket for this ip:port pair. */
    name = sdscatlen(name,host->ptr,sdslen(host->ptr));
    name = sdscatlen(name,":",1);
    name = sdscatlen(name,port->ptr,sdslen(port->ptr));
    cs = dictFetchValue(server.migrate_cached_sockets,name);
    if (cs) {
        sdsfree(name);
        cs->last_use_time = server.unixtime;
        return cs;
    }

    /* No cached socket, create one. */
    if (dictSize(server.migrate_cached_sockets) == MIGRATE_SOCKET_CACHE_ITEMS) {
        /* Too many items, drop one at random. */
        dictEntry *de = dictGetRandomKey(server.migrate_cached_sockets);
        cs = dictGetVal(de);
        connClose(cs->conn);
        zfree(cs);
        dictDelete(server.migrate_cached_sockets,dictGetKey(de));
    }

    /* Create the connection */
    conn = connCreate(server.el, connTypeOfCluster());
    if (connBlockingConnect(conn, host->ptr, atoi(port->ptr), timeout)
        != C_OK) {
        addReplyError(c,"-IOERR error or timeout connecting to the client");
        connClose(conn);
        sdsfree(name);
        return NULL;
    }
    connEnableTcpNoDelay(conn);

    /* Add to the cache and return it to the caller. */
    cs = zmalloc(sizeof(*cs));
    cs->conn = conn;

    cs->last_dbid = -1;
    cs->last_use_time = server.unixtime;
    dictAdd(server.migrate_cached_sockets,name,cs);
    return cs;
}

/* Free a migrate cached connection. */
void migrateCloseSocket(robj *host, robj *port) {
    sds name = sdsempty();
    migrateCachedSocket *cs;

    name = sdscatlen(name,host->ptr,sdslen(host->ptr));
    name = sdscatlen(name,":",1);
    name = sdscatlen(name,port->ptr,sdslen(port->ptr));
    cs = dictFetchValue(server.migrate_cached_sockets,name);
    if (!cs) {
        sdsfree(name);
        return;
    }

    connClose(cs->conn);
    zfree(cs);
    dictDelete(server.migrate_cached_sockets,name);
    sdsfree(name);
}

void migrateCloseTimedoutSockets(void) {
    dictIterator di;
    dictEntry *de;

    dictInitSafeIterator(&di, server.migrate_cached_sockets);
    while((de = dictNext(&di)) != NULL) {
        migrateCachedSocket *cs = dictGetVal(de);

        if ((server.unixtime - cs->last_use_time) > MIGRATE_SOCKET_CACHE_TTL) {
            connClose(cs->conn);
            zfree(cs);
            dictDelete(server.migrate_cached_sockets,dictGetKey(de));
        }
    }
    dictResetIterator(&di);
}

/* MIGRATE host port key dbid timeout [COPY | REPLACE | AUTH password |
 *         AUTH2 username password]
 *
 * On in the multiple keys form:
 *
 * MIGRATE host port "" dbid timeout [COPY | REPLACE | AUTH password |
 *         AUTH2 username password] KEYS key1 key2 ... keyN */
void migrateCommand(client *c) {
    migrateCachedSocket *cs;
    int copy = 0, replace = 0, j;
    char *username = NULL;
    char *password = NULL;
    long timeout;
    long dbid;
    robj **kvArray = NULL; /* Objects to migrate. */
    robj **keyArray = NULL; /* Key names. */
    robj **newargv = NULL; /* Used to rewrite the command as DEL ... keys ... */
    rio cmd, payload;
    int may_retry = 1;
    int write_error = 0;
    int argv_rewritten = 0;

    /* To support the KEYS option we need the following additional state. */
    int first_key = 3; /* Argument index of the first key. */
    int num_keys = 1;  /* By default only migrate the 'key' argument. */

    /* Parse additional options */
    for (j = 6; j < c->argc; j++) {
        int moreargs = (c->argc-1) - j;
        if (!strcasecmp(c->argv[j]->ptr,"copy")) {
            copy = 1;
        } else if (!strcasecmp(c->argv[j]->ptr,"replace")) {
            replace = 1;
        } else if (!strcasecmp(c->argv[j]->ptr,"auth")) {
            if (!moreargs) {
                addReplyErrorObject(c,shared.syntaxerr);
                return;
            }
            j++;
            password = c->argv[j]->ptr;
            redactClientCommandArgument(c,j);
        } else if (!strcasecmp(c->argv[j]->ptr,"auth2")) {
            if (moreargs < 2) {
                addReplyErrorObject(c,shared.syntaxerr);
                return;
            }
            username = c->argv[++j]->ptr;
            redactClientCommandArgument(c,j);
            password = c->argv[++j]->ptr;
            redactClientCommandArgument(c,j);
        } else if (!strcasecmp(c->argv[j]->ptr,"keys")) {
            if (sdslen(c->argv[3]->ptr) != 0) {
                addReplyError(c,
                              "When using MIGRATE KEYS option, the key argument"
                              " must be set to the empty string");
                return;
            }
            first_key = j+1;
            num_keys = c->argc - j - 1;
            break; /* All the remaining args are keys. */
        } else {
            addReplyErrorObject(c,shared.syntaxerr);
            return;
        }
    }

    /* Sanity check */
    if (getLongFromObjectOrReply(c,c->argv[5],&timeout,NULL) != C_OK ||
        getLongFromObjectOrReply(c,c->argv[4],&dbid,NULL) != C_OK)
    {
        return;
    }
    if (timeout <= 0) timeout = 1000;

    /* Check if the keys are here. If at least one key is to migrate, do it
     * otherwise if all the keys are missing reply with "NOKEY" to signal
     * the caller there was nothing to migrate. We don't return an error in
     * this case, since often this is due to a normal condition like the key
     * expiring in the meantime. */
    kvArray = zrealloc(kvArray,sizeof(kvobj*)*num_keys);
    keyArray = zrealloc(keyArray,sizeof(robj*)*num_keys);
    int num_exists = 0;

    for (j = 0; j < num_keys; j++) {
        if ((kvArray[num_exists] = lookupKeyRead(c->db,c->argv[first_key+j])) != NULL) {
            keyArray[num_exists] = c->argv[first_key+j];
            num_exists++;
        }
    }
    num_keys = num_exists;
    if (num_keys == 0) {
        zfree(kvArray); zfree(keyArray);
        addReplySds(c,sdsnew("+NOKEY\r\n"));
        return;
    }

    try_again:
    write_error = 0;

    /* Connect */
    cs = migrateGetSocket(c,c->argv[1],c->argv[2],timeout);
    if (cs == NULL) {
        zfree(kvArray); zfree(keyArray);
        return; /* error sent to the client by migrateGetSocket() */
    }

    rioInitWithBuffer(&cmd,sdsempty());

    /* Authentication */
    if (password) {
        int arity = username ? 3 : 2;
        serverAssertWithInfo(c,NULL,rioWriteBulkCount(&cmd,'*',arity));
        serverAssertWithInfo(c,NULL,rioWriteBulkString(&cmd,"AUTH",4));
        if (username) {
            serverAssertWithInfo(c,NULL,rioWriteBulkString(&cmd,username,
                                                           sdslen(username)));
        }
        serverAssertWithInfo(c,NULL,rioWriteBulkString(&cmd,password,
                                                       sdslen(password)));
    }

    /* Send the SELECT command if the current DB is not already selected. */
    int select = cs->last_dbid != dbid; /* Should we emit SELECT? */
    if (select) {
        serverAssertWithInfo(c,NULL,rioWriteBulkCount(&cmd,'*',2));
        serverAssertWithInfo(c,NULL,rioWriteBulkString(&cmd,"SELECT",6));
        serverAssertWithInfo(c,NULL,rioWriteBulkLongLong(&cmd,dbid));
    }

    int non_expired = 0; /* Number of keys that we'll find non expired.
                            Note that serializing large keys may take some time
                            so certain keys that were found non expired by the
                            lookupKey() function, may be expired later. */

    /* Create RESTORE payload and generate the protocol to call the command. */
    for (j = 0; j < num_keys; j++) {
        long long ttl = 0;
        long long expireat = kvobjGetExpire(kvArray[j]);

        if (expireat != -1) {
            ttl = expireat-commandTimeSnapshot();
            if (ttl < 0) {
                continue;
            }
            if (ttl < 1) ttl = 1;
        }

        /* Relocate valid (non expired) keys and values into the array in successive
         * positions to remove holes created by the keys that were present
         * in the first lookup but are now expired after the second lookup. */
        kvArray[non_expired] = kvArray[j];
        keyArray[non_expired++] = keyArray[j];

        serverAssertWithInfo(c,NULL,
                             rioWriteBulkCount(&cmd,'*',replace ? 5 : 4));

        if (server.cluster_enabled)
            serverAssertWithInfo(c,NULL,
                                 rioWriteBulkString(&cmd,"RESTORE-ASKING",14));
        else
            serverAssertWithInfo(c,NULL,rioWriteBulkString(&cmd,"RESTORE",7));
        serverAssertWithInfo(c,NULL,sdsEncodedObject(keyArray[j]));
        serverAssertWithInfo(c,NULL,rioWriteBulkString(&cmd,keyArray[j]->ptr,
                                                       sdslen(keyArray[j]->ptr)));
        serverAssertWithInfo(c,NULL,rioWriteBulkLongLong(&cmd,ttl));

        /* Emit the payload argument, that is the serialized object using
         * the DUMP format. */
        createDumpPayload(&payload,kvArray[j],keyArray[j],dbid,0);
        serverAssertWithInfo(c,NULL,
                             rioWriteBulkString(&cmd,payload.io.buffer.ptr,
                                                sdslen(payload.io.buffer.ptr)));
        sdsfree(payload.io.buffer.ptr);

        /* Add the REPLACE option to the RESTORE command if it was specified
         * as a MIGRATE option. */
        if (replace)
            serverAssertWithInfo(c,NULL,rioWriteBulkString(&cmd,"REPLACE",7));
    }

    /* Fix the actual number of keys we are migrating. */
    num_keys = non_expired;

    /* Transfer the query to the other node in 64K chunks. */
    errno = 0;
    {
        sds buf = cmd.io.buffer.ptr;
        size_t pos = 0, towrite;
        int nwritten = 0;

        while ((towrite = sdslen(buf)-pos) > 0) {
            towrite = (towrite > (64*1024) ? (64*1024) : towrite);
            nwritten = connSyncWrite(cs->conn,buf+pos,towrite,timeout);
            if (nwritten != (signed)towrite) {
                write_error = 1;
                goto socket_err;
            }
            pos += nwritten;
        }
    }

    char buf0[1024]; /* Auth reply. */
    char buf1[1024]; /* Select reply. */
    char buf2[1024]; /* Restore reply. */

    /* Read the AUTH reply if needed. */
    if (password && connSyncReadLine(cs->conn, buf0, sizeof(buf0), timeout) <= 0)
        goto socket_err;

    /* Read the SELECT reply if needed. */
    if (select && connSyncReadLine(cs->conn, buf1, sizeof(buf1), timeout) <= 0)
        goto socket_err;

    /* Read the RESTORE replies. */
    int error_from_target = 0;
    int socket_error = 0;
    int del_idx = 1; /* Index of the key argument for the replicated DEL op. */

    /* Allocate the new argument vector that will replace the current command,
     * to propagate the MIGRATE as a DEL command (if no COPY option was given).
     * We allocate num_keys+1 because the additional argument is for "DEL"
     * command name itself. */
    if (!copy) newargv = zmalloc(sizeof(robj*)*(num_keys+1));

    for (j = 0; j < num_keys; j++) {
        if (connSyncReadLine(cs->conn, buf2, sizeof(buf2), timeout) <= 0) {
            socket_error = 1;
            break;
        }
        if ((password && buf0[0] == '-') ||
            (select && buf1[0] == '-') ||
            buf2[0] == '-')
        {
            /* On error assume that last_dbid is no longer valid. */
            if (!error_from_target) {
                cs->last_dbid = -1;
                char *errbuf;
                if (password && buf0[0] == '-') errbuf = buf0;
                else if (select && buf1[0] == '-') errbuf = buf1;
                else errbuf = buf2;

                error_from_target = 1;
                addReplyErrorFormat(c,"Target instance replied with error: %s",
                                    errbuf+1);
            }
        } else {
            if (!copy) {
                /* No COPY option: remove the local key, signal the change. */
                dbDelete(c->db,keyArray[j]);
                signalModifiedKey(c,c->db,keyArray[j]);
                notifyKeyspaceEvent(NOTIFY_GENERIC,"del",keyArray[j],c->db->id);
                server.dirty++;

                /* Populate the argument vector to replace the old one. */
                newargv[del_idx++] = keyArray[j];
                incrRefCount(keyArray[j]);
            }
        }
    }

    /* On socket error, if we want to retry, do it now before rewriting the
     * command vector. We only retry if we are sure nothing was processed
     * and we failed to read the first reply (j == 0 test). */
    if (!error_from_target && socket_error && j == 0 && may_retry &&
        errno != ETIMEDOUT)
    {
        goto socket_err; /* A retry is guaranteed because of tested conditions.*/
    }

    /* On socket errors, close the migration socket now that we still have
     * the original host/port in the ARGV. Later the original command may be
     * rewritten to DEL and will be too later. */
    if (socket_error) migrateCloseSocket(c->argv[1],c->argv[2]);

    if (!copy) {
        /* Translate MIGRATE as DEL for replication/AOF. Note that we do
         * this only for the keys for which we received an acknowledgement
         * from the receiving Redis server, by using the del_idx index. */
        if (del_idx > 1) {
            newargv[0] = createStringObject("DEL",3);
            /* Note that the following call takes ownership of newargv. */
            replaceClientCommandVector(c,del_idx,newargv);
            argv_rewritten = 1;
        } else {
            /* No key transfer acknowledged, no need to rewrite as DEL. */
            zfree(newargv);
        }
        newargv = NULL; /* Make it safe to call zfree() on it in the future. */
    }

    /* If we are here and a socket error happened, we don't want to retry.
     * Just signal the problem to the client, but only do it if we did not
     * already queue a different error reported by the destination server. */
    if (!error_from_target && socket_error) {
        may_retry = 0;
        goto socket_err;
    }

    if (!error_from_target) {
        /* Success! Update the last_dbid in migrateCachedSocket, so that we can
         * avoid SELECT the next time if the target DB is the same. Reply +OK.
         *
         * Note: If we reached this point, even if socket_error is true
         * still the SELECT command succeeded (otherwise the code jumps to
         * socket_err label. */
        cs->last_dbid = dbid;
        addReply(c,shared.ok);
    } else {
        /* On error we already sent it in the for loop above, and set
         * the currently selected socket to -1 to force SELECT the next time. */
    }

    sdsfree(cmd.io.buffer.ptr);
    zfree(kvArray); zfree(keyArray); zfree(newargv);
    return;

/* On socket errors we try to close the cached socket and try again.
 * It is very common for the cached socket to get closed, if just reopening
 * it works it's a shame to notify the error to the caller. */
    socket_err:
    /* Cleanup we want to perform in both the retry and no retry case.
     * Note: Closing the migrate socket will also force SELECT next time. */
    sdsfree(cmd.io.buffer.ptr);

    /* If the command was rewritten as DEL and there was a socket error,
     * we already closed the socket earlier. While migrateCloseSocket()
     * is idempotent, the host/port arguments are now gone, so don't do it
     * again. */
    if (!argv_rewritten) migrateCloseSocket(c->argv[1],c->argv[2]);
    zfree(newargv);
    newargv = NULL; /* This will get reallocated on retry. */

    /* Retry only if it's not a timeout and we never attempted a retry
     * (or the code jumping here did not set may_retry to zero). */
    if (errno != ETIMEDOUT && may_retry) {
        may_retry = 0;
        goto try_again;
    }

    /* Cleanup we want to do if no retry is attempted. */
    zfree(kvArray); zfree(keyArray);
    addReplyErrorSds(c, sdscatprintf(sdsempty(),
                                     "-IOERR error or timeout %s to target instance",
                                     write_error ? "writing" : "reading"));
    return;
}

/* Cluster node sanity check. Returns C_OK if the node id
 * is valid an C_ERR otherwise. */
int verifyClusterNodeId(const char *name, int length) {
    if (length != CLUSTER_NAMELEN) return C_ERR;
    for (int i = 0; i < length; i++) {
        if (name[i] >= 'a' && name[i] <= 'z') continue;
        if (name[i] >= '0' && name[i] <= '9') continue;
        return C_ERR;
    }
    return C_OK;
}

int isValidAuxChar(int c) {
    return isalnum(c) || (strchr("!#$%&()*+:;<>?@[]^{|}~", c) == NULL);
}

int isValidAuxString(char *s, unsigned int length) {
    for (unsigned i = 0; i < length; i++) {
        if (!isValidAuxChar(s[i])) return 0;
    }
    return 1;
}

void clusterCommandMyId(client *c) {
    char *name = clusterNodeGetName(getMyClusterNode());
    if (name) {
        addReplyBulkCBuffer(c,name, CLUSTER_NAMELEN);
    } else {
        addReplyError(c, "No ID yet");
    }
}

char* getMyClusterId(void) {
    return clusterNodeGetName(getMyClusterNode());
}

void clusterCommandMyShardId(client *c) {
    char *sid = clusterNodeGetShardId(getMyClusterNode());
    if (sid) {
        addReplyBulkCBuffer(c,sid, CLUSTER_NAMELEN);
    } else {
        addReplyError(c, "No shard ID yet");
    }
}

/* When a cluster command is called, we need to decide whether to return TLS info or
 * non-TLS info by the client's connection type. However if the command is called by
 * a Lua script or RM_call, there is no connection in the fake client, so we use
 * server.current_client here to get the real client if available. And if it is not
 * available (modules may call commands without a real client), we return the default
 * info, which is determined by server.tls_cluster. */
static int shouldReturnTlsInfo(void) {
    if (server.current_client && server.current_client->conn) {
        return connIsTLS(server.current_client->conn);
    } else {
        return server.tls_cluster;
    }
}

unsigned int countKeysInSlot(unsigned int slot) {
    return kvstoreDictSize(server.db->keys, slot);
}

/* Add detailed information of a node to the output buffer of the given client. */
void addNodeDetailsToShardReply(client *c, clusterNode *node) {

    int reply_count = 0;
    char *hostname;
    void *node_replylen = addReplyDeferredLen(c);

    addReplyBulkCString(c, "id");
    addReplyBulkCBuffer(c, clusterNodeGetName(node), CLUSTER_NAMELEN);
    reply_count++;

    if (clusterNodeTcpPort(node)) {
        addReplyBulkCString(c, "port");
        addReplyLongLong(c, clusterNodeTcpPort(node));
        reply_count++;
    }

    if (clusterNodeTlsPort(node)) {
        addReplyBulkCString(c, "tls-port");
        addReplyLongLong(c, clusterNodeTlsPort(node));
        reply_count++;
    }

    addReplyBulkCString(c, "ip");
    addReplyBulkCString(c, clusterNodeIp(node));
    reply_count++;

    addReplyBulkCString(c, "endpoint");
    addReplyBulkCString(c, clusterNodePreferredEndpoint(node));
    reply_count++;

    hostname = clusterNodeHostname(node);
    if (hostname != NULL && *hostname != '\0') {
        addReplyBulkCString(c, "hostname");
        addReplyBulkCString(c, hostname);
        reply_count++;
    }

    long long node_offset;
    if (clusterNodeIsMyself(node)) {
        node_offset = clusterNodeIsSlave(node) ? replicationGetSlaveOffset() : server.master_repl_offset;
    } else {
        node_offset = clusterNodeReplOffset(node);
    }

    addReplyBulkCString(c, "role");
    addReplyBulkCString(c, clusterNodeIsSlave(node) ? "replica" : "master");
    reply_count++;

    addReplyBulkCString(c, "replication-offset");
    addReplyLongLong(c, node_offset);
    reply_count++;

    addReplyBulkCString(c, "health");
    const char *health_msg = NULL;
    if (clusterNodeIsFailing(node)) {
        health_msg = "fail";
    } else if (clusterNodeIsSlave(node) && node_offset == 0) {
        health_msg = "loading";
    } else {
        health_msg = "online";
    }
    addReplyBulkCString(c, health_msg);
    reply_count++;

    setDeferredMapLen(c, node_replylen, reply_count);
}

static clusterNode *clusterGetMasterFromShard(void *shard_handle) {
    clusterNode *n = NULL;
    void *node_it = clusterShardHandleGetNodeIterator(shard_handle);
    while((n = clusterShardNodeIteratorNext(node_it)) != NULL) {
        if (!clusterNodeIsFailing(n)) {
            break;
        }
    }
    clusterShardNodeIteratorFree(node_it);
    if (!n) return NULL;
    return clusterNodeGetMaster(n);
}

/* Add the shard reply of a single shard based off the given primary node. */
void addShardReplyForClusterShards(client *c, void *shard_handle) {
    serverAssert(clusterGetShardNodeCount(shard_handle) > 0);
    addReplyMapLen(c, 2);
    addReplyBulkCString(c, "slots");

    /* Use slot_info_pairs from the primary only */
    clusterNode *master_node = clusterGetMasterFromShard(shard_handle);

    if (master_node && clusterNodeHasSlotInfo(master_node)) {
        serverAssert((clusterNodeSlotInfoCount(master_node) % 2) == 0);
        addReplyArrayLen(c, clusterNodeSlotInfoCount(master_node));
        for (int i = 0; i < clusterNodeSlotInfoCount(master_node); i++)
            addReplyLongLong(c, (unsigned long)clusterNodeSlotInfoEntry(master_node, i));
    } else {
        /* If no slot info pair is provided, the node owns no slots */
        addReplyArrayLen(c, 0);
    }

    addReplyBulkCString(c, "nodes");
    addReplyArrayLen(c, clusterGetShardNodeCount(shard_handle));
    void *node_it = clusterShardHandleGetNodeIterator(shard_handle);
    for (clusterNode *n = clusterShardNodeIteratorNext(node_it); n != NULL; n = clusterShardNodeIteratorNext(node_it)) {
        addNodeDetailsToShardReply(c, n);
        clusterFreeNodesSlotsInfo(n);
    }
    clusterShardNodeIteratorFree(node_it);
}

/* Add to the output buffer of the given client, an array of slot (start, end)
 * pair owned by the shard, also the primary and set of replica(s) along with
 * information about each node. */
void clusterCommandShards(client *c) {
    addReplyArrayLen(c, clusterGetShardCount());
    /* This call will add slot_info_pairs to all nodes */
    clusterGenNodesSlotsInfo(0);
    dictIterator *shard_it = clusterGetShardIterator();
    for(void *shard_handle = clusterNextShardHandle(shard_it); shard_handle != NULL; shard_handle = clusterNextShardHandle(shard_it)) {
        addShardReplyForClusterShards(c, shard_handle);
    }
    clusterFreeShardIterator(shard_it);
}

void clusterCommandHelp(client *c) {
    const char *help[] = {
            "COUNTKEYSINSLOT <slot>",
            "    Return the number of keys in <slot>.",
            "GETKEYSINSLOT <slot> <count>",
            "    Return key names stored by current node in a slot.",
            "INFO",
            "    Return information about the cluster.",
            "KEYSLOT <key>",
            "    Return the hash slot for <key>.",
            "MYID",
            "    Return the node id.",
            "MYSHARDID",
            "    Return the node's shard id.",
            "NODES",
            "    Return cluster configuration seen by node. Output format:",
            "    <id> <ip:port@bus-port[,hostname]> <flags> <master> <pings> <pongs> <epoch> <link> <slot> ...",
            "REPLICAS <node-id>",
            "    Return <node-id> replicas.",
            "SLOTS",
            "    Return information about slots range mappings. Each range is made of:",
            "    start, end, master and replicas IP addresses, ports and ids",
            "SLOT-STATS",
            "    Return an array of slot usage statistics for slots assigned to the current node.",
            "SHARDS",
            "    Return information about slot range mappings and the nodes associated with them.",
            NULL
    };

    addExtendedReplyHelp(c, help, clusterCommandExtendedHelp());
}

void clusterCommand(client *c) {
    if (server.cluster_enabled == 0) {
        addReplyError(c,"This instance has cluster support disabled");
        return;
    }

    if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"help")) {
        clusterCommandHelp(c);
    } else  if (!strcasecmp(c->argv[1]->ptr,"nodes") && c->argc == 2) {
        /* CLUSTER NODES */
        /* Report TLS ports to TLS client, and report non-TLS port to non-TLS client. */
        sds nodes = clusterGenNodesDescription(c, 0, shouldReturnTlsInfo());
        addReplyVerbatim(c,nodes,sdslen(nodes),"txt");
        sdsfree(nodes);
    } else if (!strcasecmp(c->argv[1]->ptr,"myid") && c->argc == 2) {
        /* CLUSTER MYID */
        clusterCommandMyId(c);
    } else if (!strcasecmp(c->argv[1]->ptr,"myshardid") && c->argc == 2) {
        /* CLUSTER MYSHARDID */
        clusterCommandMyShardId(c);
    } else if (!strcasecmp(c->argv[1]->ptr,"slots") && c->argc == 2) {
        /* CLUSTER SLOTS */
        clusterCommandSlots(c);
    } else if (!strcasecmp(c->argv[1]->ptr,"shards") && c->argc == 2) {
        /* CLUSTER SHARDS */
        clusterCommandShards(c);
    } else if (!strcasecmp(c->argv[1]->ptr,"info") && c->argc == 2) {
        /* CLUSTER INFO */

        sds info = genClusterInfoString();

        /* Produce the reply protocol. */
        addReplyVerbatim(c,info,sdslen(info),"txt");
        sdsfree(info);
    } else if (!strcasecmp(c->argv[1]->ptr,"keyslot") && c->argc == 3) {
        /* CLUSTER KEYSLOT <key> */
        sds key = c->argv[2]->ptr;

        addReplyLongLong(c,keyHashSlot(key,sdslen(key)));
    } else if (!strcasecmp(c->argv[1]->ptr,"countkeysinslot") && c->argc == 3) {
        /* CLUSTER COUNTKEYSINSLOT <slot> */
        long long slot;

        if (getLongLongFromObjectOrReply(c,c->argv[2],&slot,NULL) != C_OK)
            return;
        if (slot < 0 || slot >= CLUSTER_SLOTS) {
            addReplyError(c,"Invalid slot");
            return;
        }

        if (!clusterCanAccessKeysInSlot(slot)) {
            addReplyLongLong(c, 0);
            return;
        }
        addReplyLongLong(c,countKeysInSlot(slot));
    } else if (!strcasecmp(c->argv[1]->ptr,"getkeysinslot") && c->argc == 4) {
        /* CLUSTER GETKEYSINSLOT <slot> <count> */
        long long maxkeys, slot;

        if (getLongLongFromObjectOrReply(c,c->argv[2],&slot,NULL) != C_OK)
            return;
        if (getLongLongFromObjectOrReply(c,c->argv[3],&maxkeys,NULL)
            != C_OK)
            return;
        if (slot < 0 || slot >= CLUSTER_SLOTS || maxkeys < 0) {
            addReplyError(c,"Invalid slot or number of keys");
            return;
        }

        if (!clusterCanAccessKeysInSlot(slot)) {
            addReplyArrayLen(c, 0);
            return;
        }

        unsigned int keys_in_slot = countKeysInSlot(slot);
        unsigned int numkeys = maxkeys > keys_in_slot ? keys_in_slot : maxkeys;
        addReplyArrayLen(c,numkeys);
        kvstoreDictIterator *kvs_di = NULL;
        dictEntry *de = NULL;
        kvs_di = kvstoreGetDictIterator(server.db->keys, slot);
        for (unsigned int i = 0; i < numkeys; i++) {
            de = kvstoreDictIteratorNext(kvs_di);
            serverAssert(de != NULL);
            sds sdskey = kvobjGetKey(dictGetKV(de));
            addReplyBulkCBuffer(c, sdskey, sdslen(sdskey));
        }
        kvstoreReleaseDictIterator(kvs_di);
    } else if ((!strcasecmp(c->argv[1]->ptr,"slaves") ||
                !strcasecmp(c->argv[1]->ptr,"replicas")) && c->argc == 3) {
        /* CLUSTER SLAVES <NODE ID> */
        /* CLUSTER REPLICAS <NODE ID> */
        clusterNode *n = clusterLookupNode(c->argv[2]->ptr, sdslen(c->argv[2]->ptr));
        int j;

        /* Lookup the specified node in our table. */
        if (!n) {
            addReplyErrorFormat(c,"Unknown node %s", (char*)c->argv[2]->ptr);
            return;
        }

        if (clusterNodeIsSlave(n)) {
            addReplyError(c,"The specified node is not a master");
            return;
        }

        /* Report TLS ports to TLS client, and report non-TLS port to non-TLS client. */
        addReplyArrayLen(c, clusterNodeNumSlaves(n));
        for (j = 0; j < clusterNodeNumSlaves(n); j++) {
            sds ni = clusterGenNodeDescription(c, clusterNodeGetSlave(n, j), shouldReturnTlsInfo());
            addReplyBulkCString(c,ni);
            sdsfree(ni);
        }
    } else if(!clusterCommandSpecial(c)) {
        addReplySubcommandSyntaxError(c);
        return;
    }
}

/* Extract slot number from keys in a keys_result structure and return to caller.
 * Returns INVALID_CLUSTER_SLOT if keys belong to different slots (cross-slot error),
 * or if there are no keys.
 */
int extractSlotFromKeysResult(robj **argv, getKeysResult *keys_result) {
    if (keys_result->numkeys == 0)
        return INVALID_CLUSTER_SLOT;

    if (!server.cluster_enabled)
        return 0;

    int first_slot = INVALID_CLUSTER_SLOT;
    for (int j = 0; j < keys_result->numkeys; j++) {
        robj *this_key = argv[keys_result->keys[j].pos];
        int this_slot = (int)keyHashSlot((char*)this_key->ptr, sdslen(this_key->ptr));

        if (first_slot == INVALID_CLUSTER_SLOT)
            first_slot = this_slot;
        else if (first_slot != this_slot) {
            return INVALID_CLUSTER_SLOT;
        }
    }
    return first_slot;
}

/* Return the pointer to the cluster node that is able to serve the command.
 * For the function to succeed the command should only target either:
 *
 * 1) A single key (even multiple times like RPOPLPUSH mylist mylist).
 * 2) Multiple keys in the same hash slot, while the slot is stable (no
 *    resharding in progress).
 *
 * On success the function returns the node that is able to serve the request.
 * If the node is not 'myself' a redirection must be performed. The kind of
 * redirection is specified setting the integer passed by reference
 * 'error_code', which will be set to CLUSTER_REDIR_ASK or
 * CLUSTER_REDIR_MOVED.
 *
 * When the node is 'myself' 'error_code' is set to CLUSTER_REDIR_NONE.
 *
 * If the command fails NULL is returned, and the reason of the failure is
 * provided via 'error_code', which will be set to:
 *
 * CLUSTER_REDIR_CROSS_SLOT if the request contains multiple keys that
 * don't belong to the same hash slot.
 *
 * CLUSTER_REDIR_UNSTABLE if the request contains multiple keys
 * belonging to the same slot, but the slot is not stable (in migration or
 * importing state, likely because a resharding is in progress).
 *
 * CLUSTER_REDIR_DOWN_UNBOUND if the request addresses a slot which is
 * not bound to any node. In this case the cluster global state should be
 * already "down" but it is fragile to rely on the update of the global state,
 * so we also handle it here.
 *
 * CLUSTER_REDIR_DOWN_STATE and CLUSTER_REDIR_DOWN_RO_STATE if the cluster is
 * down but the user attempts to execute a command that addresses one or more keys. */
clusterNode *getNodeByQuery(client *c, struct redisCommand *cmd, robj **argv, int argc, int *hashslot,
    getKeysResult *keys_result, uint8_t read_error, uint64_t cmd_flags, int *error_code)
{
    clusterNode *myself = getMyClusterNode();
    clusterNode *n = NULL;
    robj *firstkey = NULL;
    int multiple_keys = 0;
    multiState *ms, _ms;
    pendingCommand mc;
    pendingCommand *mcp = &mc;
    int i, slot = 0, migrating_slot = 0, importing_slot = 0, missing_keys = 0,
            existing_keys = 0;
    int pubsubshard_included = 0; /* Flag to indicate if a pubsub shard cmd is included. */

    /* Allow any key to be set if a module disabled cluster redirections. */
    if (server.cluster_module_flags & CLUSTER_MODULE_FLAG_NO_REDIRECTION)
        return myself;

    /* Set error code optimistically for the base case. */
    if (error_code) *error_code = CLUSTER_REDIR_NONE;

    /* Modules can turn off Redis Cluster redirection: this is useful
     * when writing a module that implements a completely different
     * distributed system. */

    /* We handle all the cases as if they were EXEC commands, so we have
     * a common code path for everything */
    if (cmd->proc == execCommand) {
        /* If CLIENT_MULTI flag is not set EXEC is just going to return an
         * error. */
        if (!(c->flags & CLIENT_MULTI)) return myself;
        ms = &c->mstate;
    } else {
        /* In order to have a single codepath create a fake Multi State
         * structure if the client is not in MULTI/EXEC state, this way
         * we have a single codepath below. */
        ms = &_ms;
        _ms.commands = &mcp;
        _ms.count = 1;

        /* Properly initialize the fake pendingCommand */
        initPendingCommand(&mc);
        mc.argv = argv;
        mc.argc = argc;
        mc.cmd = cmd;
        mc.slot = hashslot ? *hashslot : INVALID_CLUSTER_SLOT;
        mc.read_error = read_error;
        if (keys_result) {
            mc.keys_result = *keys_result;
            mc.flags |= PENDING_CMD_KEYS_RESULT_VALID;
        }
    }

    /* Check that all the keys are in the same hash slot, and obtain this
     * slot and the node associated. */
    for (i = 0; i < ms->count; i++) {
        struct redisCommand *mcmd;
        robj **margv;
        int margc, j;
        keyReference *keyindex;

        pendingCommand *pcmd = ms->commands[i];

        mcmd = pcmd->cmd;
        margc = pcmd->argc;
        margv = pcmd->argv;

        /* Only valid for sharded pubsub as regular pubsub can operate on any node and bypasses this layer. */
        if (!pubsubshard_included &&
            doesCommandHaveChannelsWithFlags(mcmd, CMD_CHANNEL_PUBLISH | CMD_CHANNEL_SUBSCRIBE))
        {
            pubsubshard_included = 1;
        }

        /* If we have a cached keys result from preprocessCommand(), use it.
         * Otherwise, extract keys result. */
        int use_cache_keys_result = pcmd->flags & PENDING_CMD_KEYS_RESULT_VALID;
        getKeysResult result = GETKEYS_RESULT_INIT;
        if (use_cache_keys_result)
            result = pcmd->keys_result;
        else
            getKeysFromCommand(mcmd,margv,margc,&result);
        keyindex = result.keys;

        for (j = 0; j < result.numkeys; j++) {
            /* The command has keys and was checked for cross-slot between its keys in preprocessCommand() */
            if (pcmd->read_error == CLIENT_READ_CROSS_SLOT) {
                /* Error: multiple keys from different slots. */
                if (error_code)
                    *error_code = CLUSTER_REDIR_CROSS_SLOT;
                return NULL;
            }

            robj *thiskey = margv[keyindex[j].pos];
            int thisslot = pcmd->slot;
            if (thisslot == INVALID_CLUSTER_SLOT)
                thisslot = keyHashSlot((char*)thiskey->ptr, sdslen(thiskey->ptr));

            if (firstkey == NULL) {
                /* This is the first key we see. Check what is the slot
                 * and node. */
                firstkey = thiskey;
                slot = thisslot;
                n = getNodeBySlot(slot);

                /* Error: If a slot is not served, we are in "cluster down"
                 * state. However the state is yet to be updated, so this was
                 * not trapped earlier in processCommand(). Report the same
                 * error to the client. */
                if (n == NULL) {
                    if (!use_cache_keys_result) getKeysFreeResult(&result);
                    if (error_code)
                        *error_code = CLUSTER_REDIR_DOWN_UNBOUND;
                    return NULL;
                }

                /* If we are migrating or importing this slot, we need to check
                 * if we have all the keys in the request (the only way we
                 * can safely serve the request, otherwise we return a TRYAGAIN
                 * error). To do so we set the importing/migrating state and
                 * increment a counter for every missing key. */
                if (n == myself &&
                    getMigratingSlotDest(slot) != NULL)
                {
                    migrating_slot = 1;
                } else if (getImportingSlotSource(slot) != NULL) {
                    importing_slot = 1;
                }
            } else {
                /* If it is not the first key/channel, make sure it is exactly
                 * the same key/channel as the first we saw. */
                if (slot != thisslot) {
                    /* Error: multiple keys from different slots. */
                    if (!use_cache_keys_result) getKeysFreeResult(&result);
                    if (error_code)
                        *error_code = CLUSTER_REDIR_CROSS_SLOT;
                    return NULL;
                }
                if (importing_slot && !multiple_keys && !equalStringObjects(firstkey,thiskey)) {
                    /* Flag this request as one with multiple different
                     * keys/channels when the slot is in importing state. */
                    multiple_keys = 1;
                }
            }

            /* Migrating / Importing slot? Count keys we don't have.
             * If it is pubsubshard command, it isn't required to check
             * the channel being present or not in the node during the
             * slot migration, the channel will be served from the source
             * node until the migration completes with CLUSTER SETSLOT <slot>
             * NODE <node-id>. */
            int flags = LOOKUP_NOTOUCH | LOOKUP_NOSTATS | LOOKUP_NONOTIFY | LOOKUP_NOEXPIRE;
            if ((migrating_slot || importing_slot) && !pubsubshard_included)
            {
                if (lookupKeyReadWithFlags(&server.db[0], thiskey, flags) == NULL) missing_keys++;
                else existing_keys++;
            }
        }
        if (!use_cache_keys_result) getKeysFreeResult(&result);
    }

    /* No key at all in command? then we can serve the request
     * without redirections or errors in all the cases. */
    if (n == NULL) return myself;

    /* Cluster is globally down but we got keys? We only serve the request
     * if it is a read command and when allow_reads_when_down is enabled. */
    if (!isClusterHealthy()) {
        if (pubsubshard_included) {
            if (!server.cluster_allow_pubsubshard_when_down) {
                if (error_code) *error_code = CLUSTER_REDIR_DOWN_STATE;
                return NULL;
            }
        } else if (!server.cluster_allow_reads_when_down) {
            /* The cluster is configured to block commands when the
             * cluster is down. */
            if (error_code) *error_code = CLUSTER_REDIR_DOWN_STATE;
            return NULL;
        } else if (cmd_flags & CMD_WRITE) {
            /* The cluster is configured to allow read only commands */
            if (error_code) *error_code = CLUSTER_REDIR_DOWN_RO_STATE;
            return NULL;
        } else {
            /* Fall through and allow the command to be executed:
             * this happens when server.cluster_allow_reads_when_down is
             * true and the command is not a write command */
        }
    }

    /* Return the hashslot by reference. */
    if (hashslot) *hashslot = slot;

    /* MIGRATE always works in the context of the local node if the slot
     * is open (migrating or importing state). We need to be able to freely
     * move keys among instances in this case. */
    if ((migrating_slot || importing_slot) && cmd->proc == migrateCommand)
        return myself;

    /* If we don't have all the keys and we are migrating the slot, send
     * an ASK redirection or TRYAGAIN. */
    if (migrating_slot && missing_keys) {
        /* If we have keys but we don't have all keys, we return TRYAGAIN */
        if (existing_keys) {
            if (error_code) *error_code = CLUSTER_REDIR_UNSTABLE;
            return NULL;
        } else {
            if (error_code) *error_code = CLUSTER_REDIR_ASK;
            return getMigratingSlotDest(slot);
        }
    }

    /* If we are receiving the slot, and the client correctly flagged the
     * request as "ASKING", we can serve the request. However if the request
     * involves multiple keys and we don't have them all, the only option is
     * to send a TRYAGAIN error. */
    if (importing_slot &&
        (c->flags & CLIENT_ASKING || cmd_flags & CMD_ASKING))
    {
        if (multiple_keys && missing_keys) {
            if (error_code) *error_code = CLUSTER_REDIR_UNSTABLE;
            return NULL;
        } else {
            return myself;
        }
    }

    /* Handle the read-only client case reading from a slave: if this
     * node is a slave and the request is about a hash slot our master
     * is serving, we can reply without redirection. */
    int is_write_command = (cmd_flags & CMD_WRITE) ||
                           (c->cmd->proc == execCommand && (c->mstate.cmd_flags & CMD_WRITE));
    if (((c->flags & CLIENT_READONLY) || pubsubshard_included) &&
        !is_write_command &&
        clusterNodeIsSlave(myself) &&
        clusterNodeGetSlaveof(myself) == n)
    {
        return myself;
    }

    /* Base case: just return the right node. However, if this node is not
     * myself, set error_code to MOVED since we need to issue a redirection. */
    if (n != myself && error_code) *error_code = CLUSTER_REDIR_MOVED;
    return n;
}

/* Send the client the right redirection code, according to error_code
 * that should be set to one of CLUSTER_REDIR_* macros.
 *
 * If CLUSTER_REDIR_ASK or CLUSTER_REDIR_MOVED error codes
 * are used, then the node 'n' should not be NULL, but should be the
 * node we want to mention in the redirection. Moreover hashslot should
 * be set to the hash slot that caused the redirection. */
void clusterRedirectClient(client *c, clusterNode *n, int hashslot, int error_code) {
    if (error_code == CLUSTER_REDIR_CROSS_SLOT) {
        addReplyError(c,"-CROSSSLOT Keys in request don't hash to the same slot");
    } else if (error_code == CLUSTER_REDIR_UNSTABLE) {
        /* The request spawns multiple keys in the same slot,
         * but the slot is not "stable" currently as there is
         * a migration or import in progress. */
        addReplyError(c,"-TRYAGAIN Multiple keys request during rehashing of slot");
    } else if (error_code == CLUSTER_REDIR_DOWN_STATE) {
        addReplyError(c,"-CLUSTERDOWN The cluster is down");
    } else if (error_code == CLUSTER_REDIR_DOWN_RO_STATE) {
        addReplyError(c,"-CLUSTERDOWN The cluster is down and only accepts read commands");
    } else if (error_code == CLUSTER_REDIR_DOWN_UNBOUND) {
        addReplyError(c,"-CLUSTERDOWN Hash slot not served");
    } else if (error_code == CLUSTER_REDIR_MOVED ||
               error_code == CLUSTER_REDIR_ASK)
    {
        /* Report TLS ports to TLS client, and report non-TLS port to non-TLS client. */
        int port = clusterNodeClientPort(n, shouldReturnTlsInfo());
        addReplyErrorSds(c,sdscatprintf(sdsempty(),
                                        "-%s %d %s:%d",
                                        (error_code == CLUSTER_REDIR_ASK) ? "ASK" : "MOVED",
                                        hashslot, clusterNodePreferredEndpoint(n), port));
    } else {
        serverPanic("getNodeByQuery() unknown error.");
    }
}

/* This function is called by the function processing clients incrementally
 * to detect timeouts, in order to handle the following case:
 *
 * 1) A client blocks with BLPOP or similar blocking operation.
 * 2) The master migrates the hash slot elsewhere or turns into a slave.
 * 3) The client may remain blocked forever (or up to the max timeout time)
 *    waiting for a key change that will never happen.
 *
 * If the client is found to be blocked into a hash slot this node no
 * longer handles, the client is sent a redirection error, and the function
 * returns 1. Otherwise 0 is returned and no operation is performed. */
int clusterRedirectBlockedClientIfNeeded(client *c) {
    clusterNode *myself = getMyClusterNode();
    if (c->flags & CLIENT_BLOCKED &&
        (c->bstate.btype == BLOCKED_LIST ||
         c->bstate.btype == BLOCKED_ZSET ||
         c->bstate.btype == BLOCKED_STREAM ||
         c->bstate.btype == BLOCKED_MODULE))
    {
        dictEntry *de;
        dictIterator di;

        /* If the cluster is down, unblock the client with the right error.
         * If the cluster is configured to allow reads on cluster down, we
         * still want to emit this error since a write will be required
         * to unblock them which may never come.  */
        if (!isClusterHealthy()) {
            clusterRedirectClient(c,NULL,0,CLUSTER_REDIR_DOWN_STATE);
            return 1;
        }

        /* If the client is blocked on module, but not on a specific key,
         * don't unblock it (except for the CLUSTER_FAIL case above). */
        if (c->bstate.btype == BLOCKED_MODULE && !moduleClientIsBlockedOnKeys(c))
            return 0;

        /* All keys must belong to the same slot, so check first key only. */
        dictInitIterator(&di, c->bstate.keys);
        if ((de = dictNext(&di)) != NULL) {
            robj *key = dictGetKey(de);
            int slot = keyHashSlot((char*)key->ptr, sdslen(key->ptr));
            clusterNode *node = getNodeBySlot(slot);

            /* if the client is read-only and attempting to access key that our
             * replica can handle, allow it. */
            if ((c->flags & CLIENT_READONLY) &&
                !(c->lastcmd->flags & CMD_WRITE) &&
                clusterNodeIsSlave(myself) && clusterNodeGetSlaveof(myself) == node)
            {
                node = myself;
            }

            /* We send an error and unblock the client if:
             * 1) The slot is unassigned, emitting a cluster down error.
             * 2) The slot is not handled by this node, nor being imported. */
            if (node != myself && getImportingSlotSource(slot) == NULL)
            {
                if (node == NULL) {
                    clusterRedirectClient(c,NULL,0,
                                          CLUSTER_REDIR_DOWN_UNBOUND);
                } else {
                    clusterRedirectClient(c,node,slot,
                                          CLUSTER_REDIR_MOVED);
                }
                dictResetIterator(&di);
                return 1;
            }
        }
        dictResetIterator(&di);
    }
    return 0;
}

/* Returns an indication if the replica node is fully available
 * and should be listed in CLUSTER SLOTS response.
 * Returns 1 for available nodes, 0 for nodes that have
 * not finished their initial sync, in failed state, or are
 * otherwise considered not available to serve read commands. */
static int isReplicaAvailable(clusterNode *node) {
    if (clusterNodeIsFailing(node)) {
        return 0;
    }
    long long repl_offset = clusterNodeReplOffset(node);
    if (clusterNodeIsMyself(node)) {
        /* Nodes do not update their own information
         * in the cluster node list. */
        repl_offset = replicationGetSlaveOffset();
    }
    return (repl_offset != 0);
}

void addNodeToNodeReply(client *c, clusterNode *node) {
    char* hostname = clusterNodeHostname(node);
    addReplyArrayLen(c, 4);
    if (server.cluster_preferred_endpoint_type == CLUSTER_ENDPOINT_TYPE_IP) {
        addReplyBulkCString(c, clusterNodeIp(node));
    } else if (server.cluster_preferred_endpoint_type == CLUSTER_ENDPOINT_TYPE_HOSTNAME) {
        if (hostname != NULL && hostname[0] != '\0') {
            addReplyBulkCString(c, hostname);
        } else {
            addReplyBulkCString(c, "?");
        }
    } else if (server.cluster_preferred_endpoint_type == CLUSTER_ENDPOINT_TYPE_UNKNOWN_ENDPOINT) {
        addReplyNull(c);
    } else {
        serverPanic("Unrecognized preferred endpoint type");
    }

    /* Report TLS ports to TLS client, and report non-TLS port to non-TLS client. */
    addReplyLongLong(c, clusterNodeClientPort(node, shouldReturnTlsInfo()));
    addReplyBulkCBuffer(c, clusterNodeGetName(node), CLUSTER_NAMELEN);

    /* Add the additional endpoint information, this is all the known networking information
     * that is not the preferred endpoint. Note the logic is evaluated twice so we can
     * correctly report the number of additional network arguments without using a deferred
     * map, an assertion is made at the end to check we set the right length. */
    int length = 0;
    if (server.cluster_preferred_endpoint_type != CLUSTER_ENDPOINT_TYPE_IP) {
        length++;
    }
    if (server.cluster_preferred_endpoint_type != CLUSTER_ENDPOINT_TYPE_HOSTNAME
        && hostname != NULL && hostname[0] != '\0')
    {
        length++;
    }
    addReplyMapLen(c, length);

    if (server.cluster_preferred_endpoint_type != CLUSTER_ENDPOINT_TYPE_IP) {
        addReplyBulkCString(c, "ip");
        addReplyBulkCString(c, clusterNodeIp(node));
        length--;
    }
    if (server.cluster_preferred_endpoint_type != CLUSTER_ENDPOINT_TYPE_HOSTNAME
        && hostname != NULL && hostname[0] != '\0')
    {
        addReplyBulkCString(c, "hostname");
        addReplyBulkCString(c, hostname);
        length--;
    }
    serverAssert(length == 0);
}

void addNodeReplyForClusterSlot(client *c, clusterNode *node, int start_slot, int end_slot) {
    int i, nested_elements = 3; /* slots (2) + master addr (1) */
    for (i = 0; i < clusterNodeNumSlaves(node); i++) {
        if (!isReplicaAvailable(clusterNodeGetSlave(node, i))) continue;
        nested_elements++;
    }
    addReplyArrayLen(c, nested_elements);
    addReplyLongLong(c, start_slot);
    addReplyLongLong(c, end_slot);
    addNodeToNodeReply(c, node);

    /* Remaining nodes in reply are replicas for slot range */
    for (i = 0; i < clusterNodeNumSlaves(node); i++) {
        /* This loop is copy/pasted from clusterGenNodeDescription()
         * with modifications for per-slot node aggregation. */
        if (!isReplicaAvailable(clusterNodeGetSlave(node, i))) continue;
        addNodeToNodeReply(c, clusterNodeGetSlave(node, i));
        nested_elements--;
    }
    serverAssert(nested_elements == 3); /* Original 3 elements */
}

void clusterCommandSlots(client * c) {
    /* Format: 1) 1) start slot
     *            2) end slot
     *            3) 1) master IP
     *               2) master port
     *               3) node ID
     *            4) 1) replica IP
     *               2) replica port
     *               3) node ID
     *           ... continued until done
     */
    clusterNode *n = NULL;
    int num_masters = 0, start = -1;
    void *slot_replylen = addReplyDeferredLen(c);

    for (int i = 0; i <= CLUSTER_SLOTS; i++) {
        /* Find start node and slot id. */
        if (n == NULL) {
            if (i == CLUSTER_SLOTS) break;
            n = getNodeBySlot(i);
            start = i;
            continue;
        }

        /* Add cluster slots info when occur different node with start
         * or end of slot. */
        if (i == CLUSTER_SLOTS || n != getNodeBySlot(i)) {
            addNodeReplyForClusterSlot(c, n, start, i-1);
            num_masters++;
            if (i == CLUSTER_SLOTS) break;
            n = getNodeBySlot(i);
            start = i;
        }
    }
    setDeferredArrayLen(c, slot_replylen, num_masters);
}

/* -----------------------------------------------------------------------------
 * Cluster functions related to serving / redirecting clients
 * -------------------------------------------------------------------------- */

/* The ASKING command is required after a -ASK redirection.
 * The client should issue ASKING before to actually send the command to
 * the target instance. See the Redis Cluster specification for more
 * information. */
void askingCommand(client *c) {
    if (server.cluster_enabled == 0) {
        addReplyError(c,"This instance has cluster support disabled");
        return;
    }
    c->flags |= CLIENT_ASKING;
    addReply(c,shared.ok);
}

/* The READONLY command is used by clients to enter the read-only mode.
 * In this mode slaves will not redirect clients as long as clients access
 * with read-only commands to keys that are served by the slave's master. */
void readonlyCommand(client *c) {
    if (server.cluster_enabled == 0) {
        addReplyError(c,"This instance has cluster support disabled");
        return;
    }
    c->flags |= CLIENT_READONLY;
    addReply(c,shared.ok);
}

/* Remove all the keys in the specified hash slot.
 * The number of removed items is returned. */
unsigned int clusterDelKeysInSlot(unsigned int hashslot, int by_command) {
    unsigned int j = 0;

    if (!kvstoreDictSize(server.db->keys, (int) hashslot))
        return 0;

    kvstoreDictIterator *kvs_di = NULL;
    dictEntry *de = NULL;
    kvs_di = kvstoreGetDictSafeIterator(server.db->keys, (int) hashslot);
    while((de = kvstoreDictIteratorNext(kvs_di)) != NULL) {
        enterExecutionUnit(1, 0);
        sds sdskey = kvobjGetKey(dictGetKV(de));
        robj *key = createStringObject(sdskey, sdslen(sdskey));
        dbDelete(&server.db[0], key);

        signalModifiedKey(NULL, &server.db[0], key);
        if (by_command) {
            /* Keys are deleted by a command (trimslots), we need to notify the
             * keyspace event. Though, we don't need to propagate the DEL
             * command, as the command (trimslots) will be propagated. */
            notifyKeyspaceEvent(NOTIFY_GENERIC, "del", key, server.db[0].id);
        } else {
            /* Propagate the DEL command */
            propagateDeletion(&server.db[0], key, server.lazyfree_lazy_server_del);
            /* The keys are not actually logically deleted from the database,
             * just moved to another node. The modules needs to know that these
             * keys are no longer available locally, so just send the keyspace
             * notification to the modules, but not to clients. */
            moduleNotifyKeyspaceEvent(NOTIFY_GENERIC, "del", key, server.db[0].id);
        }
        exitExecutionUnit();
        postExecutionUnitOperations();
        decrRefCount(key);
        j++;
        server.dirty++;
    }
    kvstoreReleaseDictIterator(kvs_di);
    return j;
}

/* Delete the keys in the slot ranges. Returns the number of deleted items */
unsigned int clusterDelKeysInSlotRangeArray(slotRangeArray *slots, int by_command) {
    unsigned int j = 0;
    for (int i = 0; i < slots->num_ranges; i++) {
        for (int slot = slots->ranges[i].start; slot <= slots->ranges[i].end; slot++) {
            j += clusterDelKeysInSlot(slot, by_command);
        }
    }
    return j;
}

int clusterIsMySlot(int slot) {
    return getMyClusterNode() == getNodeBySlot(slot);
}

void replySlotsFlushAndFree(client *c, slotRangeArray *slots) {
    addReplyArrayLen(c, slots->num_ranges);
    for (int i = 0 ; i < slots->num_ranges ; i++) {
        addReplyArrayLen(c, 2);
        addReplyLongLong(c, slots->ranges[i].start);
        addReplyLongLong(c, slots->ranges[i].end);
    }
    slotRangeArrayFree(slots);
}

/* Checks that slot ranges are well-formed and non-overlapping. */
int validateSlotRanges(slotRangeArray *slots, sds *err) {
    unsigned char used_slots[CLUSTER_SLOTS] = {0};

    if (slots->num_ranges <= 0 || slots->num_ranges >= CLUSTER_SLOTS) {
        *err = sdscatprintf(sdsempty(), "invalid number of slot ranges: %d", slots->num_ranges);
        return C_ERR;
    }

    for (int i = 0; i < slots->num_ranges; i++) {
        if (slots->ranges[i].start >= CLUSTER_SLOTS ||
            slots->ranges[i].end >= CLUSTER_SLOTS)
        {
            *err = sdscatprintf(sdsempty(), "slot range is out of range: %d-%d",
                                slots->ranges[i].start, slots->ranges[i].end);
            return C_ERR;
        }

        if (slots->ranges[i].start > slots->ranges[i].end) {
            *err = sdscatprintf(sdsempty(), "start slot number %d is greater than end slot number %d",
                                slots->ranges[i].start, slots->ranges[i].end);
            return C_ERR;
        }

        for (int j = slots->ranges[i].start; j <= slots->ranges[i].end; j++) {
            if (used_slots[j]) {
                *err = sdscatprintf(sdsempty(), "Slot %d specified multiple times", j);
                return C_ERR;
            }
            used_slots[j]++;
        }
    }
    return C_OK;
}

/* Create a slot range array with the specified number of ranges. */
slotRangeArray *slotRangeArrayCreate(int num_ranges) {
    slotRangeArray *slots = zcalloc(sizeof(slotRangeArray) + num_ranges * sizeof(slotRange));
    slots->num_ranges = num_ranges;
    return slots;
}

/* Duplicate the slot range array. */
slotRangeArray *slotRangeArrayDup(slotRangeArray *slots) {
    slotRangeArray *dup = slotRangeArrayCreate(slots->num_ranges);
    memcpy(dup->ranges, slots->ranges, sizeof(slotRange) * slots->num_ranges);
    return dup;
}

/* Set the slot range at the specified index. */
void slotRangeArraySet(slotRangeArray *slots, int idx, int start, int end) {
    slots->ranges[idx].start = start;
    slots->ranges[idx].end = end;
}

/* Create a slot range string in the format of: "1000-2000 3000-4000 ..." */
sds slotRangeArrayToString(slotRangeArray *slots) {
    sds s = sdsempty();

    for (int i = 0; i < slots->num_ranges; i++) {
        slotRange *sr = &slots->ranges[i];
        s = sdscatprintf(s, "%d-%d ", sr->start, sr->end);
    }
    sdssetlen(s, sdslen(s) - 1);
    s[sdslen(s)] = '\0';

    return s;
}

/* Parse a slot range string in the format "1000-2000 3000-4000 ..." into a slotRangeArray.
 * Returns a new slotRangeArray on success, NULL on failure. */
slotRangeArray *slotRangeArrayFromString(sds data) {
    int num_ranges;
    long long start, end;
    slotRangeArray *slots = NULL;
    if (!data || sdslen(data) == 0) return NULL;

    sds *parts = sdssplitlen(data, sdslen(data), " ", 1, &num_ranges);
    if (num_ranges <= 0) goto err;

    slots = slotRangeArrayCreate(num_ranges);

    /* Parse each slot range */
    for (int i = 0; i < num_ranges; i++) {
        char *dash = strchr(parts[i], '-');
        if (!dash) goto err;

        if (string2ll(parts[i], dash - parts[i], &start) == 0 ||
            string2ll(dash + 1, sdslen(parts[i]) - (dash - parts[i]) - 1, &end) == 0)
            goto err;
        slotRangeArraySet(slots, i, start, end);
    }

    /* Validate all ranges */
    sds err_msg = NULL;
    if (validateSlotRanges(slots, &err_msg) != C_OK) {
        if (err_msg) sdsfree(err_msg);
        goto err;
    }
    sdsfreesplitres(parts, num_ranges);
    return slots;

err:
    if (slots) slotRangeArrayFree(slots);
    sdsfreesplitres(parts, num_ranges);
    return NULL;
}

static int compareSlotRange(const void *a, const void *b) {
    const slotRange *sa = a;
    const slotRange *sb = b;
    if (sa->start < sb->start) return -1;
    if (sa->start > sb->start) return 1;
    return 0;
}

/* Compare two slot range arrays, return 1 if equal, 0 otherwise */
int slotRangeArrayIsEqual(slotRangeArray *slots1, slotRangeArray *slots2) {
    if (slots1->num_ranges != slots2->num_ranges) return 0;

    /* Sort slot ranges first */
    qsort(slots1->ranges, slots1->num_ranges, sizeof(slotRange), compareSlotRange);
    qsort(slots2->ranges, slots2->num_ranges, sizeof(slotRange), compareSlotRange);

    for (int i = 0; i < slots1->num_ranges; i++) {
        if (slots1->ranges[i].start != slots2->ranges[i].start ||
            slots1->ranges[i].end != slots2->ranges[i].end) {
            return 0;
        }
    }
    return 1;
}

/* Add a slot to the slot range array.
 * Usage:
 *     slotRangeArray *slots = NULL
 *     slots = slotRangeArrayAppend(slots, 1000);
 *     slots = slotRangeArrayAppend(slots, 1001);
 *     slots = slotRangeArrayAppend(slots, 1003);
 *     slots = slotRangeArrayAppend(slots, 1004);
 *     slots = slotRangeArrayAppend(slots, 1005);
 *
 *     Result: 1000-1001, 1003-1005
 *     Note: `slot` must be greater than the previous slot.
 * */
slotRangeArray *slotRangeArrayAppend(slotRangeArray *slots, int slot) {
    if (slots == NULL) {
        slots = slotRangeArrayCreate(4);
        slots->ranges[0].start = slot;
        slots->ranges[0].end = slot;
        slots->num_ranges = 1;
        return slots;
    }

    serverAssert(slots->num_ranges >= 0 && slots->num_ranges <= CLUSTER_SLOTS);
    serverAssert(slot > slots->ranges[slots->num_ranges - 1].end);

    /* Check if we can extend the last range */
    slotRange *last = &slots->ranges[slots->num_ranges - 1];
    if (slot == last->end + 1) {
        last->end = slot;
        return slots;
    }

    /* Calculate current capacity and reallocate if needed */
    int cap = (int) ((zmalloc_size(slots) - sizeof(slotRangeArray)) / sizeof(slotRange));
    if (slots->num_ranges >= cap)
        slots = zrealloc(slots, sizeof(slotRangeArray) + sizeof(slotRange) * cap * 2);

    /* Add new single-slot range */
    slots->ranges[slots->num_ranges].start = slot;
    slots->ranges[slots->num_ranges].end = slot;
    slots->num_ranges++;

    return slots;
}

/* Returns 1 if the slot range array contains the given slot, 0 otherwise. */
int slotRangeArrayContains(slotRangeArray *slots, unsigned int slot) {
    for (int i = 0; i < slots->num_ranges; i++)
        if (slots->ranges[i].start <= slot && slots->ranges[i].end >= slot)
            return 1;
    return 0;
}

/* Free the slot range array. */
void slotRangeArrayFree(slotRangeArray *slots) {
    zfree(slots);
}

/* Generic version of slotRangeArrayFree(). */
void slotRangeArrayFreeGeneric(void *slots) {
    slotRangeArrayFree(slots);
}

/* Slot range array iterator */
slotRangeArrayIter *slotRangeArrayGetIterator(slotRangeArray *slots) {
    slotRangeArrayIter *it = zmalloc(sizeof(*it));
    it->slots = slots;
    it->range_index = 0;
    it->cur_slot = slots->num_ranges > 0 ? slots->ranges[0].start : -1;
    return it;
}

/* Returns the next slot in the array, or -1 if there are no more slots. */
int slotRangeArrayNext(slotRangeArrayIter *it) {
    if (it->range_index >= it->slots->num_ranges) return -1;

    if (it->cur_slot < it->slots->ranges[it->range_index].end) {
        it->cur_slot++;
    } else {
        it->range_index++;
        if (it->range_index < it->slots->num_ranges)
            it->cur_slot = it->slots->ranges[it->range_index].start;
        else
            it->cur_slot = -1; /* finished */
    }
    return it->cur_slot;
}

int slotRangeArrayGetCurrentSlot(slotRangeArrayIter *it) {
    return it->cur_slot;
}

void slotRangeArrayIteratorFree(slotRangeArrayIter *it) {
    zfree(it);
}

/* Parse slot ranges from the command arguments. Returns NULL on error. */
slotRangeArray *parseSlotRangesOrReply(client *c, int argc, int pos) {
    int start, end, count;
    slotRangeArray *slots;

    serverAssert(pos <= argc);
    serverAssert((argc - pos) % 2 == 0);

    count = (argc - pos) / 2;
    slots = slotRangeArrayCreate(count);
    slots->num_ranges = 0;

    for (int j = pos; j < argc; j += 2) {
        if ((start = getSlotOrReply(c, c->argv[j])) == -1 ||
            (end = getSlotOrReply(c, c->argv[j + 1])) == -1)
        {
            slotRangeArrayFree(slots);
            return NULL;
        }
        slotRangeArraySet(slots, slots->num_ranges, start, end);
        slots->num_ranges++;
    }

    sds err = NULL;
    if (validateSlotRanges(slots, &err) != C_OK) {
        addReplyErrorSds(c, err);
        slotRangeArrayFree(slots);
        return NULL;
    }
    return slots;
}

/* Return 1 if the keys in the slot can be accessed, 0 otherwise. */
int clusterCanAccessKeysInSlot(int slot) {
    /* If not in cluster mode, all keys are accessible */
    if (server.cluster_enabled == 0) return 1;

    /* If the slot is being imported under old slot migration approach, we should
     * allow to list keys from the slot as previously. */
    if (getImportingSlotSource(slot)) return 1;

    /* If using atomic slot migration, check if the slot belongs to the current
     * node or its master, return 1 if so. */
    clusterNode *myself = getMyClusterNode();
    if (clusterNodeIsSlave(myself)) {
        clusterNode *master = clusterNodeGetMaster(myself);
        if (master && clusterNodeCoversSlot(master, slot))
            return 1;
    } else {
        if (clusterNodeCoversSlot(myself, slot))
            return 1;
    }
    return 0;
}

/* Return the slot ranges that belong to the current node or its master. */
slotRangeArray *clusterGetLocalSlotRanges(void) {
    slotRangeArray *slots = NULL;

    if (!server.cluster_enabled) {
        slots = slotRangeArrayCreate(1);
        slotRangeArraySet(slots, 0, 0, CLUSTER_SLOTS - 1);
        return slots;
    }

    clusterNode *master = clusterNodeGetMaster(getMyClusterNode());
    if (master) {
        for (int i = 0; i < CLUSTER_SLOTS; i++) {
            if (clusterNodeCoversSlot(master, i))
                slots = slotRangeArrayAppend(slots, i);
        }
    }
    return slots ? slots : slotRangeArrayCreate(0);
}

/* Partially flush destination DB in a cluster node, based on the slot range.
 *
 * Usage: SFLUSH <start-slot> <end slot> [<start-slot> <end slot>]* [SYNC|ASYNC]
 *
 * This is an initial implementation of SFLUSH (slots flush) which is limited to
 * flushing a single shard as a whole, but in the future the same command may be
 * used to partially flush a shard based on hash slots. Currently only if provided
 * slots cover entirely the slots of a node, the node will be flushed and the
 * return value will be pairs of slot ranges. Otherwise, a single empty set will 
 * be returned. If possible, SFLUSH SYNC will be run as blocking ASYNC as an 
 * optimization.
 */
void sflushCommand(client *c) {
    int flags = EMPTYDB_NO_FLAGS, argc = c->argc;

    if (server.cluster_enabled == 0) {
        addReplyError(c,"This instance has cluster support disabled");
        return;
    }

    /* check if last argument is SYNC or ASYNC */
    if (!strcasecmp(c->argv[c->argc-1]->ptr,"sync")) {
        flags = EMPTYDB_NO_FLAGS;
        argc--;
    } else if (!strcasecmp(c->argv[c->argc-1]->ptr,"async")) {
        flags = EMPTYDB_ASYNC;
        argc--;
    } else if (server.lazyfree_lazy_user_flush) {
        flags = EMPTYDB_ASYNC;
    }

    /* parse the slot range */
    if (argc % 2 == 0) {
        addReplyErrorArity(c);
        return;
    }

    /* Parse slot ranges from the command arguments. */
    slotRangeArray *slots = parseSlotRangesOrReply(c, argc, 1);
    if (!slots) return;

    /* Iterate and find the slot ranges that belong to this node. Save them in
     * a new slotRangeArray. It is allocated on heap since there is a chance
     * that FLUSH SYNC will be running as blocking ASYNC and only later reply
     * with slot ranges */
    unsigned char slots_to_flush[CLUSTER_SLOTS] = {0}; /* Requested slots to flush */
    slotRangeArray *myslots = NULL;
    for (int i = 0; i < slots->num_ranges; i++) {
        for (int j = slots->ranges[i].start; j <= slots->ranges[i].end; j++) {
            if (clusterIsMySlot(j)) {
                myslots = slotRangeArrayAppend(myslots, j);
                slots_to_flush[j] = 1;
            }
        }
    }

    /* Verify that all slots of mynode got covered. See sflushCommand() comment. */
    int all_slots_covered = 1;
    for (int i = 0; i < CLUSTER_SLOTS; i++) {
        if (clusterIsMySlot(i) && !slots_to_flush[i]) {
            all_slots_covered = 0;
            break;
        }
    }
    if (myslots == NULL || !all_slots_covered) {
        addReplyArrayLen(c, 0);
        slotRangeArrayFree(slots);
        slotRangeArrayFree(myslots);
        return;
    }
    slotRangeArrayFree(slots);

    /* Flush selected slots. If not flush as blocking async, then reply immediately */
    if (flushCommandCommon(c, FLUSH_TYPE_SLOTS, flags, myslots) == 0)
        replySlotsFlushAndFree(c, myslots);
}

/* The READWRITE command just clears the READONLY command state. */
void readwriteCommand(client *c) {
    if (server.cluster_enabled == 0) {
        addReplyError(c,"This instance has cluster support disabled");
        return;
    }
    c->flags &= ~CLIENT_READONLY;
    addReply(c,shared.ok);
}

/* Resets transient cluster stats that we expose via INFO or other means that we want
 * to reset via CONFIG RESETSTAT. The function is also used in order to
 * initialize these fields in clusterInit() at server startup. */
void resetClusterStats(void) {
    if (!server.cluster_enabled) return;

    clusterSlotStatResetAll();
}
