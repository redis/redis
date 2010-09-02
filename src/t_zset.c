#include "redis.h"

#include <math.h>

/*-----------------------------------------------------------------------------
 * Sorted set API
 *----------------------------------------------------------------------------*/

/* ZSETs are ordered sets using two data structures to hold the same elements
 * in order to get O(log(N)) INSERT and REMOVE operations into a sorted
 * data structure.
 *
 * The elements are added to an hash table mapping Redis objects to scores.
 * At the same time the elements are added to a skip list mapping scores
 * to Redis objects (so objects are sorted by scores in this "view"). */

/* This skiplist implementation is almost a C translation of the original
 * algorithm described by William Pugh in "Skip Lists: A Probabilistic
 * Alternative to Balanced Trees", modified in three ways:
 * a) this implementation allows for repeated values.
 * b) the comparison is not just by key (our 'score') but by satellite data.
 * c) there is a back pointer, so it's a doubly linked list with the back
 * pointers being only at "level 1". This allows to traverse the list
 * from tail to head, useful for ZREVRANGE. */

zskiplistNode *zslCreateNode(int level, double score, robj *obj) {
    zskiplistNode *zn = zmalloc(sizeof(*zn));

    zn->forward = zmalloc(sizeof(zskiplistNode*) * level);
    if (level > 1)
        zn->span = zmalloc(sizeof(unsigned int) * (level - 1));
    else
        zn->span = NULL;
    zn->score = score;
    zn->obj = obj;
    return zn;
}

zskiplist *zslCreate(void) {
    int j;
    zskiplist *zsl;

    zsl = zmalloc(sizeof(*zsl));
    zsl->level = 1;
    zsl->length = 0;
    zsl->header = zslCreateNode(ZSKIPLIST_MAXLEVEL,0,NULL);
    for (j = 0; j < ZSKIPLIST_MAXLEVEL; j++) {
        zsl->header->forward[j] = NULL;

        /* span has space for ZSKIPLIST_MAXLEVEL-1 elements */
        if (j < ZSKIPLIST_MAXLEVEL-1)
            zsl->header->span[j] = 0;
    }
    zsl->header->backward = NULL;
    zsl->tail = NULL;
    return zsl;
}

void zslFreeNode(zskiplistNode *node) {
    decrRefCount(node->obj);
    zfree(node->forward);
    zfree(node->span);
    zfree(node);
}

void zslFree(zskiplist *zsl) {
    zskiplistNode *node = zsl->header->forward[0], *next;

    zfree(zsl->header->forward);
    zfree(zsl->header->span);
    zfree(zsl->header);
    while(node) {
        next = node->forward[0];
        zslFreeNode(node);
        node = next;
    }
    zfree(zsl);
}

int zslRandomLevel(void) {
    int level = 1;
    while ((random()&0xFFFF) < (ZSKIPLIST_P * 0xFFFF))
        level += 1;
    return (level<ZSKIPLIST_MAXLEVEL) ? level : ZSKIPLIST_MAXLEVEL;
}

void zslInsert(zskiplist *zsl, double score, robj *obj) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    unsigned int rank[ZSKIPLIST_MAXLEVEL];
    int i, level;

    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        /* store rank that is crossed to reach the insert position */
        rank[i] = i == (zsl->level-1) ? 0 : rank[i+1];

        while (x->forward[i] &&
            (x->forward[i]->score < score ||
                (x->forward[i]->score == score &&
                compareStringObjects(x->forward[i]->obj,obj) < 0))) {
            rank[i] += i > 0 ? x->span[i-1] : 1;
            x = x->forward[i];
        }
        update[i] = x;
    }
    /* we assume the key is not already inside, since we allow duplicated
     * scores, and the re-insertion of score and redis object should never
     * happpen since the caller of zslInsert() should test in the hash table
     * if the element is already inside or not. */
    level = zslRandomLevel();
    if (level > zsl->level) {
        for (i = zsl->level; i < level; i++) {
            rank[i] = 0;
            update[i] = zsl->header;
            update[i]->span[i-1] = zsl->length;
        }
        zsl->level = level;
    }
    x = zslCreateNode(level,score,obj);
    for (i = 0; i < level; i++) {
        x->forward[i] = update[i]->forward[i];
        update[i]->forward[i] = x;

        /* update span covered by update[i] as x is inserted here */
        if (i > 0) {
            x->span[i-1] = update[i]->span[i-1] - (rank[0] - rank[i]);
            update[i]->span[i-1] = (rank[0] - rank[i]) + 1;
        }
    }

    /* increment span for untouched levels */
    for (i = level; i < zsl->level; i++) {
        update[i]->span[i-1]++;
    }

    x->backward = (update[0] == zsl->header) ? NULL : update[0];
    if (x->forward[0])
        x->forward[0]->backward = x;
    else
        zsl->tail = x;
    zsl->length++;
}

/* Internal function used by zslDelete, zslDeleteByScore and zslDeleteByRank */
void zslDeleteNode(zskiplist *zsl, zskiplistNode *x, zskiplistNode **update) {
    int i;
    for (i = 0; i < zsl->level; i++) {
        if (update[i]->forward[i] == x) {
            if (i > 0) {
                update[i]->span[i-1] += x->span[i-1] - 1;
            }
            update[i]->forward[i] = x->forward[i];
        } else {
            /* invariant: i > 0, because update[0]->forward[0]
             * is always equal to x */
            update[i]->span[i-1] -= 1;
        }
    }
    if (x->forward[0]) {
        x->forward[0]->backward = x->backward;
    } else {
        zsl->tail = x->backward;
    }
    while(zsl->level > 1 && zsl->header->forward[zsl->level-1] == NULL)
        zsl->level--;
    zsl->length--;
}

/* Delete an element with matching score/object from the skiplist. */
int zslDelete(zskiplist *zsl, double score, robj *obj) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    int i;

    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        while (x->forward[i] &&
            (x->forward[i]->score < score ||
                (x->forward[i]->score == score &&
                compareStringObjects(x->forward[i]->obj,obj) < 0)))
            x = x->forward[i];
        update[i] = x;
    }
    /* We may have multiple elements with the same score, what we need
     * is to find the element with both the right score and object. */
    x = x->forward[0];
    if (x && score == x->score && equalStringObjects(x->obj,obj)) {
        zslDeleteNode(zsl, x, update);
        zslFreeNode(x);
        return 1;
    } else {
        return 0; /* not found */
    }
    return 0; /* not found */
}

/* Delete all the elements with score between min and max from the skiplist.
 * Min and mx are inclusive, so a score >= min || score <= max is deleted.
 * Note that this function takes the reference to the hash table view of the
 * sorted set, in order to remove the elements from the hash table too. */
unsigned long zslDeleteRangeByScore(zskiplist *zsl, double min, double max, dict *dict) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    unsigned long removed = 0;
    int i;

    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        while (x->forward[i] && x->forward[i]->score < min)
            x = x->forward[i];
        update[i] = x;
    }
    /* We may have multiple elements with the same score, what we need
     * is to find the element with both the right score and object. */
    x = x->forward[0];
    while (x && x->score <= max) {
        zskiplistNode *next = x->forward[0];
        zslDeleteNode(zsl, x, update);
        dictDelete(dict,x->obj);
        zslFreeNode(x);
        removed++;
        x = next;
    }
    return removed; /* not found */
}

/* Delete all the elements with rank between start and end from the skiplist.
 * Start and end are inclusive. Note that start and end need to be 1-based */
unsigned long zslDeleteRangeByRank(zskiplist *zsl, unsigned int start, unsigned int end, dict *dict) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    unsigned long traversed = 0, removed = 0;
    int i;

    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        while (x->forward[i] && (traversed + (i > 0 ? x->span[i-1] : 1)) < start) {
            traversed += i > 0 ? x->span[i-1] : 1;
            x = x->forward[i];
        }
        update[i] = x;
    }

    traversed++;
    x = x->forward[0];
    while (x && traversed <= end) {
        zskiplistNode *next = x->forward[0];
        zslDeleteNode(zsl, x, update);
        dictDelete(dict,x->obj);
        zslFreeNode(x);
        removed++;
        traversed++;
        x = next;
    }
    return removed;
}

/* Find the first node having a score equal or greater than the specified one.
 * Returns NULL if there is no match. */
zskiplistNode *zslFirstWithScore(zskiplist *zsl, double score) {
    zskiplistNode *x;
    int i;

    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        while (x->forward[i] && x->forward[i]->score < score)
            x = x->forward[i];
    }
    /* We may have multiple elements with the same score, what we need
     * is to find the element with both the right score and object. */
    return x->forward[0];
}

/* Find the rank for an element by both score and key.
 * Returns 0 when the element cannot be found, rank otherwise.
 * Note that the rank is 1-based due to the span of zsl->header to the
 * first element. */
unsigned long zslistTypeGetRank(zskiplist *zsl, double score, robj *o) {
    zskiplistNode *x;
    unsigned long rank = 0;
    int i;

    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        while (x->forward[i] &&
            (x->forward[i]->score < score ||
                (x->forward[i]->score == score &&
                compareStringObjects(x->forward[i]->obj,o) <= 0))) {
            rank += i > 0 ? x->span[i-1] : 1;
            x = x->forward[i];
        }

        /* x might be equal to zsl->header, so test if obj is non-NULL */
        if (x->obj && equalStringObjects(x->obj,o)) {
            return rank;
        }
    }
    return 0;
}

/* Finds an element by its rank. The rank argument needs to be 1-based. */
zskiplistNode* zslistTypeGetElementByRank(zskiplist *zsl, unsigned long rank) {
    zskiplistNode *x;
    unsigned long traversed = 0;
    int i;

    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        while (x->forward[i] && (traversed + (i>0 ? x->span[i-1] : 1)) <= rank)
        {
            traversed += i > 0 ? x->span[i-1] : 1;
            x = x->forward[i];
        }
        if (traversed == rank) {
            return x;
        }
    }
    return NULL;
}

/*-----------------------------------------------------------------------------
 * Sorted set commands 
 *----------------------------------------------------------------------------*/

/* This generic command implements both ZADD and ZINCRBY.
 * scoreval is the score if the operation is a ZADD (doincrement == 0) or
 * the increment if the operation is a ZINCRBY (doincrement == 1). */
void zaddGenericCommand(redisClient *c, robj *key, robj *ele, double scoreval, int doincrement) {
    robj *zsetobj;
    zset *zs;
    double *score;

    zsetobj = lookupKeyWrite(c->db,key);
    if (zsetobj == NULL) {
        zsetobj = createZsetObject();
        dbAdd(c->db,key,zsetobj);
    } else {
        if (zsetobj->type != REDIS_ZSET) {
            addReply(c,shared.wrongtypeerr);
            return;
        }
    }
    zs = zsetobj->ptr;

    /* Ok now since we implement both ZADD and ZINCRBY here the code
     * needs to handle the two different conditions. It's all about setting
     * '*score', that is, the new score to set, to the right value. */
    score = zmalloc(sizeof(double));
    if (doincrement) {
        dictEntry *de;

        /* Read the old score. If the element was not present starts from 0 */
        de = dictFind(zs->dict,ele);
        if (de) {
            double *oldscore = dictGetEntryVal(de);
            *score = *oldscore + scoreval;
        } else {
            *score = scoreval;
        }
        if (isnan(*score)) {
            addReplySds(c,
                sdsnew("-ERR resulting score is not a number (NaN)\r\n"));
            zfree(score);
            /* Note that we don't need to check if the zset may be empty and
             * should be removed here, as we can only obtain Nan as score if
             * there was already an element in the sorted set. */
            return;
        }
    } else {
        *score = scoreval;
    }

    /* What follows is a simple remove and re-insert operation that is common
     * to both ZADD and ZINCRBY... */
    if (dictAdd(zs->dict,ele,score) == DICT_OK) {
        /* case 1: New element */
        incrRefCount(ele); /* added to hash */
        zslInsert(zs->zsl,*score,ele);
        incrRefCount(ele); /* added to skiplist */
        touchWatchedKey(c->db,c->argv[1]);
        server.dirty++;
        if (doincrement)
            addReplyDouble(c,*score);
        else
            addReply(c,shared.cone);
    } else {
        dictEntry *de;
        double *oldscore;

        /* case 2: Score update operation */
        de = dictFind(zs->dict,ele);
        redisAssert(de != NULL);
        oldscore = dictGetEntryVal(de);
        if (*score != *oldscore) {
            int deleted;

            /* Remove and insert the element in the skip list with new score */
            deleted = zslDelete(zs->zsl,*oldscore,ele);
            redisAssert(deleted != 0);
            zslInsert(zs->zsl,*score,ele);
            incrRefCount(ele);
            /* Update the score in the hash table */
            dictReplace(zs->dict,ele,score);
            touchWatchedKey(c->db,c->argv[1]);
            server.dirty++;
        } else {
            zfree(score);
        }
        if (doincrement)
            addReplyDouble(c,*score);
        else
            addReply(c,shared.czero);
    }
}

void zaddCommand(redisClient *c) {
    double scoreval;
    if (getDoubleFromObjectOrReply(c,c->argv[2],&scoreval,NULL) != REDIS_OK) return;
    zaddGenericCommand(c,c->argv[1],c->argv[3],scoreval,0);
}

void zincrbyCommand(redisClient *c) {
    double scoreval;
    if (getDoubleFromObjectOrReply(c,c->argv[2],&scoreval,NULL) != REDIS_OK) return;
    zaddGenericCommand(c,c->argv[1],c->argv[3],scoreval,1);
}

void zremCommand(redisClient *c) {
    robj *zsetobj;
    zset *zs;
    dictEntry *de;
    double *oldscore;
    int deleted;

    if ((zsetobj = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,zsetobj,REDIS_ZSET)) return;

    zs = zsetobj->ptr;
    de = dictFind(zs->dict,c->argv[2]);
    if (de == NULL) {
        addReply(c,shared.czero);
        return;
    }
    /* Delete from the skiplist */
    oldscore = dictGetEntryVal(de);
    deleted = zslDelete(zs->zsl,*oldscore,c->argv[2]);
    redisAssert(deleted != 0);

    /* Delete from the hash table */
    dictDelete(zs->dict,c->argv[2]);
    if (htNeedsResize(zs->dict)) dictResize(zs->dict);
    if (dictSize(zs->dict) == 0) dbDelete(c->db,c->argv[1]);
    touchWatchedKey(c->db,c->argv[1]);
    server.dirty++;
    addReply(c,shared.cone);
}

void zremrangebyscoreCommand(redisClient *c) {
    double min;
    double max;
    long deleted;
    robj *zsetobj;
    zset *zs;

    if ((getDoubleFromObjectOrReply(c, c->argv[2], &min, NULL) != REDIS_OK) ||
        (getDoubleFromObjectOrReply(c, c->argv[3], &max, NULL) != REDIS_OK)) return;

    if ((zsetobj = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,zsetobj,REDIS_ZSET)) return;

    zs = zsetobj->ptr;
    deleted = zslDeleteRangeByScore(zs->zsl,min,max,zs->dict);
    if (htNeedsResize(zs->dict)) dictResize(zs->dict);
    if (dictSize(zs->dict) == 0) dbDelete(c->db,c->argv[1]);
    if (deleted) touchWatchedKey(c->db,c->argv[1]);
    server.dirty += deleted;
    addReplyLongLong(c,deleted);
}

void zremrangebyrankCommand(redisClient *c) {
    long start;
    long end;
    int llen;
    long deleted;
    robj *zsetobj;
    zset *zs;

    if ((getLongFromObjectOrReply(c, c->argv[2], &start, NULL) != REDIS_OK) ||
        (getLongFromObjectOrReply(c, c->argv[3], &end, NULL) != REDIS_OK)) return;

    if ((zsetobj = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,zsetobj,REDIS_ZSET)) return;
    zs = zsetobj->ptr;
    llen = zs->zsl->length;

    /* convert negative indexes */
    if (start < 0) start = llen+start;
    if (end < 0) end = llen+end;
    if (start < 0) start = 0;

    /* Invariant: start >= 0, so this test will be true when end < 0.
     * The range is empty when start > end or start >= length. */
    if (start > end || start >= llen) {
        addReply(c,shared.czero);
        return;
    }
    if (end >= llen) end = llen-1;

    /* increment start and end because zsl*Rank functions
     * use 1-based rank */
    deleted = zslDeleteRangeByRank(zs->zsl,start+1,end+1,zs->dict);
    if (htNeedsResize(zs->dict)) dictResize(zs->dict);
    if (dictSize(zs->dict) == 0) dbDelete(c->db,c->argv[1]);
    if (deleted) touchWatchedKey(c->db,c->argv[1]);
    server.dirty += deleted;
    addReplyLongLong(c, deleted);
}

typedef struct {
    dict *dict;
    double weight;
} zsetopsrc;

int qsortCompareZsetopsrcByCardinality(const void *s1, const void *s2) {
    zsetopsrc *d1 = (void*) s1, *d2 = (void*) s2;
    unsigned long size1, size2;
    size1 = d1->dict ? dictSize(d1->dict) : 0;
    size2 = d2->dict ? dictSize(d2->dict) : 0;
    return size1 - size2;
}

#define REDIS_AGGR_SUM 1
#define REDIS_AGGR_MIN 2
#define REDIS_AGGR_MAX 3
#define zunionInterDictValue(_e) (dictGetEntryVal(_e) == NULL ? 1.0 : *(double*)dictGetEntryVal(_e))

inline static void zunionInterAggregate(double *target, double val, int aggregate) {
    if (aggregate == REDIS_AGGR_SUM) {
        *target = *target + val;
        /* The result of adding two doubles is NaN when one variable
         * is +inf and the other is -inf. When these numbers are added,
         * we maintain the convention of the result being 0.0. */
        if (isnan(*target)) *target = 0.0;
    } else if (aggregate == REDIS_AGGR_MIN) {
        *target = val < *target ? val : *target;
    } else if (aggregate == REDIS_AGGR_MAX) {
        *target = val > *target ? val : *target;
    } else {
        /* safety net */
        redisPanic("Unknown ZUNION/INTER aggregate type");
    }
}

void zunionInterGenericCommand(redisClient *c, robj *dstkey, int op) {
    int i, j, setnum;
    int aggregate = REDIS_AGGR_SUM;
    zsetopsrc *src;
    robj *dstobj;
    zset *dstzset;
    dictIterator *di;
    dictEntry *de;
    int touched = 0;

    /* expect setnum input keys to be given */
    setnum = atoi(c->argv[2]->ptr);
    if (setnum < 1) {
        addReplySds(c,sdsnew("-ERR at least 1 input key is needed for ZUNIONSTORE/ZINTERSTORE\r\n"));
        return;
    }

    /* test if the expected number of keys would overflow */
    if (3+setnum > c->argc) {
        addReply(c,shared.syntaxerr);
        return;
    }

    /* read keys to be used for input */
    src = zmalloc(sizeof(zsetopsrc) * setnum);
    for (i = 0, j = 3; i < setnum; i++, j++) {
        robj *obj = lookupKeyWrite(c->db,c->argv[j]);
        if (!obj) {
            src[i].dict = NULL;
        } else {
            if (obj->type == REDIS_ZSET) {
                src[i].dict = ((zset*)obj->ptr)->dict;
            } else if (obj->type == REDIS_SET) {
                src[i].dict = (obj->ptr);
            } else {
                zfree(src);
                addReply(c,shared.wrongtypeerr);
                return;
            }
        }

        /* default all weights to 1 */
        src[i].weight = 1.0;
    }

    /* parse optional extra arguments */
    if (j < c->argc) {
        int remaining = c->argc - j;

        while (remaining) {
            if (remaining >= (setnum + 1) && !strcasecmp(c->argv[j]->ptr,"weights")) {
                j++; remaining--;
                for (i = 0; i < setnum; i++, j++, remaining--) {
                    if (getDoubleFromObjectOrReply(c,c->argv[j],&src[i].weight,
                            "weight value is not a double") != REDIS_OK)
                    {
                        zfree(src);
                        return;
                    }
                }
            } else if (remaining >= 2 && !strcasecmp(c->argv[j]->ptr,"aggregate")) {
                j++; remaining--;
                if (!strcasecmp(c->argv[j]->ptr,"sum")) {
                    aggregate = REDIS_AGGR_SUM;
                } else if (!strcasecmp(c->argv[j]->ptr,"min")) {
                    aggregate = REDIS_AGGR_MIN;
                } else if (!strcasecmp(c->argv[j]->ptr,"max")) {
                    aggregate = REDIS_AGGR_MAX;
                } else {
                    zfree(src);
                    addReply(c,shared.syntaxerr);
                    return;
                }
                j++; remaining--;
            } else {
                zfree(src);
                addReply(c,shared.syntaxerr);
                return;
            }
        }
    }

    /* sort sets from the smallest to largest, this will improve our
     * algorithm's performance */
    qsort(src,setnum,sizeof(zsetopsrc),qsortCompareZsetopsrcByCardinality);

    dstobj = createZsetObject();
    dstzset = dstobj->ptr;

    if (op == REDIS_OP_INTER) {
        /* skip going over all entries if the smallest zset is NULL or empty */
        if (src[0].dict && dictSize(src[0].dict) > 0) {
            /* precondition: as src[0].dict is non-empty and the zsets are ordered
             * from small to large, all src[i > 0].dict are non-empty too */
            di = dictGetIterator(src[0].dict);
            while((de = dictNext(di)) != NULL) {
                double *score = zmalloc(sizeof(double)), value;
                *score = src[0].weight * zunionInterDictValue(de);

                for (j = 1; j < setnum; j++) {
                    dictEntry *other = dictFind(src[j].dict,dictGetEntryKey(de));
                    if (other) {
                        value = src[j].weight * zunionInterDictValue(other);
                        zunionInterAggregate(score, value, aggregate);
                    } else {
                        break;
                    }
                }

                /* skip entry when not present in every source dict */
                if (j != setnum) {
                    zfree(score);
                } else {
                    robj *o = dictGetEntryKey(de);
                    dictAdd(dstzset->dict,o,score);
                    incrRefCount(o); /* added to dictionary */
                    zslInsert(dstzset->zsl,*score,o);
                    incrRefCount(o); /* added to skiplist */
                }
            }
            dictReleaseIterator(di);
        }
    } else if (op == REDIS_OP_UNION) {
        for (i = 0; i < setnum; i++) {
            if (!src[i].dict) continue;

            di = dictGetIterator(src[i].dict);
            while((de = dictNext(di)) != NULL) {
                /* skip key when already processed */
                if (dictFind(dstzset->dict,dictGetEntryKey(de)) != NULL) continue;

                double *score = zmalloc(sizeof(double)), value;
                *score = src[i].weight * zunionInterDictValue(de);

                /* because the zsets are sorted by size, its only possible
                 * for sets at larger indices to hold this entry */
                for (j = (i+1); j < setnum; j++) {
                    dictEntry *other = dictFind(src[j].dict,dictGetEntryKey(de));
                    if (other) {
                        value = src[j].weight * zunionInterDictValue(other);
                        zunionInterAggregate(score, value, aggregate);
                    }
                }

                robj *o = dictGetEntryKey(de);
                dictAdd(dstzset->dict,o,score);
                incrRefCount(o); /* added to dictionary */
                zslInsert(dstzset->zsl,*score,o);
                incrRefCount(o); /* added to skiplist */
            }
            dictReleaseIterator(di);
        }
    } else {
        /* unknown operator */
        redisAssert(op == REDIS_OP_INTER || op == REDIS_OP_UNION);
    }

    if (dbDelete(c->db,dstkey)) {
        touchWatchedKey(c->db,dstkey);
        touched = 1;
        server.dirty++;
    }
    if (dstzset->zsl->length) {
        dbAdd(c->db,dstkey,dstobj);
        addReplyLongLong(c, dstzset->zsl->length);
        if (!touched) touchWatchedKey(c->db,dstkey);
        server.dirty++;
    } else {
        decrRefCount(dstobj);
        addReply(c, shared.czero);
    }
    zfree(src);
}

void zunionstoreCommand(redisClient *c) {
    zunionInterGenericCommand(c,c->argv[1], REDIS_OP_UNION);
}

void zinterstoreCommand(redisClient *c) {
    zunionInterGenericCommand(c,c->argv[1], REDIS_OP_INTER);
}

void zrangeGenericCommand(redisClient *c, int reverse) {
    robj *o;
    long start;
    long end;
    int withscores = 0;
    int llen;
    int rangelen, j;
    zset *zsetobj;
    zskiplist *zsl;
    zskiplistNode *ln;
    robj *ele;

    if ((getLongFromObjectOrReply(c, c->argv[2], &start, NULL) != REDIS_OK) ||
        (getLongFromObjectOrReply(c, c->argv[3], &end, NULL) != REDIS_OK)) return;

    if (c->argc == 5 && !strcasecmp(c->argv[4]->ptr,"withscores")) {
        withscores = 1;
    } else if (c->argc >= 5) {
        addReply(c,shared.syntaxerr);
        return;
    }

    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptymultibulk)) == NULL
         || checkType(c,o,REDIS_ZSET)) return;
    zsetobj = o->ptr;
    zsl = zsetobj->zsl;
    llen = zsl->length;

    /* convert negative indexes */
    if (start < 0) start = llen+start;
    if (end < 0) end = llen+end;
    if (start < 0) start = 0;

    /* Invariant: start >= 0, so this test will be true when end < 0.
     * The range is empty when start > end or start >= length. */
    if (start > end || start >= llen) {
        addReply(c,shared.emptymultibulk);
        return;
    }
    if (end >= llen) end = llen-1;
    rangelen = (end-start)+1;

    /* check if starting point is trivial, before searching
     * the element in log(N) time */
    if (reverse) {
        ln = start == 0 ? zsl->tail : zslistTypeGetElementByRank(zsl, llen-start);
    } else {
        ln = start == 0 ?
            zsl->header->forward[0] : zslistTypeGetElementByRank(zsl, start+1);
    }

    /* Return the result in form of a multi-bulk reply */
    addReplyMultiBulkLen(c,withscores ? (rangelen*2) : rangelen);
    for (j = 0; j < rangelen; j++) {
        ele = ln->obj;
        addReplyBulk(c,ele);
        if (withscores)
            addReplyDouble(c,ln->score);
        ln = reverse ? ln->backward : ln->forward[0];
    }
}

void zrangeCommand(redisClient *c) {
    zrangeGenericCommand(c,0);
}

void zrevrangeCommand(redisClient *c) {
    zrangeGenericCommand(c,1);
}

/* This command implements both ZRANGEBYSCORE and ZCOUNT.
 * If justcount is non-zero, just the count is returned. */
void genericZrangebyscoreCommand(redisClient *c, int justcount) {
    robj *o;
    double min, max;
    int minex = 0, maxex = 0; /* are min or max exclusive? */
    int offset = 0, limit = -1;
    int withscores = 0;
    int badsyntax = 0;

    /* Parse the min-max interval. If one of the values is prefixed
     * by the "(" character, it's considered "open". For instance
     * ZRANGEBYSCORE zset (1.5 (2.5 will match min < x < max
     * ZRANGEBYSCORE zset 1.5 2.5 will instead match min <= x <= max */
    if (((char*)c->argv[2]->ptr)[0] == '(') {
        min = strtod((char*)c->argv[2]->ptr+1,NULL);
        minex = 1;
    } else {
        min = strtod(c->argv[2]->ptr,NULL);
    }
    if (((char*)c->argv[3]->ptr)[0] == '(') {
        max = strtod((char*)c->argv[3]->ptr+1,NULL);
        maxex = 1;
    } else {
        max = strtod(c->argv[3]->ptr,NULL);
    }

    /* Parse "WITHSCORES": note that if the command was called with
     * the name ZCOUNT then we are sure that c->argc == 4, so we'll never
     * enter the following paths to parse WITHSCORES and LIMIT. */
    if (c->argc == 5 || c->argc == 8) {
        if (strcasecmp(c->argv[c->argc-1]->ptr,"withscores") == 0)
            withscores = 1;
        else
            badsyntax = 1;
    }
    if (c->argc != (4 + withscores) && c->argc != (7 + withscores))
        badsyntax = 1;
    if (badsyntax) {
        addReplySds(c,
            sdsnew("-ERR wrong number of arguments for ZRANGEBYSCORE\r\n"));
        return;
    }

    /* Parse "LIMIT" */
    if (c->argc == (7 + withscores) && strcasecmp(c->argv[4]->ptr,"limit")) {
        addReply(c,shared.syntaxerr);
        return;
    } else if (c->argc == (7 + withscores)) {
        offset = atoi(c->argv[5]->ptr);
        limit = atoi(c->argv[6]->ptr);
        if (offset < 0) offset = 0;
    }

    /* Ok, lookup the key and get the range */
    o = lookupKeyRead(c->db,c->argv[1]);
    if (o == NULL) {
        addReply(c,justcount ? shared.czero : shared.emptymultibulk);
    } else {
        if (o->type != REDIS_ZSET) {
            addReply(c,shared.wrongtypeerr);
        } else {
            zset *zsetobj = o->ptr;
            zskiplist *zsl = zsetobj->zsl;
            zskiplistNode *ln;
            robj *ele;
            void *replylen = NULL;
            unsigned long rangelen = 0;

            /* Get the first node with the score >= min, or with
             * score > min if 'minex' is true. */
            ln = zslFirstWithScore(zsl,min);
            while (minex && ln && ln->score == min) ln = ln->forward[0];

            if (ln == NULL) {
                /* No element matching the speciifed interval */
                addReply(c,justcount ? shared.czero : shared.emptymultibulk);
                return;
            }

            /* We don't know in advance how many matching elements there
             * are in the list, so we push this object that will represent
             * the multi-bulk length in the output buffer, and will "fix"
             * it later */
            if (!justcount)
                replylen = addDeferredMultiBulkLength(c);

            while(ln && (maxex ? (ln->score < max) : (ln->score <= max))) {
                if (offset) {
                    offset--;
                    ln = ln->forward[0];
                    continue;
                }
                if (limit == 0) break;
                if (!justcount) {
                    ele = ln->obj;
                    addReplyBulk(c,ele);
                    if (withscores)
                        addReplyDouble(c,ln->score);
                }
                ln = ln->forward[0];
                rangelen++;
                if (limit > 0) limit--;
            }
            if (justcount) {
                addReplyLongLong(c,(long)rangelen);
            } else {
                setDeferredMultiBulkLength(c,replylen,
                     withscores ? (rangelen*2) : rangelen);
            }
        }
    }
}

void zrangebyscoreCommand(redisClient *c) {
    genericZrangebyscoreCommand(c,0);
}

void zcountCommand(redisClient *c) {
    genericZrangebyscoreCommand(c,1);
}

void zcardCommand(redisClient *c) {
    robj *o;
    zset *zs;

    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,REDIS_ZSET)) return;

    zs = o->ptr;
    addReplyLongLong(c,zs->zsl->length);
}

void zscoreCommand(redisClient *c) {
    robj *o;
    zset *zs;
    dictEntry *de;

    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.nullbulk)) == NULL ||
        checkType(c,o,REDIS_ZSET)) return;

    zs = o->ptr;
    de = dictFind(zs->dict,c->argv[2]);
    if (!de) {
        addReply(c,shared.nullbulk);
    } else {
        double *score = dictGetEntryVal(de);

        addReplyDouble(c,*score);
    }
}

void zrankGenericCommand(redisClient *c, int reverse) {
    robj *o;
    zset *zs;
    zskiplist *zsl;
    dictEntry *de;
    unsigned long rank;
    double *score;

    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.nullbulk)) == NULL ||
        checkType(c,o,REDIS_ZSET)) return;

    zs = o->ptr;
    zsl = zs->zsl;
    de = dictFind(zs->dict,c->argv[2]);
    if (!de) {
        addReply(c,shared.nullbulk);
        return;
    }

    score = dictGetEntryVal(de);
    rank = zslistTypeGetRank(zsl, *score, c->argv[2]);
    if (rank) {
        if (reverse) {
            addReplyLongLong(c, zsl->length - rank);
        } else {
            addReplyLongLong(c, rank-1);
        }
    } else {
        addReply(c,shared.nullbulk);
    }
}

void zrankCommand(redisClient *c) {
    zrankGenericCommand(c, 0);
}

void zrevrankCommand(redisClient *c) {
    zrankGenericCommand(c, 1);
}
