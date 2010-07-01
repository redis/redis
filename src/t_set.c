#include "redis.h"

/*-----------------------------------------------------------------------------
 * Set Commands
 *----------------------------------------------------------------------------*/

void saddCommand(redisClient *c) {
    robj *set;

    set = lookupKeyWrite(c->db,c->argv[1]);
    if (set == NULL) {
        set = createSetObject();
        dbAdd(c->db,c->argv[1],set);
    } else {
        if (set->type != REDIS_SET) {
            addReply(c,shared.wrongtypeerr);
            return;
        }
    }
    if (dictAdd(set->ptr,c->argv[2],NULL) == DICT_OK) {
        incrRefCount(c->argv[2]);
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

    if (dictDelete(set->ptr,c->argv[2]) == DICT_OK) {
        server.dirty++;
        if (htNeedsResize(set->ptr)) dictResize(set->ptr);
        if (dictSize((dict*)set->ptr) == 0) dbDelete(c->db,c->argv[1]);
        addReply(c,shared.cone);
    } else {
        addReply(c,shared.czero);
    }
}

void smoveCommand(redisClient *c) {
    robj *srcset, *dstset;

    srcset = lookupKeyWrite(c->db,c->argv[1]);
    dstset = lookupKeyWrite(c->db,c->argv[2]);

    /* If the source key does not exist return 0, if it's of the wrong type
     * raise an error */
    if (srcset == NULL || srcset->type != REDIS_SET) {
        addReply(c, srcset ? shared.wrongtypeerr : shared.czero);
        return;
    }
    /* Error if the destination key is not a set as well */
    if (dstset && dstset->type != REDIS_SET) {
        addReply(c,shared.wrongtypeerr);
        return;
    }
    /* Remove the element from the source set */
    if (dictDelete(srcset->ptr,c->argv[3]) == DICT_ERR) {
        /* Key not found in the src set! return zero */
        addReply(c,shared.czero);
        return;
    }
    if (dictSize((dict*)srcset->ptr) == 0 && srcset != dstset)
        dbDelete(c->db,c->argv[1]);
    server.dirty++;
    /* Add the element to the destination set */
    if (!dstset) {
        dstset = createSetObject();
        dbAdd(c->db,c->argv[2],dstset);
    }
    if (dictAdd(dstset->ptr,c->argv[3],NULL) == DICT_OK)
        incrRefCount(c->argv[3]);
    addReply(c,shared.cone);
}

void sismemberCommand(redisClient *c) {
    robj *set;

    if ((set = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,set,REDIS_SET)) return;

    if (dictFind(set->ptr,c->argv[2]))
        addReply(c,shared.cone);
    else
        addReply(c,shared.czero);
}

void scardCommand(redisClient *c) {
    robj *o;
    dict *s;

    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,REDIS_SET)) return;

    s = o->ptr;
    addReplyUlong(c,dictSize(s));
}

void spopCommand(redisClient *c) {
    robj *set;
    dictEntry *de;

    if ((set = lookupKeyWriteOrReply(c,c->argv[1],shared.nullbulk)) == NULL ||
        checkType(c,set,REDIS_SET)) return;

    de = dictGetRandomKey(set->ptr);
    if (de == NULL) {
        addReply(c,shared.nullbulk);
    } else {
        robj *ele = dictGetEntryKey(de);

        addReplyBulk(c,ele);
        dictDelete(set->ptr,ele);
        if (htNeedsResize(set->ptr)) dictResize(set->ptr);
        if (dictSize((dict*)set->ptr) == 0) dbDelete(c->db,c->argv[1]);
        server.dirty++;
    }
}

void srandmemberCommand(redisClient *c) {
    robj *set;
    dictEntry *de;

    if ((set = lookupKeyReadOrReply(c,c->argv[1],shared.nullbulk)) == NULL ||
        checkType(c,set,REDIS_SET)) return;

    de = dictGetRandomKey(set->ptr);
    if (de == NULL) {
        addReply(c,shared.nullbulk);
    } else {
        robj *ele = dictGetEntryKey(de);

        addReplyBulk(c,ele);
    }
}

int qsortCompareSetsByCardinality(const void *s1, const void *s2) {
    dict **d1 = (void*) s1, **d2 = (void*) s2;

    return dictSize(*d1)-dictSize(*d2);
}

void sinterGenericCommand(redisClient *c, robj **setskeys, unsigned long setsnum, robj *dstkey) {
    dict **dv = zmalloc(sizeof(dict*)*setsnum);
    dictIterator *di;
    dictEntry *de;
    robj *lenobj = NULL, *dstset = NULL;
    unsigned long j, cardinality = 0;

    for (j = 0; j < setsnum; j++) {
        robj *setobj;

        setobj = dstkey ?
                    lookupKeyWrite(c->db,setskeys[j]) :
                    lookupKeyRead(c->db,setskeys[j]);
        if (!setobj) {
            zfree(dv);
            if (dstkey) {
                if (dbDelete(c->db,dstkey))
                    server.dirty++;
                addReply(c,shared.czero);
            } else {
                addReply(c,shared.emptymultibulk);
            }
            return;
        }
        if (setobj->type != REDIS_SET) {
            zfree(dv);
            addReply(c,shared.wrongtypeerr);
            return;
        }
        dv[j] = setobj->ptr;
    }
    /* Sort sets from the smallest to largest, this will improve our
     * algorithm's performace */
    qsort(dv,setsnum,sizeof(dict*),qsortCompareSetsByCardinality);

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
        dstset = createSetObject();
    }

    /* Iterate all the elements of the first (smallest) set, and test
     * the element against all the other sets, if at least one set does
     * not include the element it is discarded */
    di = dictGetIterator(dv[0]);

    while((de = dictNext(di)) != NULL) {
        robj *ele;

        for (j = 1; j < setsnum; j++)
            if (dictFind(dv[j],dictGetEntryKey(de)) == NULL) break;
        if (j != setsnum)
            continue; /* at least one set does not contain the member */
        ele = dictGetEntryKey(de);
        if (!dstkey) {
            addReplyBulk(c,ele);
            cardinality++;
        } else {
            dictAdd(dstset->ptr,ele,NULL);
            incrRefCount(ele);
        }
    }
    dictReleaseIterator(di);

    if (dstkey) {
        /* Store the resulting set into the target, if the intersection
         * is not an empty set. */
        dbDelete(c->db,dstkey);
        if (dictSize((dict*)dstset->ptr) > 0) {
            dbAdd(c->db,dstkey,dstset);
            addReplyLongLong(c,dictSize((dict*)dstset->ptr));
        } else {
            decrRefCount(dstset);
            addReply(c,shared.czero);
        }
        server.dirty++;
    } else {
        lenobj->ptr = sdscatprintf(sdsempty(),"*%lu\r\n",cardinality);
    }
    zfree(dv);
}

void sinterCommand(redisClient *c) {
    sinterGenericCommand(c,c->argv+1,c->argc-1,NULL);
}

void sinterstoreCommand(redisClient *c) {
    sinterGenericCommand(c,c->argv+2,c->argc-2,c->argv[1]);
}

void sunionDiffGenericCommand(redisClient *c, robj **setskeys, int setsnum, robj *dstkey, int op) {
    dict **dv = zmalloc(sizeof(dict*)*setsnum);
    dictIterator *di;
    dictEntry *de;
    robj *dstset = NULL;
    int j, cardinality = 0;

    for (j = 0; j < setsnum; j++) {
        robj *setobj;

        setobj = dstkey ?
                    lookupKeyWrite(c->db,setskeys[j]) :
                    lookupKeyRead(c->db,setskeys[j]);
        if (!setobj) {
            dv[j] = NULL;
            continue;
        }
        if (setobj->type != REDIS_SET) {
            zfree(dv);
            addReply(c,shared.wrongtypeerr);
            return;
        }
        dv[j] = setobj->ptr;
    }

    /* We need a temp set object to store our union. If the dstkey
     * is not NULL (that is, we are inside an SUNIONSTORE operation) then
     * this set object will be the resulting object to set into the target key*/
    dstset = createSetObject();

    /* Iterate all the elements of all the sets, add every element a single
     * time to the result set */
    for (j = 0; j < setsnum; j++) {
        if (op == REDIS_OP_DIFF && j == 0 && !dv[j]) break; /* result set is empty */
        if (!dv[j]) continue; /* non existing keys are like empty sets */

        di = dictGetIterator(dv[j]);

        while((de = dictNext(di)) != NULL) {
            robj *ele;

            /* dictAdd will not add the same element multiple times */
            ele = dictGetEntryKey(de);
            if (op == REDIS_OP_UNION || j == 0) {
                if (dictAdd(dstset->ptr,ele,NULL) == DICT_OK) {
                    incrRefCount(ele);
                    cardinality++;
                }
            } else if (op == REDIS_OP_DIFF) {
                if (dictDelete(dstset->ptr,ele) == DICT_OK) {
                    cardinality--;
                }
            }
        }
        dictReleaseIterator(di);

        /* result set is empty? Exit asap. */
        if (op == REDIS_OP_DIFF && cardinality == 0) break;
    }

    /* Output the content of the resulting set, if not in STORE mode */
    if (!dstkey) {
        addReplySds(c,sdscatprintf(sdsempty(),"*%d\r\n",cardinality));
        di = dictGetIterator(dstset->ptr);
        while((de = dictNext(di)) != NULL) {
            robj *ele;

            ele = dictGetEntryKey(de);
            addReplyBulk(c,ele);
        }
        dictReleaseIterator(di);
        decrRefCount(dstset);
    } else {
        /* If we have a target key where to store the resulting set
         * create this key with the result set inside */
        dbDelete(c->db,dstkey);
        if (dictSize((dict*)dstset->ptr) > 0) {
            dbAdd(c->db,dstkey,dstset);
            addReplyLongLong(c,dictSize((dict*)dstset->ptr));
        } else {
            decrRefCount(dstset);
            addReply(c,shared.czero);
        }
        server.dirty++;
    }
    zfree(dv);
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
