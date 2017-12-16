/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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
#include "sha1.h"   /* SHA1 is used for DEBUG DIGEST */
#include "crc64.h"

#include <arpa/inet.h>
#include <signal.h>
#include <dlfcn.h>

#ifdef HAVE_BACKTRACE
#include <execinfo.h>
#include <ucontext.h>
#include <fcntl.h>
#include "bio.h"
#include <unistd.h>
#endif /* HAVE_BACKTRACE */

#ifdef __CYGWIN__
#ifndef SA_ONSTACK
#define SA_ONSTACK 0x08000000
#endif
#endif

/* ================================= Debugging ============================== */

/* Compute the sha1 of string at 's' with 'len' bytes long.
 * The SHA1 is then xored against the string pointed by digest.
 * Since xor is commutative, this operation is used in order to
 * "add" digests relative to unordered elements.
 *
 * So digest(a,b,c,d) will be the same of digest(b,a,c,d) */
void xorDigest(unsigned char *digest, void *ptr, size_t len) {
    SHA1_CTX ctx;
    unsigned char hash[20], *s = ptr;
    int j;

    SHA1Init(&ctx);
    SHA1Update(&ctx,s,len);
    SHA1Final(hash,&ctx);

    for (j = 0; j < 20; j++)
        digest[j] ^= hash[j];
}

void xorObjectDigest(unsigned char *digest, robj *o) {
    o = getDecodedObject(o);
    xorDigest(digest,o->ptr,sdslen(o->ptr));
    decrRefCount(o);
}

/* This function instead of just computing the SHA1 and xoring it
 * against digest, also perform the digest of "digest" itself and
 * replace the old value with the new one.
 *
 * So the final digest will be:
 *
 * digest = SHA1(digest xor SHA1(data))
 *
 * This function is used every time we want to preserve the order so
 * that digest(a,b,c,d) will be different than digest(b,c,d,a)
 *
 * Also note that mixdigest("foo") followed by mixdigest("bar")
 * will lead to a different digest compared to "fo", "obar".
 */
void mixDigest(unsigned char *digest, void *ptr, size_t len) {
    SHA1_CTX ctx;
    char *s = ptr;

    xorDigest(digest,s,len);
    SHA1Init(&ctx);
    SHA1Update(&ctx,digest,20);
    SHA1Final(digest,&ctx);
}

void mixObjectDigest(unsigned char *digest, robj *o) {
    o = getDecodedObject(o);
    mixDigest(digest,o->ptr,sdslen(o->ptr));
    decrRefCount(o);
}

/* Compute the dataset digest. Since keys, sets elements, hashes elements
 * are not ordered, we use a trick: every aggregate digest is the xor
 * of the digests of their elements. This way the order will not change
 * the result. For list instead we use a feedback entering the output digest
 * as input in order to ensure that a different ordered list will result in
 * a different digest. */
void computeDatasetDigest(unsigned char *final) {
    unsigned char digest[20];
    char buf[128];
    dictIterator *di = NULL;
    dictEntry *de;
    int j;
    uint32_t aux;

    memset(final,0,20); /* Start with a clean result */

    for (j = 0; j < server.dbnum; j++) {
        redisDb *db = server.db+j;

        if (dictSize(db->dict) == 0) continue;
        di = dictGetSafeIterator(db->dict);

        /* hash the DB id, so the same dataset moved in a different
         * DB will lead to a different digest */
        aux = htonl(j);
        mixDigest(final,&aux,sizeof(aux));

        /* Iterate this DB writing every entry */
        while((de = dictNext(di)) != NULL) {
            sds key;
            robj *keyobj, *o;
            long long expiretime;

            memset(digest,0,20); /* This key-val digest */
            key = dictGetKey(de);
            keyobj = createStringObject(key,sdslen(key));

            mixDigest(digest,key,sdslen(key));

            o = dictGetVal(de);

            aux = htonl(o->type);
            mixDigest(digest,&aux,sizeof(aux));
            expiretime = getExpire(db,keyobj);

            /* Save the key and associated value */
            if (o->type == OBJ_STRING) {
                mixObjectDigest(digest,o);
            } else if (o->type == OBJ_LIST) {
                listTypeIterator *li = listTypeInitIterator(o,0,LIST_TAIL);
                listTypeEntry entry;
                while(listTypeNext(li,&entry)) {
                    robj *eleobj = listTypeGet(&entry);
                    mixObjectDigest(digest,eleobj);
                    decrRefCount(eleobj);
                }
                listTypeReleaseIterator(li);
            } else if (o->type == OBJ_SET) {
                setTypeIterator *si = setTypeInitIterator(o);
                sds sdsele;
                while((sdsele = setTypeNextObject(si)) != NULL) {
                    xorDigest(digest,sdsele,sdslen(sdsele));
                    sdsfree(sdsele);
                }
                setTypeReleaseIterator(si);
            } else if (o->type == OBJ_ZSET) {
                unsigned char eledigest[20];

                if (o->encoding == OBJ_ENCODING_ZIPLIST) {
                    unsigned char *zl = o->ptr;
                    unsigned char *eptr, *sptr;
                    unsigned char *vstr;
                    unsigned int vlen;
                    long long vll;
                    double score;

                    eptr = ziplistIndex(zl,0);
                    serverAssert(eptr != NULL);
                    sptr = ziplistNext(zl,eptr);
                    serverAssert(sptr != NULL);

                    while (eptr != NULL) {
                        serverAssert(ziplistGet(eptr,&vstr,&vlen,&vll));
                        score = zzlGetScore(sptr);

                        memset(eledigest,0,20);
                        if (vstr != NULL) {
                            mixDigest(eledigest,vstr,vlen);
                        } else {
                            ll2string(buf,sizeof(buf),vll);
                            mixDigest(eledigest,buf,strlen(buf));
                        }

                        snprintf(buf,sizeof(buf),"%.17g",score);
                        mixDigest(eledigest,buf,strlen(buf));
                        xorDigest(digest,eledigest,20);
                        zzlNext(zl,&eptr,&sptr);
                    }
                } else if (o->encoding == OBJ_ENCODING_SKIPLIST) {
                    zset *zs = o->ptr;
                    dictIterator *di = dictGetIterator(zs->dict);
                    dictEntry *de;

                    while((de = dictNext(di)) != NULL) {
                        sds sdsele = dictGetKey(de);
                        double *score = dictGetVal(de);

                        snprintf(buf,sizeof(buf),"%.17g",*score);
                        memset(eledigest,0,20);
                        mixDigest(eledigest,sdsele,sdslen(sdsele));
                        mixDigest(eledigest,buf,strlen(buf));
                        xorDigest(digest,eledigest,20);
                    }
                    dictReleaseIterator(di);
                } else {
                    serverPanic("Unknown sorted set encoding");
                }
            } else if (o->type == OBJ_HASH) {
                hashTypeIterator *hi = hashTypeInitIterator(o);
                while (hashTypeNext(hi) != C_ERR) {
                    unsigned char eledigest[20];
                    sds sdsele;

                    memset(eledigest,0,20);
                    sdsele = hashTypeCurrentObjectNewSds(hi,OBJ_HASH_KEY);
                    mixDigest(eledigest,sdsele,sdslen(sdsele));
                    sdsfree(sdsele);
                    sdsele = hashTypeCurrentObjectNewSds(hi,OBJ_HASH_VALUE);
                    mixDigest(eledigest,sdsele,sdslen(sdsele));
                    sdsfree(sdsele);
                    xorDigest(digest,eledigest,20);
                }
                hashTypeReleaseIterator(hi);
            } else if (o->type == OBJ_STREAM) {
                streamIterator si;
                streamIteratorStart(&si,o->ptr,NULL,NULL,0);
                streamID id;
                int64_t numfields;

                while(streamIteratorGetID(&si,&id,&numfields)) {
                    sds itemid = sdscatfmt(sdsempty(),"%U.%U",id.ms,id.seq);
                    mixDigest(digest,itemid,sdslen(itemid));
                    sdsfree(itemid);

                    while(numfields--) {
                        unsigned char *field, *value;
                        int64_t field_len, value_len;
                        streamIteratorGetField(&si,&field,&value,
                                                   &field_len,&value_len);
                        mixDigest(digest,field,field_len);
                        mixDigest(digest,value,value_len);
                    }
                }
                streamIteratorStop(&si);
            } else if (o->type == OBJ_MODULE) {
                RedisModuleDigest md;
                moduleValue *mv = o->ptr;
                moduleType *mt = mv->type;
                moduleInitDigestContext(md);
                if (mt->digest) {
                    mt->digest(&md,mv->value);
                    xorDigest(digest,md.x,sizeof(md.x));
                }
            } else {
                serverPanic("Unknown object type");
            }
            /* If the key has an expire, add it to the mix */
            if (expiretime != -1) xorDigest(digest,"!!expire!!",10);
            /* We can finally xor the key-val digest to the final digest */
            xorDigest(final,digest,20);
            decrRefCount(keyobj);
        }
        dictReleaseIterator(di);
    }
}

void debugCommand(client *c) {
    if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"help")) {
        const char *help[] = {
"assert -- Crash by assertion failed.",
"change-repl-id -- Change the replication IDs of the instance. Dangerous, should be used only for testing the replication subsystem.",
"crash-and-recovery <milliseconds> -- Hard crash and restart after <milliseconds> delay.",
"digest -- Outputs an hex signature representing the current DB content.",
"htstats <dbid> -- Return hash table statistics of the specified Redis database.",
"loadaof -- Flush the AOF buffers on disk and reload the AOF in memory.",
"lua-always-replicate-commands (0|1) -- Setting it to 1 makes Lua replication defaulting to replicating single commands, without the script having to enable effects replication.",
"object <key> -- Show low level info about key and associated value.",
"panic -- Crash the server simulating a panic.",
"populate <count> [prefix] [size] -- Create <count> string keys named key:<num>. If a prefix is specified is used instead of the 'key' prefix.",
"reload -- Save the RDB on disk and reload it back in memory.",
"restart -- Graceful restart: save config, db, restart.",
"sdslen <key> -- Show low level SDS string info representing key and value.",
"segfault -- Crash the server with sigsegv.",
"set-active-expire (0|1) -- Setting it to 0 disables expiring keys in background when they are not accessed (otherwise the Redis behavior). Setting it to 1 reenables back the default.",
"sleep <seconds> -- Stop the server for <seconds>. Decimals allowed.",
"structsize -- Return the size of different Redis core C structures.",
"ziplist <key> -- Show low level info about the ziplist encoding.",
"error <string> -- Return a Redis protocol error with <string> as message. Useful for clients unit tests to simulate Redis errors.",
NULL
        };
        addReplyHelp(c, help);
    } else if (!strcasecmp(c->argv[1]->ptr,"segfault")) {
        *((char*)-1) = 'x';
    } else if (!strcasecmp(c->argv[1]->ptr,"panic")) {
        serverPanic("DEBUG PANIC called at Unix time %ld", time(NULL));
    } else if (!strcasecmp(c->argv[1]->ptr,"restart") ||
               !strcasecmp(c->argv[1]->ptr,"crash-and-recover"))
    {
        long long delay = 0;
        if (c->argc >= 3) {
            if (getLongLongFromObjectOrReply(c, c->argv[2], &delay, NULL)
                != C_OK) return;
            if (delay < 0) delay = 0;
        }
        int flags = !strcasecmp(c->argv[1]->ptr,"restart") ?
            (RESTART_SERVER_GRACEFULLY|RESTART_SERVER_CONFIG_REWRITE) :
             RESTART_SERVER_NONE;
        restartServer(flags,delay);
        addReplyError(c,"failed to restart the server. Check server logs.");
    } else if (!strcasecmp(c->argv[1]->ptr,"oom")) {
        void *ptr = zmalloc(ULONG_MAX); /* Should trigger an out of memory. */
        zfree(ptr);
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"assert")) {
        if (c->argc >= 3) c->argv[2] = tryObjectEncoding(c->argv[2]);
        serverAssertWithInfo(c,c->argv[0],1 == 2);
    } else if (!strcasecmp(c->argv[1]->ptr,"reload")) {
        rdbSaveInfo rsi, *rsiptr;
        rsiptr = rdbPopulateSaveInfo(&rsi);
        if (rdbSave(server.rdb_filename,rsiptr) != C_OK) {
            addReply(c,shared.err);
            return;
        }
        emptyDb(-1,EMPTYDB_NO_FLAGS,NULL);
        if (rdbLoad(server.rdb_filename,NULL) != C_OK) {
            addReplyError(c,"Error trying to load the RDB dump");
            return;
        }
        serverLog(LL_WARNING,"DB reloaded by DEBUG RELOAD");
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"loadaof")) {
        if (server.aof_state == AOF_ON) flushAppendOnlyFile(1);
        emptyDb(-1,EMPTYDB_NO_FLAGS,NULL);
        if (loadAppendOnlyFile(server.aof_filename) != C_OK) {
            addReply(c,shared.err);
            return;
        }
        server.dirty = 0; /* Prevent AOF / replication */
        serverLog(LL_WARNING,"Append Only File loaded by DEBUG LOADAOF");
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"object") && c->argc == 3) {
        dictEntry *de;
        robj *val;
        char *strenc;

        if ((de = dictFind(c->db->dict,c->argv[2]->ptr)) == NULL) {
            addReply(c,shared.nokeyerr);
            return;
        }
        val = dictGetVal(de);
        strenc = strEncoding(val->encoding);

        char extra[138] = {0};
        if (val->encoding == OBJ_ENCODING_QUICKLIST) {
            char *nextra = extra;
            int remaining = sizeof(extra);
            quicklist *ql = val->ptr;
            /* Add number of quicklist nodes */
            int used = snprintf(nextra, remaining, " ql_nodes:%lu", ql->len);
            nextra += used;
            remaining -= used;
            /* Add average quicklist fill factor */
            double avg = (double)ql->count/ql->len;
            used = snprintf(nextra, remaining, " ql_avg_node:%.2f", avg);
            nextra += used;
            remaining -= used;
            /* Add quicklist fill level / max ziplist size */
            used = snprintf(nextra, remaining, " ql_ziplist_max:%d", ql->fill);
            nextra += used;
            remaining -= used;
            /* Add isCompressed? */
            int compressed = ql->compress != 0;
            used = snprintf(nextra, remaining, " ql_compressed:%d", compressed);
            nextra += used;
            remaining -= used;
            /* Add total uncompressed size */
            unsigned long sz = 0;
            for (quicklistNode *node = ql->head; node; node = node->next) {
                sz += node->sz;
            }
            used = snprintf(nextra, remaining, " ql_uncompressed_size:%lu", sz);
            nextra += used;
            remaining -= used;
        }

        addReplyStatusFormat(c,
            "Value at:%p refcount:%d "
            "encoding:%s serializedlength:%zu "
            "lru:%d lru_seconds_idle:%llu%s",
            (void*)val, val->refcount,
            strenc, rdbSavedObjectLen(val),
            val->lru, estimateObjectIdleTime(val)/1000, extra);
    } else if (!strcasecmp(c->argv[1]->ptr,"sdslen") && c->argc == 3) {
        dictEntry *de;
        robj *val;
        sds key;

        if ((de = dictFind(c->db->dict,c->argv[2]->ptr)) == NULL) {
            addReply(c,shared.nokeyerr);
            return;
        }
        val = dictGetVal(de);
        key = dictGetKey(de);

        if (val->type != OBJ_STRING || !sdsEncodedObject(val)) {
            addReplyError(c,"Not an sds encoded string.");
        } else {
            addReplyStatusFormat(c,
                "key_sds_len:%lld, key_sds_avail:%lld, key_zmalloc: %lld, "
                "val_sds_len:%lld, val_sds_avail:%lld, val_zmalloc: %lld",
                (long long) sdslen(key),
                (long long) sdsavail(key),
                (long long) sdsZmallocSize(key),
                (long long) sdslen(val->ptr),
                (long long) sdsavail(val->ptr),
                (long long) getStringObjectSdsUsedMemory(val));
        }
    } else if (!strcasecmp(c->argv[1]->ptr,"ziplist") && c->argc == 3) {
        robj *o;

        if ((o = objectCommandLookupOrReply(c,c->argv[2],shared.nokeyerr))
                == NULL) return;

        if (o->encoding != OBJ_ENCODING_ZIPLIST) {
            addReplyError(c,"Not an sds encoded string.");
        } else {
            ziplistRepr(o->ptr);
            addReplyStatus(c,"Ziplist structure printed on stdout");
        }
    } else if (!strcasecmp(c->argv[1]->ptr,"populate") &&
               c->argc >= 3 && c->argc <= 5) {
        long keys, j;
        robj *key, *val;
        char buf[128];

        if (getLongFromObjectOrReply(c, c->argv[2], &keys, NULL) != C_OK)
            return;
        dictExpand(c->db->dict,keys);
        for (j = 0; j < keys; j++) {
            long valsize = 0;
            snprintf(buf,sizeof(buf),"%s:%lu",
                (c->argc == 3) ? "key" : (char*)c->argv[3]->ptr, j);
            key = createStringObject(buf,strlen(buf));
            if (c->argc == 5)
                if (getLongFromObjectOrReply(c, c->argv[4], &valsize, NULL) != C_OK)
                    return;
            if (lookupKeyWrite(c->db,key) != NULL) {
                decrRefCount(key);
                continue;
            }
            snprintf(buf,sizeof(buf),"value:%lu",j);
            if (valsize==0)
                val = createStringObject(buf,strlen(buf));
            else {
                int buflen = strlen(buf);
                val = createStringObject(NULL,valsize);
                memcpy(val->ptr, buf, valsize<=buflen? valsize: buflen);
            }
            dbAdd(c->db,key,val);
            signalModifiedKey(c->db,key);
            decrRefCount(key);
        }
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"digest") && c->argc == 2) {
        unsigned char digest[20];
        sds d = sdsempty();
        int j;

        computeDatasetDigest(digest);
        for (j = 0; j < 20; j++)
            d = sdscatprintf(d, "%02x",digest[j]);
        addReplyStatus(c,d);
        sdsfree(d);
    } else if (!strcasecmp(c->argv[1]->ptr,"sleep") && c->argc == 3) {
        double dtime = strtod(c->argv[2]->ptr,NULL);
        long long utime = dtime*1000000;
        struct timespec tv;

        tv.tv_sec = utime / 1000000;
        tv.tv_nsec = (utime % 1000000) * 1000;
        nanosleep(&tv, NULL);
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"set-active-expire") &&
               c->argc == 3)
    {
        server.active_expire_enabled = atoi(c->argv[2]->ptr);
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"lua-always-replicate-commands") &&
               c->argc == 3)
    {
        server.lua_always_replicate_commands = atoi(c->argv[2]->ptr);
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"error") && c->argc == 3) {
        sds errstr = sdsnewlen("-",1);

        errstr = sdscatsds(errstr,c->argv[2]->ptr);
        errstr = sdsmapchars(errstr,"\n\r","  ",2); /* no newlines in errors. */
        errstr = sdscatlen(errstr,"\r\n",2);
        addReplySds(c,errstr);
    } else if (!strcasecmp(c->argv[1]->ptr,"structsize") && c->argc == 2) {
        sds sizes = sdsempty();
        sizes = sdscatprintf(sizes,"bits:%d ",(sizeof(void*) == 8)?64:32);
        sizes = sdscatprintf(sizes,"robj:%d ",(int)sizeof(robj));
        sizes = sdscatprintf(sizes,"dictentry:%d ",(int)sizeof(dictEntry));
        sizes = sdscatprintf(sizes,"sdshdr5:%d ",(int)sizeof(struct sdshdr5));
        sizes = sdscatprintf(sizes,"sdshdr8:%d ",(int)sizeof(struct sdshdr8));
        sizes = sdscatprintf(sizes,"sdshdr16:%d ",(int)sizeof(struct sdshdr16));
        sizes = sdscatprintf(sizes,"sdshdr32:%d ",(int)sizeof(struct sdshdr32));
        sizes = sdscatprintf(sizes,"sdshdr64:%d ",(int)sizeof(struct sdshdr64));
        addReplyBulkSds(c,sizes);
    } else if (!strcasecmp(c->argv[1]->ptr,"htstats") && c->argc == 3) {
        long dbid;
        sds stats = sdsempty();
        char buf[4096];

        if (getLongFromObjectOrReply(c, c->argv[2], &dbid, NULL) != C_OK)
            return;
        if (dbid < 0 || dbid >= server.dbnum) {
            addReplyError(c,"Out of range database");
            return;
        }

        stats = sdscatprintf(stats,"[Dictionary HT]\n");
        dictGetStats(buf,sizeof(buf),server.db[dbid].dict);
        stats = sdscat(stats,buf);

        stats = sdscatprintf(stats,"[Expires HT]\n");
        dictGetStats(buf,sizeof(buf),server.db[dbid].expires);
        stats = sdscat(stats,buf);

        addReplyBulkSds(c,stats);
    } else if (!strcasecmp(c->argv[1]->ptr,"change-repl-id") && c->argc == 2) {
        serverLog(LL_WARNING,"Changing replication IDs after receiving DEBUG change-repl-id");
        changeReplicationId();
        clearReplicationId2();
        addReply(c,shared.ok);
    } else {
        addReplyErrorFormat(c, "Unknown subcommand or wrong number of arguments for '%s'. Try DEBUG HELP",
            (char*)c->argv[1]->ptr);
        return;
    }
}

/* =========================== Crash handling  ============================== */

void _serverAssert(const char *estr, const char *file, int line) {
    bugReportStart();
    serverLog(LL_WARNING,"=== ASSERTION FAILED ===");
    serverLog(LL_WARNING,"==> %s:%d '%s' is not true",file,line,estr);
#ifdef HAVE_BACKTRACE
    server.assert_failed = estr;
    server.assert_file = file;
    server.assert_line = line;
    serverLog(LL_WARNING,"(forcing SIGSEGV to print the bug report.)");
#endif
    *((char*)-1) = 'x';
}

void _serverAssertPrintClientInfo(const client *c) {
    int j;

    bugReportStart();
    serverLog(LL_WARNING,"=== ASSERTION FAILED CLIENT CONTEXT ===");
    serverLog(LL_WARNING,"client->flags = %d", c->flags);
    serverLog(LL_WARNING,"client->fd = %d", c->fd);
    serverLog(LL_WARNING,"client->argc = %d", c->argc);
    for (j=0; j < c->argc; j++) {
        char buf[128];
        char *arg;

        if (c->argv[j]->type == OBJ_STRING && sdsEncodedObject(c->argv[j])) {
            arg = (char*) c->argv[j]->ptr;
        } else {
            snprintf(buf,sizeof(buf),"Object type: %u, encoding: %u",
                c->argv[j]->type, c->argv[j]->encoding);
            arg = buf;
        }
        serverLog(LL_WARNING,"client->argv[%d] = \"%s\" (refcount: %d)",
            j, arg, c->argv[j]->refcount);
    }
}

void serverLogObjectDebugInfo(const robj *o) {
    serverLog(LL_WARNING,"Object type: %d", o->type);
    serverLog(LL_WARNING,"Object encoding: %d", o->encoding);
    serverLog(LL_WARNING,"Object refcount: %d", o->refcount);
    if (o->type == OBJ_STRING && sdsEncodedObject(o)) {
        serverLog(LL_WARNING,"Object raw string len: %zu", sdslen(o->ptr));
        if (sdslen(o->ptr) < 4096) {
            sds repr = sdscatrepr(sdsempty(),o->ptr,sdslen(o->ptr));
            serverLog(LL_WARNING,"Object raw string content: %s", repr);
            sdsfree(repr);
        }
    } else if (o->type == OBJ_LIST) {
        serverLog(LL_WARNING,"List length: %d", (int) listTypeLength(o));
    } else if (o->type == OBJ_SET) {
        serverLog(LL_WARNING,"Set size: %d", (int) setTypeSize(o));
    } else if (o->type == OBJ_HASH) {
        serverLog(LL_WARNING,"Hash size: %d", (int) hashTypeLength(o));
    } else if (o->type == OBJ_ZSET) {
        serverLog(LL_WARNING,"Sorted set size: %d", (int) zsetLength(o));
        if (o->encoding == OBJ_ENCODING_SKIPLIST)
            serverLog(LL_WARNING,"Skiplist level: %d", (int) ((const zset*)o->ptr)->zsl->level);
    }
}

void _serverAssertPrintObject(const robj *o) {
    bugReportStart();
    serverLog(LL_WARNING,"=== ASSERTION FAILED OBJECT CONTEXT ===");
    serverLogObjectDebugInfo(o);
}

void _serverAssertWithInfo(const client *c, const robj *o, const char *estr, const char *file, int line) {
    if (c) _serverAssertPrintClientInfo(c);
    if (o) _serverAssertPrintObject(o);
    _serverAssert(estr,file,line);
}

void _serverPanic(const char *file, int line, const char *msg, ...) {
    va_list ap;
    va_start(ap,msg);
    char fmtmsg[256];
    vsnprintf(fmtmsg,sizeof(fmtmsg),msg,ap);
    va_end(ap);

    bugReportStart();
    serverLog(LL_WARNING,"------------------------------------------------");
    serverLog(LL_WARNING,"!!! Software Failure. Press left mouse button to continue");
    serverLog(LL_WARNING,"Guru Meditation: %s #%s:%d",fmtmsg,file,line);
#ifdef HAVE_BACKTRACE
    serverLog(LL_WARNING,"(forcing SIGSEGV in order to print the stack trace)");
#endif
    serverLog(LL_WARNING,"------------------------------------------------");
    *((char*)-1) = 'x';
}

void bugReportStart(void) {
    if (server.bug_report_start == 0) {
        serverLogRaw(LL_WARNING|LL_RAW,
        "\n\n=== REDIS BUG REPORT START: Cut & paste starting from here ===\n");
        server.bug_report_start = 1;
    }
}

#ifdef HAVE_BACKTRACE
static void *getMcontextEip(ucontext_t *uc) {
#if defined(__APPLE__) && !defined(MAC_OS_X_VERSION_10_6)
    /* OSX < 10.6 */
    #if defined(__x86_64__)
    return (void*) uc->uc_mcontext->__ss.__rip;
    #elif defined(__i386__)
    return (void*) uc->uc_mcontext->__ss.__eip;
    #else
    return (void*) uc->uc_mcontext->__ss.__srr0;
    #endif
#elif defined(__APPLE__) && defined(MAC_OS_X_VERSION_10_6)
    /* OSX >= 10.6 */
    #if defined(_STRUCT_X86_THREAD_STATE64) && !defined(__i386__)
    return (void*) uc->uc_mcontext->__ss.__rip;
    #else
    return (void*) uc->uc_mcontext->__ss.__eip;
    #endif
#elif defined(__linux__)
    /* Linux */
    #if defined(__i386__)
    return (void*) uc->uc_mcontext.gregs[14]; /* Linux 32 */
    #elif defined(__X86_64__) || defined(__x86_64__)
    return (void*) uc->uc_mcontext.gregs[16]; /* Linux 64 */
    #elif defined(__ia64__) /* Linux IA64 */
    return (void*) uc->uc_mcontext.sc_ip;
    #elif defined(__arm__) /* Linux ARM */
    return (void*) uc->uc_mcontext.arm_pc;
    #endif
#else
    return NULL;
#endif
}

void logStackContent(void **sp) {
    int i;
    for (i = 15; i >= 0; i--) {
        unsigned long addr = (unsigned long) sp+i;
        unsigned long val = (unsigned long) sp[i];

        if (sizeof(long) == 4)
            serverLog(LL_WARNING, "(%08lx) -> %08lx", addr, val);
        else
            serverLog(LL_WARNING, "(%016lx) -> %016lx", addr, val);
    }
}

void logRegisters(ucontext_t *uc) {
    serverLog(LL_WARNING|LL_RAW, "\n------ REGISTERS ------\n");

/* OSX */
#if defined(__APPLE__) && defined(MAC_OS_X_VERSION_10_6)
  /* OSX AMD64 */
    #if defined(_STRUCT_X86_THREAD_STATE64) && !defined(__i386__)
    serverLog(LL_WARNING,
    "\n"
    "RAX:%016lx RBX:%016lx\nRCX:%016lx RDX:%016lx\n"
    "RDI:%016lx RSI:%016lx\nRBP:%016lx RSP:%016lx\n"
    "R8 :%016lx R9 :%016lx\nR10:%016lx R11:%016lx\n"
    "R12:%016lx R13:%016lx\nR14:%016lx R15:%016lx\n"
    "RIP:%016lx EFL:%016lx\nCS :%016lx FS:%016lx  GS:%016lx",
        (unsigned long) uc->uc_mcontext->__ss.__rax,
        (unsigned long) uc->uc_mcontext->__ss.__rbx,
        (unsigned long) uc->uc_mcontext->__ss.__rcx,
        (unsigned long) uc->uc_mcontext->__ss.__rdx,
        (unsigned long) uc->uc_mcontext->__ss.__rdi,
        (unsigned long) uc->uc_mcontext->__ss.__rsi,
        (unsigned long) uc->uc_mcontext->__ss.__rbp,
        (unsigned long) uc->uc_mcontext->__ss.__rsp,
        (unsigned long) uc->uc_mcontext->__ss.__r8,
        (unsigned long) uc->uc_mcontext->__ss.__r9,
        (unsigned long) uc->uc_mcontext->__ss.__r10,
        (unsigned long) uc->uc_mcontext->__ss.__r11,
        (unsigned long) uc->uc_mcontext->__ss.__r12,
        (unsigned long) uc->uc_mcontext->__ss.__r13,
        (unsigned long) uc->uc_mcontext->__ss.__r14,
        (unsigned long) uc->uc_mcontext->__ss.__r15,
        (unsigned long) uc->uc_mcontext->__ss.__rip,
        (unsigned long) uc->uc_mcontext->__ss.__rflags,
        (unsigned long) uc->uc_mcontext->__ss.__cs,
        (unsigned long) uc->uc_mcontext->__ss.__fs,
        (unsigned long) uc->uc_mcontext->__ss.__gs
    );
    logStackContent((void**)uc->uc_mcontext->__ss.__rsp);
    #else
    /* OSX x86 */
    serverLog(LL_WARNING,
    "\n"
    "EAX:%08lx EBX:%08lx ECX:%08lx EDX:%08lx\n"
    "EDI:%08lx ESI:%08lx EBP:%08lx ESP:%08lx\n"
    "SS:%08lx  EFL:%08lx EIP:%08lx CS :%08lx\n"
    "DS:%08lx  ES:%08lx  FS :%08lx GS :%08lx",
        (unsigned long) uc->uc_mcontext->__ss.__eax,
        (unsigned long) uc->uc_mcontext->__ss.__ebx,
        (unsigned long) uc->uc_mcontext->__ss.__ecx,
        (unsigned long) uc->uc_mcontext->__ss.__edx,
        (unsigned long) uc->uc_mcontext->__ss.__edi,
        (unsigned long) uc->uc_mcontext->__ss.__esi,
        (unsigned long) uc->uc_mcontext->__ss.__ebp,
        (unsigned long) uc->uc_mcontext->__ss.__esp,
        (unsigned long) uc->uc_mcontext->__ss.__ss,
        (unsigned long) uc->uc_mcontext->__ss.__eflags,
        (unsigned long) uc->uc_mcontext->__ss.__eip,
        (unsigned long) uc->uc_mcontext->__ss.__cs,
        (unsigned long) uc->uc_mcontext->__ss.__ds,
        (unsigned long) uc->uc_mcontext->__ss.__es,
        (unsigned long) uc->uc_mcontext->__ss.__fs,
        (unsigned long) uc->uc_mcontext->__ss.__gs
    );
    logStackContent((void**)uc->uc_mcontext->__ss.__esp);
    #endif
/* Linux */
#elif defined(__linux__)
    /* Linux x86 */
    #if defined(__i386__)
    serverLog(LL_WARNING,
    "\n"
    "EAX:%08lx EBX:%08lx ECX:%08lx EDX:%08lx\n"
    "EDI:%08lx ESI:%08lx EBP:%08lx ESP:%08lx\n"
    "SS :%08lx EFL:%08lx EIP:%08lx CS:%08lx\n"
    "DS :%08lx ES :%08lx FS :%08lx GS:%08lx",
        (unsigned long) uc->uc_mcontext.gregs[11],
        (unsigned long) uc->uc_mcontext.gregs[8],
        (unsigned long) uc->uc_mcontext.gregs[10],
        (unsigned long) uc->uc_mcontext.gregs[9],
        (unsigned long) uc->uc_mcontext.gregs[4],
        (unsigned long) uc->uc_mcontext.gregs[5],
        (unsigned long) uc->uc_mcontext.gregs[6],
        (unsigned long) uc->uc_mcontext.gregs[7],
        (unsigned long) uc->uc_mcontext.gregs[18],
        (unsigned long) uc->uc_mcontext.gregs[17],
        (unsigned long) uc->uc_mcontext.gregs[14],
        (unsigned long) uc->uc_mcontext.gregs[15],
        (unsigned long) uc->uc_mcontext.gregs[3],
        (unsigned long) uc->uc_mcontext.gregs[2],
        (unsigned long) uc->uc_mcontext.gregs[1],
        (unsigned long) uc->uc_mcontext.gregs[0]
    );
    logStackContent((void**)uc->uc_mcontext.gregs[7]);
    #elif defined(__X86_64__) || defined(__x86_64__)
    /* Linux AMD64 */
    serverLog(LL_WARNING,
    "\n"
    "RAX:%016lx RBX:%016lx\nRCX:%016lx RDX:%016lx\n"
    "RDI:%016lx RSI:%016lx\nRBP:%016lx RSP:%016lx\n"
    "R8 :%016lx R9 :%016lx\nR10:%016lx R11:%016lx\n"
    "R12:%016lx R13:%016lx\nR14:%016lx R15:%016lx\n"
    "RIP:%016lx EFL:%016lx\nCSGSFS:%016lx",
        (unsigned long) uc->uc_mcontext.gregs[13],
        (unsigned long) uc->uc_mcontext.gregs[11],
        (unsigned long) uc->uc_mcontext.gregs[14],
        (unsigned long) uc->uc_mcontext.gregs[12],
        (unsigned long) uc->uc_mcontext.gregs[8],
        (unsigned long) uc->uc_mcontext.gregs[9],
        (unsigned long) uc->uc_mcontext.gregs[10],
        (unsigned long) uc->uc_mcontext.gregs[15],
        (unsigned long) uc->uc_mcontext.gregs[0],
        (unsigned long) uc->uc_mcontext.gregs[1],
        (unsigned long) uc->uc_mcontext.gregs[2],
        (unsigned long) uc->uc_mcontext.gregs[3],
        (unsigned long) uc->uc_mcontext.gregs[4],
        (unsigned long) uc->uc_mcontext.gregs[5],
        (unsigned long) uc->uc_mcontext.gregs[6],
        (unsigned long) uc->uc_mcontext.gregs[7],
        (unsigned long) uc->uc_mcontext.gregs[16],
        (unsigned long) uc->uc_mcontext.gregs[17],
        (unsigned long) uc->uc_mcontext.gregs[18]
    );
    logStackContent((void**)uc->uc_mcontext.gregs[15]);
    #endif
#else
    serverLog(LL_WARNING,
        "  Dumping of registers not supported for this OS/arch");
#endif
}

/* Return a file descriptor to write directly to the Redis log with the
 * write(2) syscall, that can be used in critical sections of the code
 * where the rest of Redis can't be trusted (for example during the memory
 * test) or when an API call requires a raw fd.
 *
 * Close it with closeDirectLogFiledes(). */
int openDirectLogFiledes(void) {
    int log_to_stdout = server.logfile[0] == '\0';
    int fd = log_to_stdout ?
        STDOUT_FILENO :
        open(server.logfile, O_APPEND|O_CREAT|O_WRONLY, 0644);
    return fd;
}

/* Used to close what closeDirectLogFiledes() returns. */
void closeDirectLogFiledes(int fd) {
    int log_to_stdout = server.logfile[0] == '\0';
    if (!log_to_stdout) close(fd);
}

/* Logs the stack trace using the backtrace() call. This function is designed
 * to be called from signal handlers safely. */
void logStackTrace(ucontext_t *uc) {
    void *trace[101];
    int trace_size = 0, fd = openDirectLogFiledes();

    if (fd == -1) return; /* If we can't log there is anything to do. */

    /* Generate the stack trace */
    trace_size = backtrace(trace+1, 100);

    if (getMcontextEip(uc) != NULL) {
        char *msg1 = "EIP:\n";
        char *msg2 = "\nBacktrace:\n";
        if (write(fd,msg1,strlen(msg1)) == -1) {/* Avoid warning. */};
        trace[0] = getMcontextEip(uc);
        backtrace_symbols_fd(trace, 1, fd);
        if (write(fd,msg2,strlen(msg2)) == -1) {/* Avoid warning. */};
    }

    /* Write symbols to log file */
    backtrace_symbols_fd(trace+1, trace_size, fd);

    /* Cleanup */
    closeDirectLogFiledes(fd);
}

/* Log information about the "current" client, that is, the client that is
 * currently being served by Redis. May be NULL if Redis is not serving a
 * client right now. */
void logCurrentClient(void) {
    if (server.current_client == NULL) return;

    client *cc = server.current_client;
    sds client;
    int j;

    serverLogRaw(LL_WARNING|LL_RAW, "\n------ CURRENT CLIENT INFO ------\n");
    client = catClientInfoString(sdsempty(),cc);
    serverLog(LL_WARNING|LL_RAW,"%s\n", client);
    sdsfree(client);
    for (j = 0; j < cc->argc; j++) {
        robj *decoded;

        decoded = getDecodedObject(cc->argv[j]);
        serverLog(LL_WARNING|LL_RAW,"argv[%d]: '%s'\n", j,
            (char*)decoded->ptr);
        decrRefCount(decoded);
    }
    /* Check if the first argument, usually a key, is found inside the
     * selected DB, and if so print info about the associated object. */
    if (cc->argc >= 1) {
        robj *val, *key;
        dictEntry *de;

        key = getDecodedObject(cc->argv[1]);
        de = dictFind(cc->db->dict, key->ptr);
        if (de) {
            val = dictGetVal(de);
            serverLog(LL_WARNING,"key '%s' found in DB containing the following object:", (char*)key->ptr);
            serverLogObjectDebugInfo(val);
        }
        decrRefCount(key);
    }
}

#if defined(HAVE_PROC_MAPS)

#define MEMTEST_MAX_REGIONS 128

/* A non destructive memory test executed during segfauls. */
int memtest_test_linux_anonymous_maps(void) {
    FILE *fp;
    char line[1024];
    char logbuf[1024];
    size_t start_addr, end_addr, size;
    size_t start_vect[MEMTEST_MAX_REGIONS];
    size_t size_vect[MEMTEST_MAX_REGIONS];
    int regions = 0, j;

    int fd = openDirectLogFiledes();
    if (!fd) return 0;

    fp = fopen("/proc/self/maps","r");
    if (!fp) return 0;
    while(fgets(line,sizeof(line),fp) != NULL) {
        char *start, *end, *p = line;

        start = p;
        p = strchr(p,'-');
        if (!p) continue;
        *p++ = '\0';
        end = p;
        p = strchr(p,' ');
        if (!p) continue;
        *p++ = '\0';
        if (strstr(p,"stack") ||
            strstr(p,"vdso") ||
            strstr(p,"vsyscall")) continue;
        if (!strstr(p,"00:00")) continue;
        if (!strstr(p,"rw")) continue;

        start_addr = strtoul(start,NULL,16);
        end_addr = strtoul(end,NULL,16);
        size = end_addr-start_addr;

        start_vect[regions] = start_addr;
        size_vect[regions] = size;
        snprintf(logbuf,sizeof(logbuf),
            "*** Preparing to test memory region %lx (%lu bytes)\n",
                (unsigned long) start_vect[regions],
                (unsigned long) size_vect[regions]);
        if (write(fd,logbuf,strlen(logbuf)) == -1) { /* Nothing to do. */ }
        regions++;
    }

    int errors = 0;
    for (j = 0; j < regions; j++) {
        if (write(fd,".",1) == -1) { /* Nothing to do. */ }
        errors += memtest_preserving_test((void*)start_vect[j],size_vect[j],1);
        if (write(fd, errors ? "E" : "O",1) == -1) { /* Nothing to do. */ }
    }
    if (write(fd,"\n",1) == -1) { /* Nothing to do. */ }

    /* NOTE: It is very important to close the file descriptor only now
     * because closing it before may result into unmapping of some memory
     * region that we are testing. */
    fclose(fp);
    closeDirectLogFiledes(fd);
    return errors;
}
#endif

/* Scans the (assumed) x86 code starting at addr, for a max of `len`
 * bytes, searching for E8 (callq) opcodes, and dumping the symbols
 * and the call offset if they appear to be valid. */
void dumpX86Calls(void *addr, size_t len) {
    size_t j;
    unsigned char *p = addr;
    Dl_info info;
    /* Hash table to best-effort avoid printing the same symbol
     * multiple times. */
    unsigned long ht[256] = {0};

    if (len < 5) return;
    for (j = 0; j < len-4; j++) {
        if (p[j] != 0xE8) continue; /* Not an E8 CALL opcode. */
        unsigned long target = (unsigned long)addr+j+5;
        target += *((int32_t*)(p+j+1));
        if (dladdr((void*)target, &info) != 0 && info.dli_sname != NULL) {
            if (ht[target&0xff] != target) {
                printf("Function at 0x%lx is %s\n",target,info.dli_sname);
                ht[target&0xff] = target;
            }
            j += 4; /* Skip the 32 bit immediate. */
        }
    }
}

void sigsegvHandler(int sig, siginfo_t *info, void *secret) {
    ucontext_t *uc = (ucontext_t*) secret;
    void *eip = getMcontextEip(uc);
    sds infostring, clients;
    struct sigaction act;
    UNUSED(info);

    bugReportStart();
    serverLog(LL_WARNING,
        "Redis %s crashed by signal: %d", REDIS_VERSION, sig);
    if (eip != NULL) {
        serverLog(LL_WARNING,
        "Crashed running the instuction at: %p", eip);
    }
    if (sig == SIGSEGV || sig == SIGBUS) {
        serverLog(LL_WARNING,
        "Accessing address: %p", (void*)info->si_addr);
    }
    serverLog(LL_WARNING,
        "Failed assertion: %s (%s:%d)", server.assert_failed,
                        server.assert_file, server.assert_line);

    /* Log the stack trace */
    serverLogRaw(LL_WARNING|LL_RAW, "\n------ STACK TRACE ------\n");
    logStackTrace(uc);

    /* Log INFO and CLIENT LIST */
    serverLogRaw(LL_WARNING|LL_RAW, "\n------ INFO OUTPUT ------\n");
    infostring = genRedisInfoString("all");
    serverLogRaw(LL_WARNING|LL_RAW, infostring);
    serverLogRaw(LL_WARNING|LL_RAW, "\n------ CLIENT LIST OUTPUT ------\n");
    clients = getAllClientsInfoString();
    serverLogRaw(LL_WARNING|LL_RAW, clients);
    sdsfree(infostring);
    sdsfree(clients);

    /* Log the current client */
    logCurrentClient();

    /* Log dump of processor registers */
    logRegisters(uc);

#if defined(HAVE_PROC_MAPS)
    /* Test memory */
    serverLogRaw(LL_WARNING|LL_RAW, "\n------ FAST MEMORY TEST ------\n");
    bioKillThreads();
    if (memtest_test_linux_anonymous_maps()) {
        serverLogRaw(LL_WARNING|LL_RAW,
            "!!! MEMORY ERROR DETECTED! Check your memory ASAP !!!\n");
    } else {
        serverLogRaw(LL_WARNING|LL_RAW,
            "Fast memory test PASSED, however your memory can still be broken. Please run a memory test for several hours if possible.\n");
    }
#endif

    if (eip != NULL) {
        Dl_info info;
        if (dladdr(eip, &info) != 0) {
            serverLog(LL_WARNING|LL_RAW,
                "\n------ DUMPING CODE AROUND EIP ------\n"
                "Symbol: %s (base: %p)\n"
                "Module: %s (base %p)\n"
                "$ xxd -r -p /tmp/dump.hex /tmp/dump.bin\n"
                "$ objdump --adjust-vma=%p -D -b binary -m i386:x86-64 /tmp/dump.bin\n"
                "------\n",
                info.dli_sname, info.dli_saddr, info.dli_fname, info.dli_fbase,
                info.dli_saddr);
            size_t len = (long)eip - (long)info.dli_saddr;
            unsigned long sz = sysconf(_SC_PAGESIZE);
            if (len < 1<<13) { /* we don't have functions over 8k (verified) */
                /* Find the address of the next page, which is our "safety"
                 * limit when dumping. Then try to dump just 128 bytes more
                 * than EIP if there is room, or stop sooner. */
                unsigned long next = ((unsigned long)eip + sz) & ~(sz-1);
                unsigned long end = (unsigned long)eip + 128;
                if (end > next) end = next;
                len = end - (unsigned long)info.dli_saddr;
                serverLogHexDump(LL_WARNING, "dump of function",
                    info.dli_saddr ,len);
                dumpX86Calls(info.dli_saddr,len);
            }
        }
    }

    serverLogRaw(LL_WARNING|LL_RAW,
"\n=== REDIS BUG REPORT END. Make sure to include from START to END. ===\n\n"
"       Please report the crash by opening an issue on github:\n\n"
"           http://github.com/antirez/redis/issues\n\n"
"  Suspect RAM error? Use redis-server --test-memory to verify it.\n\n"
);

    /* free(messages); Don't call free() with possibly corrupted memory. */
    if (server.daemonize && server.supervised == 0) unlink(server.pidfile);

    /* Make sure we exit with the right signal at the end. So for instance
     * the core will be dumped if enabled. */
    sigemptyset (&act.sa_mask);
    act.sa_flags = SA_NODEFER | SA_ONSTACK | SA_RESETHAND;
    act.sa_handler = SIG_DFL;
    sigaction (sig, &act, NULL);
    kill(getpid(),sig);
}
#endif /* HAVE_BACKTRACE */

/* ==================== Logging functions for debugging ===================== */

void serverLogHexDump(int level, char *descr, void *value, size_t len) {
    char buf[65], *b;
    unsigned char *v = value;
    char charset[] = "0123456789abcdef";

    serverLog(level,"%s (hexdump of %zu bytes):", descr, len);
    b = buf;
    while(len) {
        b[0] = charset[(*v)>>4];
        b[1] = charset[(*v)&0xf];
        b[2] = '\0';
        b += 2;
        len--;
        v++;
        if (b-buf == 64 || len == 0) {
            serverLogRaw(level|LL_RAW,buf);
            b = buf;
        }
    }
    serverLogRaw(level|LL_RAW,"\n");
}

/* =========================== Software Watchdog ============================ */
#include <sys/time.h>

void watchdogSignalHandler(int sig, siginfo_t *info, void *secret) {
#ifdef HAVE_BACKTRACE
    ucontext_t *uc = (ucontext_t*) secret;
#endif
    UNUSED(info);
    UNUSED(sig);

    serverLogFromHandler(LL_WARNING,"\n--- WATCHDOG TIMER EXPIRED ---");
#ifdef HAVE_BACKTRACE
    logStackTrace(uc);
#else
    serverLogFromHandler(LL_WARNING,"Sorry: no support for backtrace().");
#endif
    serverLogFromHandler(LL_WARNING,"--------\n");
}

/* Schedule a SIGALRM delivery after the specified period in milliseconds.
 * If a timer is already scheduled, this function will re-schedule it to the
 * specified time. If period is 0 the current timer is disabled. */
void watchdogScheduleSignal(int period) {
    struct itimerval it;

    /* Will stop the timer if period is 0. */
    it.it_value.tv_sec = period/1000;
    it.it_value.tv_usec = (period%1000)*1000;
    /* Don't automatically restart. */
    it.it_interval.tv_sec = 0;
    it.it_interval.tv_usec = 0;
    setitimer(ITIMER_REAL, &it, NULL);
}

/* Enable the software watchdog with the specified period in milliseconds. */
void enableWatchdog(int period) {
    int min_period;

    if (server.watchdog_period == 0) {
        struct sigaction act;

        /* Watchdog was actually disabled, so we have to setup the signal
         * handler. */
        sigemptyset(&act.sa_mask);
        act.sa_flags = SA_ONSTACK | SA_SIGINFO;
        act.sa_sigaction = watchdogSignalHandler;
        sigaction(SIGALRM, &act, NULL);
    }
    /* If the configured period is smaller than twice the timer period, it is
     * too short for the software watchdog to work reliably. Fix it now
     * if needed. */
    min_period = (1000/server.hz)*2;
    if (period < min_period) period = min_period;
    watchdogScheduleSignal(period); /* Adjust the current timer. */
    server.watchdog_period = period;
}

/* Disable the software watchdog. */
void disableWatchdog(void) {
    struct sigaction act;
    if (server.watchdog_period == 0) return; /* Already disabled. */
    watchdogScheduleSignal(0); /* Stop the current timer. */

    /* Set the signal handler to SIG_IGN, this will also remove pending
     * signals from the queue. */
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    act.sa_handler = SIG_IGN;
    sigaction(SIGALRM, &act, NULL);
    server.watchdog_period = 0;
}
