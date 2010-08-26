#include "redis.h"

/*-----------------------------------------------------------------------------
 * Set Commands
 *----------------------------------------------------------------------------*/

/* Factory method to return a set that *can* hold "value". When the object has
 * an integer-encodable value, an intset will be returned. Otherwise a regular
 * hash table. */
robj *setTypeCreate(robj *value) {
    if (isObjectRepresentableAsLongLong(value,NULL) == REDIS_OK)
        return createIntsetObject();
    return createSetObject();
}

int setTypeAdd(robj *subject, robj *value) {
    long long llval;
    if (subject->encoding == REDIS_ENCODING_HT) {
        if (dictAdd(subject->ptr,value,NULL) == DICT_OK) {
            incrRefCount(value);
            return 1;
        }
    } else if (subject->encoding == REDIS_ENCODING_INTSET) {
        if (isObjectRepresentableAsLongLong(value,&llval) == REDIS_OK) {
            uint8_t success = 0;
            subject->ptr = intsetAdd(subject->ptr,llval,&success);
            if (success) {
                /* Convert to regular set when the intset contains
                 * too many entries. */
                if (intsetLen(subject->ptr) > server.set_max_intset_entries)
                    setTypeConvert(subject,REDIS_ENCODING_HT);
                return 1;
            }
        } else {
            /* Failed to get integer from object, convert to regular set. */
            setTypeConvert(subject,REDIS_ENCODING_HT);

            /* The set *was* an intset and this value is not integer
             * encodable, so dictAdd should always work. */
            redisAssert(dictAdd(subject->ptr,value,NULL) == DICT_OK);
            incrRefCount(value);
            return 1;
        }
    } else {
        redisPanic("Unknown set encoding");
    }
    return 0;
}

int setTypeRemove(robj *subject, robj *value) {
    long long llval;
    if (subject->encoding == REDIS_ENCODING_HT) {
        if (dictDelete(subject->ptr,value) == DICT_OK) {
            if (htNeedsResize(subject->ptr)) dictResize(subject->ptr);
            return 1;
        }
    } else if (subject->encoding == REDIS_ENCODING_INTSET) {
        if (isObjectRepresentableAsLongLong(value,&llval) == REDIS_OK) {
            uint8_t success;
            subject->ptr = intsetRemove(subject->ptr,llval,&success);
            if (success) return 1;
        }
    } else {
        redisPanic("Unknown set encoding");
    }
    return 0;
}

int setTypeIsMember(robj *subject, robj *value) {
    long long llval;
    if (subject->encoding == REDIS_ENCODING_HT) {
        return dictFind((dict*)subject->ptr,value) != NULL;
    } else if (subject->encoding == REDIS_ENCODING_INTSET) {
        if (isObjectRepresentableAsLongLong(value,&llval) == REDIS_OK) {
            return intsetFind((intset*)subject->ptr,llval);
        }
    } else {
        redisPanic("Unknown set encoding");
    }
    return 0;
}

setTypeIterator *setTypeInitIterator(robj *subject) {
    setTypeIterator *si = zmalloc(sizeof(setTypeIterator));
    si->subject = subject;
    si->encoding = subject->encoding;
    if (si->encoding == REDIS_ENCODING_HT) {
        si->di = dictGetIterator(subject->ptr);
    } else if (si->encoding == REDIS_ENCODING_INTSET) {
        si->ii = 0;
    } else {
        redisPanic("Unknown set encoding");
    }
    return si;
}

void setTypeReleaseIterator(setTypeIterator *si) {
    if (si->encoding == REDIS_ENCODING_HT)
        dictReleaseIterator(si->di);
    zfree(si);
}

/* Move to the next entry in the set. Returns the object at the current
 * position, or NULL when the end is reached. This object will have its
 * refcount incremented, so the caller needs to take care of this. */
robj *setTypeNext(setTypeIterator *si) {
    robj *ret = NULL;
    if (si->encoding == REDIS_ENCODING_HT) {
        dictEntry *de = dictNext(si->di);
        if (de != NULL) {
            ret = dictGetEntryKey(de);
            incrRefCount(ret);
        }
    } else if (si->encoding == REDIS_ENCODING_INTSET) {
        int64_t llval;
        if (intsetGet(si->subject->ptr,si->ii++,&llval))
            ret = createStringObjectFromLongLong(llval);
    }
    return ret;
}


/* Return random element from set. The returned object will always have
 * an incremented refcount. */
robj *setTypeRandomElement(robj *subject) {
    robj *ret = NULL;
    if (subject->encoding == REDIS_ENCODING_HT) {
        dictEntry *de = dictGetRandomKey(subject->ptr);
        ret = dictGetEntryKey(de);
        incrRefCount(ret);
    } else if (subject->encoding == REDIS_ENCODING_INTSET) {
        long long llval = intsetRandom(subject->ptr);
        ret = createStringObjectFromLongLong(llval);
    } else {
        redisPanic("Unknown set encoding");
    }
    return ret;
}

unsigned long setTypeSize(robj *subject) {
    if (subject->encoding == REDIS_ENCODING_HT) {
        return dictSize((dict*)subject->ptr);
    } else if (subject->encoding == REDIS_ENCODING_INTSET) {
        return intsetLen((intset*)subject->ptr);
    } else {
        redisPanic("Unknown set encoding");
    }
}

/* Convert the set to specified encoding. The resulting dict (when converting
 * to a hashtable) is presized to hold the number of elements in the original
 * set. */
void setTypeConvert(robj *subject, int enc) {
    setTypeIterator *si;
    robj *element;
    redisAssert(subject->type == REDIS_SET);

    if (enc == REDIS_ENCODING_HT) {
        dict *d = dictCreate(&setDictType,NULL);
        /* Presize the dict to avoid rehashing */
        dictExpand(d,intsetLen(subject->ptr));

        /* setTypeGet returns a robj with incremented refcount */
        si = setTypeInitIterator(subject);
        while ((element = setTypeNext(si)) != NULL)
            redisAssert(dictAdd(d,element,NULL) == DICT_OK);
        setTypeReleaseIterator(si);

        subject->encoding = REDIS_ENCODING_HT;
        zfree(subject->ptr);
        subject->ptr = d;
    } else {
        redisPanic("Unsupported set conversion");
    }
}

void saddCommand(redisClient *c) {
    robj *set;

    set = lookupKeyWrite(c->db,c->argv[1]);
    if (set == NULL) {
        set = setTypeCreate(c->argv[2]);
        dbAdd(c->db,c->argv[1],set);
    } else {
        if (set->type != REDIS_SET) {
            addReply(c,shared.wrongtypeerr);
            return;
        }
    }
    if (setTypeAdd(set,c->argv[2])) {
        touchWatchedKey(c->db,c->argv[1]);
        server.dirty++;
        addReply(c,shared.cone);
    } else {
        addReply(c,shared.czero);
    }
}

void sremCommand(redisClient *c) {
    robj *set;

    if ((set = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,set,REDIS_SET)) return;

    if (setTypeRemove(set,c->argv[2])) {
        if (setTypeSize(set) == 0) dbDelete(c->db,c->argv[1]);
        touchWatchedKey(c->db,c->argv[1]);
        server.dirty++;
        addReply(c,shared.cone);
    } else {
        addReply(c,shared.czero);
    }
}

void smoveCommand(redisClient *c) {
    robj *srcset, *dstset, *ele;
    srcset = lookupKeyWrite(c->db,c->argv[1]);
    dstset = lookupKeyWrite(c->db,c->argv[2]);
    ele = c->argv[3];

    /* If the source key does not exist return 0 */
    if (srcset == NULL) {
        addReply(c,shared.czero);
        return;
    }

    /* If the source key has the wrong type, or the destination key
     * is set and has the wrong type, return with an error. */
    if (checkType(c,srcset,REDIS_SET) ||
        (dstset && checkType(c,dstset,REDIS_SET))) return;

    /* If srcset and dstset are equal, SMOVE is a no-op */
    if (srcset == dstset) {
        addReply(c,shared.cone);
        return;
    }

    /* If the element cannot be removed from the src set, return 0. */
    if (!setTypeRemove(srcset,ele)) {
        addReply(c,shared.czero);
        return;
    }

    /* Remove the src set from the database when empty */
    if (setTypeSize(srcset) == 0) dbDelete(c->db,c->argv[1]);
    touchWatchedKey(c->db,c->argv[1]);
    touchWatchedKey(c->db,c->argv[2]);
    server.dirty++;

    /* Create the destination set when it doesn't exist */
    if (!dstset) {
        dstset = setTypeCreate(ele);
        dbAdd(c->db,c->argv[2],dstset);
    }

    /* An extra key has changed when ele was successfully added to dstset */
    if (setTypeAdd(dstset,ele)) server.dirty++;
    addReply(c,shared.cone);
}

void sismemberCommand(redisClient *c) {
    robj *set;

    if ((set = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,set,REDIS_SET)) return;

    if (setTypeIsMember(set,c->argv[2]))
        addReply(c,shared.cone);
    else
        addReply(c,shared.czero);
}

void scardCommand(redisClient *c) {
    robj *o;

    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,REDIS_SET)) return;

    addReplyUlong(c,setTypeSize(o));
}

void spopCommand(redisClient *c) {
    robj *set, *ele;

    if ((set = lookupKeyWriteOrReply(c,c->argv[1],shared.nullbulk)) == NULL ||
        checkType(c,set,REDIS_SET)) return;

    ele = setTypeRandomElement(set);
    if (ele == NULL) {
        addReply(c,shared.nullbulk);
    } else {
        setTypeRemove(set,ele);
        addReplyBulk(c,ele);
        decrRefCount(ele);
        if (setTypeSize(set) == 0) dbDelete(c->db,c->argv[1]);
        touchWatchedKey(c->db,c->argv[1]);
        server.dirty++;
    }
}

void srandmemberCommand(redisClient *c) {
    robj *set, *ele;

    if ((set = lookupKeyReadOrReply(c,c->argv[1],shared.nullbulk)) == NULL ||
        checkType(c,set,REDIS_SET)) return;

    ele = setTypeRandomElement(set);
    if (ele == NULL) {
        addReply(c,shared.nullbulk);
    } else {
        addReplyBulk(c,ele);
        decrRefCount(ele);
    }
}

int qsortCompareSetsByCardinality(const void *s1, const void *s2) {
    return setTypeSize(*(robj**)s1)-setTypeSize(*(robj**)s2);
}

void sinterGenericCommand(redisClient *c, robj **setkeys, unsigned long setnum, robj *dstkey) {
    robj **sets = zmalloc(sizeof(robj*)*setnum);
    setTypeIterator *si;
    robj *ele, *lenobj = NULL, *dstset = NULL;
    unsigned long j, cardinality = 0;

    for (j = 0; j < setnum; j++) {
        robj *setobj = dstkey ?
            lookupKeyWrite(c->db,setkeys[j]) :
            lookupKeyRead(c->db,setkeys[j]);
        if (!setobj) {
            zfree(sets);
            if (dstkey) {
                if (dbDelete(c->db,dstkey)) {
                    touchWatchedKey(c->db,dstkey);
                    server.dirty++;
                }
                addReply(c,shared.czero);
            } else {
                addReply(c,shared.emptymultibulk);
            }
            return;
        }
        if (checkType(c,setobj,REDIS_SET)) {
            zfree(sets);
            return;
        }
        sets[j] = setobj;
    }
    /* Sort sets from the smallest to largest, this will improve our
     * algorithm's performace */
    qsort(sets,setnum,sizeof(robj*),qsortCompareSetsByCardinality);

    /* The first thing we should output is the total number of elements...
     * since this is a multi-bulk write, but at this stage we don't know
     * the intersection set size, so we use a trick, append an empty object
     * to the output list and save the pointer to later modify it with the
     * right length */
    if (!dstkey) {
        lenobj = createObject(REDIS_STRING,NULL);
        addReply(c,lenobj);
        decrRefCount(lenobj);
    } else {
        /* If we have a target key where to store the resulting set
         * create this key with an empty set inside */
        dstset = createIntsetObject();
    }

    /* Iterate all the elements of the first (smallest) set, and test
     * the element against all the other sets, if at least one set does
     * not include the element it is discarded */
    si = setTypeInitIterator(sets[0]);
    while((ele = setTypeNext(si)) != NULL) {
        for (j = 1; j < setnum; j++)
            if (!setTypeIsMember(sets[j],ele)) break;

        /* Only take action when all sets contain the member */
        if (j == setnum) {
            if (!dstkey) {
                addReplyBulk(c,ele);
                cardinality++;
            } else {
                setTypeAdd(dstset,ele);
            }
        }
        decrRefCount(ele);
    }
    setTypeReleaseIterator(si);

    if (dstkey) {
        /* Store the resulting set into the target, if the intersection
         * is not an empty set. */
        dbDelete(c->db,dstkey);
        if (setTypeSize(dstset) > 0) {
            dbAdd(c->db,dstkey,dstset);
            addReplyLongLong(c,setTypeSize(dstset));
        } else {
            decrRefCount(dstset);
            addReply(c,shared.czero);
        }
        touchWatchedKey(c->db,dstkey);
        server.dirty++;
    } else {
        lenobj->ptr = sdscatprintf(sdsempty(),"*%lu\r\n",cardinality);
    }
    zfree(sets);
}

void sinterCommand(redisClient *c) {
    sinterGenericCommand(c,c->argv+1,c->argc-1,NULL);
}

void sinterstoreCommand(redisClient *c) {
    sinterGenericCommand(c,c->argv+2,c->argc-2,c->argv[1]);
}

#define REDIS_OP_UNION 0
#define REDIS_OP_DIFF 1
#define REDIS_OP_INTER 2

void sunionDiffGenericCommand(redisClient *c, robj **setkeys, int setnum, robj *dstkey, int op) {
    robj **sets = zmalloc(sizeof(robj*)*setnum);
    setTypeIterator *si;
    robj *ele, *dstset = NULL;
    int j, cardinality = 0;

    for (j = 0; j < setnum; j++) {
        robj *setobj = dstkey ?
            lookupKeyWrite(c->db,setkeys[j]) :
            lookupKeyRead(c->db,setkeys[j]);
        if (!setobj) {
            sets[j] = NULL;
            continue;
        }
        if (checkType(c,setobj,REDIS_SET)) {
            zfree(sets);
            return;
        }
        sets[j] = setobj;
    }

    /* We need a temp set object to store our union. If the dstkey
     * is not NULL (that is, we are inside an SUNIONSTORE operation) then
     * this set object will be the resulting object to set into the target key*/
    dstset = createIntsetObject();

    /* Iterate all the elements of all the sets, add every element a single
     * time to the result set */
    for (j = 0; j < setnum; j++) {
        if (op == REDIS_OP_DIFF && j == 0 && !sets[j]) break; /* result set is empty */
        if (!sets[j]) continue; /* non existing keys are like empty sets */

        si = setTypeInitIterator(sets[j]);
        while((ele = setTypeNext(si)) != NULL) {
            if (op == REDIS_OP_UNION || j == 0) {
                if (setTypeAdd(dstset,ele)) {
                    cardinality++;
                }
            } else if (op == REDIS_OP_DIFF) {
                if (setTypeRemove(dstset,ele)) {
                    cardinality--;
                }
            }
            decrRefCount(ele);
        }
        setTypeReleaseIterator(si);

        /* Exit when result set is empty. */
        if (op == REDIS_OP_DIFF && cardinality == 0) break;
    }

    /* Output the content of the resulting set, if not in STORE mode */
    if (!dstkey) {
        addReplySds(c,sdscatprintf(sdsempty(),"*%d\r\n",cardinality));
        si = setTypeInitIterator(dstset);
        while((ele = setTypeNext(si)) != NULL) {
            addReplyBulk(c,ele);
            decrRefCount(ele);
        }
        setTypeReleaseIterator(si);
        decrRefCount(dstset);
    } else {
        /* If we have a target key where to store the resulting set
         * create this key with the result set inside */
        dbDelete(c->db,dstkey);
        if (setTypeSize(dstset) > 0) {
            dbAdd(c->db,dstkey,dstset);
            addReplyLongLong(c,setTypeSize(dstset));
        } else {
            decrRefCount(dstset);
            addReply(c,shared.czero);
        }
        touchWatchedKey(c->db,dstkey);
        server.dirty++;
    }
    zfree(sets);
}

void sunionCommand(redisClient *c) {
    sunionDiffGenericCommand(c,c->argv+1,c->argc-1,NULL,REDIS_OP_UNION);
}

void sunionstoreCommand(redisClient *c) {
    sunionDiffGenericCommand(c,c->argv+2,c->argc-2,c->argv[1],REDIS_OP_UNION);
}

void sdiffCommand(redisClient *c) {
    sunionDiffGenericCommand(c,c->argv+1,c->argc-1,NULL,REDIS_OP_DIFF);
}

void sdiffstoreCommand(redisClient *c) {
    sunionDiffGenericCommand(c,c->argv+2,c->argc-2,c->argv[1],REDIS_OP_DIFF);
}
