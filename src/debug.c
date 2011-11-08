#include "redis.h"
#include "sha1.h"   /* SHA1 is used for DEBUG DIGEST */

#include <arpa/inet.h>

/* ================================= Debugging ============================== */

/* Compute the sha1 of string at 's' with 'len' bytes long.
 * The SHA1 is then xored againt the string pointed by digest.
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
 * against diget, also perform the digest of "digest" itself and
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
        di = dictGetIterator(db->dict);

        /* hash the DB id, so the same dataset moved in a different
         * DB will lead to a different digest */
        aux = htonl(j);
        mixDigest(final,&aux,sizeof(aux));

        /* Iterate this DB writing every entry */
        while((de = dictNext(di)) != NULL) {
            sds key;
            robj *keyobj, *o;
            time_t expiretime;

            memset(digest,0,20); /* This key-val digest */
            key = dictGetKey(de);
            keyobj = createStringObject(key,sdslen(key));

            mixDigest(digest,key,sdslen(key));

            /* Make sure the key is loaded if VM is active */
            o = dictGetVal(de);

            aux = htonl(o->type);
            mixDigest(digest,&aux,sizeof(aux));
            expiretime = getExpire(db,keyobj);

            /* Save the key and associated value */
            if (o->type == REDIS_STRING) {
                mixObjectDigest(digest,o);
            } else if (o->type == REDIS_LIST) {
                listTypeIterator *li = listTypeInitIterator(o,0,REDIS_TAIL);
                listTypeEntry entry;
                while(listTypeNext(li,&entry)) {
                    robj *eleobj = listTypeGet(&entry);
                    mixObjectDigest(digest,eleobj);
                    decrRefCount(eleobj);
                }
                listTypeReleaseIterator(li);
            } else if (o->type == REDIS_SET) {
                setTypeIterator *si = setTypeInitIterator(o);
                robj *ele;
                while((ele = setTypeNextObject(si)) != NULL) {
                    xorObjectDigest(digest,ele);
                    decrRefCount(ele);
                }
                setTypeReleaseIterator(si);
            } else if (o->type == REDIS_ZSET) {
                unsigned char eledigest[20];

                if (o->encoding == REDIS_ENCODING_ZIPLIST) {
                    unsigned char *zl = o->ptr;
                    unsigned char *eptr, *sptr;
                    unsigned char *vstr;
                    unsigned int vlen;
                    long long vll;
                    double score;

                    eptr = ziplistIndex(zl,0);
                    redisAssert(eptr != NULL);
                    sptr = ziplistNext(zl,eptr);
                    redisAssert(sptr != NULL);

                    while (eptr != NULL) {
                        redisAssert(ziplistGet(eptr,&vstr,&vlen,&vll));
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
                } else if (o->encoding == REDIS_ENCODING_SKIPLIST) {
                    zset *zs = o->ptr;
                    dictIterator *di = dictGetIterator(zs->dict);
                    dictEntry *de;

                    while((de = dictNext(di)) != NULL) {
                        robj *eleobj = dictGetKey(de);
                        double *score = dictGetVal(de);

                        snprintf(buf,sizeof(buf),"%.17g",*score);
                        memset(eledigest,0,20);
                        mixObjectDigest(eledigest,eleobj);
                        mixDigest(eledigest,buf,strlen(buf));
                        xorDigest(digest,eledigest,20);
                    }
                    dictReleaseIterator(di);
                } else {
                    redisPanic("Unknown sorted set encoding");
                }
            } else if (o->type == REDIS_HASH) {
                hashTypeIterator *hi;
                robj *obj;

                hi = hashTypeInitIterator(o);
                while (hashTypeNext(hi) != REDIS_ERR) {
                    unsigned char eledigest[20];

                    memset(eledigest,0,20);
                    obj = hashTypeCurrentObject(hi,REDIS_HASH_KEY);
                    mixObjectDigest(eledigest,obj);
                    decrRefCount(obj);
                    obj = hashTypeCurrentObject(hi,REDIS_HASH_VALUE);
                    mixObjectDigest(eledigest,obj);
                    decrRefCount(obj);
                    xorDigest(digest,eledigest,20);
                }
                hashTypeReleaseIterator(hi);
            } else {
                redisPanic("Unknown object type");
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

void debugCommand(redisClient *c) {
    if (!strcasecmp(c->argv[1]->ptr,"segfault")) {
        *((char*)-1) = 'x';
    } else if (!strcasecmp(c->argv[1]->ptr,"assert")) {
        if (c->argc >= 3) c->argv[2] = tryObjectEncoding(c->argv[2]);
        redisAssertWithInfo(c,c->argv[0],1 == 2);
    } else if (!strcasecmp(c->argv[1]->ptr,"reload")) {
        if (rdbSave(server.dbfilename) != REDIS_OK) {
            addReply(c,shared.err);
            return;
        }
        emptyDb();
        if (rdbLoad(server.dbfilename) != REDIS_OK) {
            addReplyError(c,"Error trying to load the RDB dump");
            return;
        }
        redisLog(REDIS_WARNING,"DB reloaded by DEBUG RELOAD");
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"loadaof")) {
        emptyDb();
        if (loadAppendOnlyFile(server.appendfilename) != REDIS_OK) {
            addReply(c,shared.err);
            return;
        }
        redisLog(REDIS_WARNING,"Append Only File loaded by DEBUG LOADAOF");
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

        addReplyStatusFormat(c,
            "Value at:%p refcount:%d "
            "encoding:%s serializedlength:%lld "
            "lru:%d lru_seconds_idle:%lu",
            (void*)val, val->refcount,
            strenc, (long long) rdbSavedObjectLen(val),
            val->lru, estimateObjectIdleTime(val));
    } else if (!strcasecmp(c->argv[1]->ptr,"populate") && c->argc == 3) {
        long keys, j;
        robj *key, *val;
        char buf[128];

        if (getLongFromObjectOrReply(c, c->argv[2], &keys, NULL) != REDIS_OK)
            return;
        for (j = 0; j < keys; j++) {
            snprintf(buf,sizeof(buf),"key:%lu",j);
            key = createStringObject(buf,strlen(buf));
            if (lookupKeyRead(c->db,key) != NULL) {
                decrRefCount(key);
                continue;
            }
            snprintf(buf,sizeof(buf),"value:%lu",j);
            val = createStringObject(buf,strlen(buf));
            dbAdd(c->db,key,val);
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

        usleep(utime);
        addReply(c,shared.ok);
    } else {
        addReplyError(c,
            "Syntax error, try DEBUG [SEGFAULT|OBJECT <key>|SWAPIN <key>|SWAPOUT <key>|RELOAD]");
    }
}

void _redisAssert(char *estr, char *file, int line) {
    redisLog(REDIS_WARNING,"=== ASSERTION FAILED ===");
    redisLog(REDIS_WARNING,"==> %s:%d '%s' is not true",file,line,estr);
#ifdef HAVE_BACKTRACE
    redisLog(REDIS_WARNING,"(forcing SIGSEGV in order to print the stack trace)");
    *((char*)-1) = 'x';
#endif
}

void _redisAssertPrintClientInfo(redisClient *c) {
    int j;

    redisLog(REDIS_WARNING,"=== ASSERTION FAILED CLIENT CONTEXT ===");
    redisLog(REDIS_WARNING,"client->flags = %d", c->flags);
    redisLog(REDIS_WARNING,"client->fd = %d", c->fd);
    redisLog(REDIS_WARNING,"client->argc = %d", c->argc);
    for (j=0; j < c->argc; j++) {
        char buf[128];
        char *arg;

        if (c->argv[j]->type == REDIS_STRING &&
            c->argv[j]->encoding == REDIS_ENCODING_RAW)
        {
            arg = (char*) c->argv[j]->ptr;
        } else {
            snprintf(buf,sizeof(buf),"Object type: %d, encoding: %d",
                c->argv[j]->type, c->argv[j]->encoding);
            arg = buf;
        }
        redisLog(REDIS_WARNING,"client->argv[%d] = \"%s\" (refcount: %d)",
            j, arg, c->argv[j]->refcount);
    }
}

void _redisAssertPrintObject(robj *o) {
    redisLog(REDIS_WARNING,"=== ASSERTION FAILED OBJECT CONTEXT ===");
    redisLog(REDIS_WARNING,"Object type: %d", o->type);
    redisLog(REDIS_WARNING,"Object encoding: %d", o->encoding);
    redisLog(REDIS_WARNING,"Object refcount: %d", o->refcount);
    if (o->type == REDIS_STRING && o->encoding == REDIS_ENCODING_RAW) {
        redisLog(REDIS_WARNING,"Object raw string len: %d", sdslen(o->ptr));
        if (sdslen(o->ptr) < 4096)
            redisLog(REDIS_WARNING,"Object raw string content: \"%s\"", (char*)o->ptr);
    }
}

void _redisAssertWithInfo(redisClient *c, robj *o, char *estr, char *file, int line) {
    if (c) _redisAssertPrintClientInfo(c);
    if (o) _redisAssertPrintObject(o);
    _redisAssert(estr,file,line);
}

void _redisPanic(char *msg, char *file, int line) {
    redisLog(REDIS_WARNING,"------------------------------------------------");
    redisLog(REDIS_WARNING,"!!! Software Failure. Press left mouse button to continue");
    redisLog(REDIS_WARNING,"Guru Meditation: %s #%s:%d",msg,file,line);
#ifdef HAVE_BACKTRACE
    redisLog(REDIS_WARNING,"(forcing SIGSEGV in order to print the stack trace)");
    redisLog(REDIS_WARNING,"------------------------------------------------");
    *((char*)-1) = 'x';
#endif
}
