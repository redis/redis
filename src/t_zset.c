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
    zskiplistNode *zn = zmalloc(sizeof(*zn)+level*sizeof(struct zskiplistLevel));
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
        zsl->header->level[j].forward = NULL;
        zsl->header->level[j].span = 0;
    }
    zsl->header->backward = NULL;
    zsl->tail = NULL;
    return zsl;
}

void zslFreeNode(zskiplistNode *node) {
    decrRefCount(node->obj);
    zfree(node);
}

void zslFree(zskiplist *zsl) {
    zskiplistNode *node = zsl->header->level[0].forward, *next;

    zfree(zsl->header);
    while(node) {
        next = node->level[0].forward;
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

zskiplistNode *zslInsert(zskiplist *zsl, double score, robj *obj) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    unsigned int rank[ZSKIPLIST_MAXLEVEL];
    int i, level;

    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        /* store rank that is crossed to reach the insert position */
        rank[i] = i == (zsl->level-1) ? 0 : rank[i+1];
        while (x->level[i].forward &&
            (x->level[i].forward->score < score ||
                (x->level[i].forward->score == score &&
                compareStringObjects(x->level[i].forward->obj,obj) < 0))) {
            rank[i] += x->level[i].span;
            x = x->level[i].forward;
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
            update[i]->level[i].span = zsl->length;
        }
        zsl->level = level;
    }
    x = zslCreateNode(level,score,obj);
    for (i = 0; i < level; i++) {
        x->level[i].forward = update[i]->level[i].forward;
        update[i]->level[i].forward = x;

        /* update span covered by update[i] as x is inserted here */
        x->level[i].span = update[i]->level[i].span - (rank[0] - rank[i]);
        update[i]->level[i].span = (rank[0] - rank[i]) + 1;
    }

    /* increment span for untouched levels */
    for (i = level; i < zsl->level; i++) {
        update[i]->level[i].span++;
    }

    x->backward = (update[0] == zsl->header) ? NULL : update[0];
    if (x->level[0].forward)
        x->level[0].forward->backward = x;
    else
        zsl->tail = x;
    zsl->length++;
    return x;
}

/* Internal function used by zslDelete, zslDeleteByScore and zslDeleteByRank */
void zslDeleteNode(zskiplist *zsl, zskiplistNode *x, zskiplistNode **update) {
    int i;
    for (i = 0; i < zsl->level; i++) {
        if (update[i]->level[i].forward == x) {
            update[i]->level[i].span += x->level[i].span - 1;
            update[i]->level[i].forward = x->level[i].forward;
        } else {
            update[i]->level[i].span -= 1;
        }
    }
    if (x->level[0].forward) {
        x->level[0].forward->backward = x->backward;
    } else {
        zsl->tail = x->backward;
    }
    while(zsl->level > 1 && zsl->header->level[zsl->level-1].forward == NULL)
        zsl->level--;
    zsl->length--;
}

/* Delete an element with matching score/object from the skiplist. */
int zslDelete(zskiplist *zsl, double score, robj *obj) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    int i;

    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        while (x->level[i].forward &&
            (x->level[i].forward->score < score ||
                (x->level[i].forward->score == score &&
                compareStringObjects(x->level[i].forward->obj,obj) < 0)))
            x = x->level[i].forward;
        update[i] = x;
    }
    /* We may have multiple elements with the same score, what we need
     * is to find the element with both the right score and object. */
    x = x->level[0].forward;
    if (x && score == x->score && equalStringObjects(x->obj,obj)) {
        zslDeleteNode(zsl, x, update);
        zslFreeNode(x);
        return 1;
    } else {
        return 0; /* not found */
    }
    return 0; /* not found */
}

/* Struct to hold a inclusive/exclusive range spec. */
typedef struct {
    double min, max;
    int minex, maxex; /* are min or max exclusive? */
} zrangespec;

/* Delete all the elements with score between min and max from the skiplist.
 * Min and mx are inclusive, so a score >= min || score <= max is deleted.
 * Note that this function takes the reference to the hash table view of the
 * sorted set, in order to remove the elements from the hash table too. */
unsigned long zslDeleteRangeByScore(zskiplist *zsl, zrangespec range, dict *dict) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    unsigned long removed = 0;
    int i;

    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        while (x->level[i].forward && (range.minex ?
            x->level[i].forward->score <= range.min :
            x->level[i].forward->score < range.min))
                x = x->level[i].forward;
        update[i] = x;
    }

    /* Current node is the last with score < or <= min. */
    x = x->level[0].forward;

    /* Delete nodes while in range. */
    while (x && (range.maxex ? x->score < range.max : x->score <= range.max)) {
        zskiplistNode *next = x->level[0].forward;
        zslDeleteNode(zsl,x,update);
        dictDelete(dict,x->obj);
        zslFreeNode(x);
        removed++;
        x = next;
    }
    return removed;
}

/* Delete all the elements with rank between start and end from the skiplist.
 * Start and end are inclusive. Note that start and end need to be 1-based */
unsigned long zslDeleteRangeByRank(zskiplist *zsl, unsigned int start, unsigned int end, dict *dict) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    unsigned long traversed = 0, removed = 0;
    int i;

    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        while (x->level[i].forward && (traversed + x->level[i].span) < start) {
            traversed += x->level[i].span;
            x = x->level[i].forward;
        }
        update[i] = x;
    }

    traversed++;
    x = x->level[0].forward;
    while (x && traversed <= end) {
        zskiplistNode *next = x->level[0].forward;
        zslDeleteNode(zsl,x,update);
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
        while (x->level[i].forward && x->level[i].forward->score < score)
            x = x->level[i].forward;
    }
    /* We may have multiple elements with the same score, what we need
     * is to find the element with both the right score and object. */
    return x->level[0].forward;
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
        while (x->level[i].forward &&
            (x->level[i].forward->score < score ||
                (x->level[i].forward->score == score &&
                compareStringObjects(x->level[i].forward->obj,o) <= 0))) {
            rank += x->level[i].span;
            x = x->level[i].forward;
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
        while (x->level[i].forward && (traversed + x->level[i].span) <= rank)
        {
            traversed += x->level[i].span;
            x = x->level[i].forward;
        }
        if (traversed == rank) {
            return x;
        }
    }
    return NULL;
}

/* Populate the rangespec according to the objects min and max. */
static int zslParseRange(robj *min, robj *max, zrangespec *spec) {
    char *eptr;
    spec->minex = spec->maxex = 0;

    /* Parse the min-max interval. If one of the values is prefixed
     * by the "(" character, it's considered "open". For instance
     * ZRANGEBYSCORE zset (1.5 (2.5 will match min < x < max
     * ZRANGEBYSCORE zset 1.5 2.5 will instead match min <= x <= max */
    if (min->encoding == REDIS_ENCODING_INT) {
        spec->min = (long)min->ptr;
    } else {
        if (((char*)min->ptr)[0] == '(') {
            spec->min = strtod((char*)min->ptr+1,&eptr);
            if (eptr[0] != '\0' || isnan(spec->min)) return REDIS_ERR;
            spec->minex = 1;
        } else {
            spec->min = strtod((char*)min->ptr,&eptr);
            if (eptr[0] != '\0' || isnan(spec->min)) return REDIS_ERR;
        }
    }
    if (max->encoding == REDIS_ENCODING_INT) {
        spec->max = (long)max->ptr;
    } else {
        if (((char*)max->ptr)[0] == '(') {
            spec->max = strtod((char*)max->ptr+1,&eptr);
            if (eptr[0] != '\0' || isnan(spec->max)) return REDIS_ERR;
            spec->maxex = 1;
        } else {
            spec->max = strtod((char*)max->ptr,&eptr);
            if (eptr[0] != '\0' || isnan(spec->max)) return REDIS_ERR;
        }
    }

    return REDIS_OK;
}


/*-----------------------------------------------------------------------------
 * Sorted set commands 
 *----------------------------------------------------------------------------*/

/* This generic command implements both ZADD and ZINCRBY. */
void zaddGenericCommand(redisClient *c, robj *key, robj *ele, double score, int incr) {
    robj *zsetobj;
    zset *zs;
    zskiplistNode *znode;

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

    /* Since both ZADD and ZINCRBY are implemented here, we need to increment
     * the score first by the current score if ZINCRBY is called. */
    if (incr) {
        /* Read the old score. If the element was not present starts from 0 */
        dictEntry *de = dictFind(zs->dict,ele);
        if (de != NULL)
            score += *(double*)dictGetEntryVal(de);

        if (isnan(score)) {
            addReplyError(c,"resulting score is not a number (NaN)");
            /* Note that we don't need to check if the zset may be empty and
             * should be removed here, as we can only obtain Nan as score if
             * there was already an element in the sorted set. */
            return;
        }
    }

    /* We need to remove and re-insert the element when it was already present
     * in the dictionary, to update the skiplist. Note that we delay adding a
     * pointer to the score because we want to reference the score in the
     * skiplist node. */
    if (dictAdd(zs->dict,ele,NULL) == DICT_OK) {
        dictEntry *de;

        /* New element */
        incrRefCount(ele); /* added to hash */
        znode = zslInsert(zs->zsl,score,ele);
        incrRefCount(ele); /* added to skiplist */

        /* Update the score in the dict entry */
        de = dictFind(zs->dict,ele);
        redisAssert(de != NULL);
        dictGetEntryVal(de) = &znode->score;
        touchWatchedKey(c->db,c->argv[1]);
        server.dirty++;
        if (incr)
            addReplyDouble(c,score);
        else
            addReply(c,shared.cone);
    } else {
        dictEntry *de;
        robj *curobj;
        double *curscore;
        int deleted;

        /* Update score */
        de = dictFind(zs->dict,ele);
        redisAssert(de != NULL);
        curobj = dictGetEntryKey(de);
        curscore = dictGetEntryVal(de);

        /* When the score is updated, reuse the existing string object to
         * prevent extra alloc/dealloc of strings on ZINCRBY. */
        if (score != *curscore) {
            deleted = zslDelete(zs->zsl,*curscore,curobj);
            redisAssert(deleted != 0);
            znode = zslInsert(zs->zsl,score,curobj);
            incrRefCount(curobj);

            /* Update the score in the current dict entry */
            dictGetEntryVal(de) = &znode->score;
            touchWatchedKey(c->db,c->argv[1]);
            server.dirty++;
        }
        if (incr)
            addReplyDouble(c,score);
        else
            addReply(c,shared.czero);
    }
}

void zaddCommand(redisClient *c) {
    double scoreval;
    if (getDoubleFromObjectOrReply(c,c->argv[2],&scoreval,NULL) != REDIS_OK) return;
    c->argv[3] = tryObjectEncoding(c->argv[3]);
    zaddGenericCommand(c,c->argv[1],c->argv[3],scoreval,0);
}

void zincrbyCommand(redisClient *c) {
    double scoreval;
    if (getDoubleFromObjectOrReply(c,c->argv[2],&scoreval,NULL) != REDIS_OK) return;
    c->argv[3] = tryObjectEncoding(c->argv[3]);
    zaddGenericCommand(c,c->argv[1],c->argv[3],scoreval,1);
}

void zremCommand(redisClient *c) {
    robj *zsetobj;
    zset *zs;
    dictEntry *de;
    double curscore;
    int deleted;

    if ((zsetobj = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,zsetobj,REDIS_ZSET)) return;

    zs = zsetobj->ptr;
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    de = dictFind(zs->dict,c->argv[2]);
    if (de == NULL) {
        addReply(c,shared.czero);
        return;
    }
    /* Delete from the skiplist */
    curscore = *(double*)dictGetEntryVal(de);
    deleted = zslDelete(zs->zsl,curscore,c->argv[2]);
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
    zrangespec range;
    long deleted;
    robj *o;
    zset *zs;

    /* Parse the range arguments. */
    if (zslParseRange(c->argv[2],c->argv[3],&range) != REDIS_OK) {
        addReplyError(c,"min or max is not a double");
        return;
    }

    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,REDIS_ZSET)) return;

    zs = o->ptr;
    deleted = zslDeleteRangeByScore(zs->zsl,range,zs->dict);
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
    zskiplistNode *znode;
    dictIterator *di;
    dictEntry *de;
    int touched = 0;

    /* expect setnum input keys to be given */
    setnum = atoi(c->argv[2]->ptr);
    if (setnum < 1) {
        addReplyError(c,
            "at least 1 input key is needed for ZUNIONSTORE/ZINTERSTORE");
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
                double score, value;

                score = src[0].weight * zunionInterDictValue(de);
                for (j = 1; j < setnum; j++) {
                    dictEntry *other = dictFind(src[j].dict,dictGetEntryKey(de));
                    if (other) {
                        value = src[j].weight * zunionInterDictValue(other);
                        zunionInterAggregate(&score,value,aggregate);
                    } else {
                        break;
                    }
                }

                /* Only continue when present in every source dict. */
                if (j == setnum) {
                    robj *o = dictGetEntryKey(de);
                    znode = zslInsert(dstzset->zsl,score,o);
                    incrRefCount(o); /* added to skiplist */
                    dictAdd(dstzset->dict,o,&znode->score);
                    incrRefCount(o); /* added to dictionary */
                }
            }
            dictReleaseIterator(di);
        }
    } else if (op == REDIS_OP_UNION) {
        for (i = 0; i < setnum; i++) {
            if (!src[i].dict) continue;

            di = dictGetIterator(src[i].dict);
            while((de = dictNext(di)) != NULL) {
                double score, value;

                /* skip key when already processed */
                if (dictFind(dstzset->dict,dictGetEntryKey(de)) != NULL)
                    continue;

                /* initialize score */
                score = src[i].weight * zunionInterDictValue(de);

                /* because the zsets are sorted by size, its only possible
                 * for sets at larger indices to hold this entry */
                for (j = (i+1); j < setnum; j++) {
                    dictEntry *other = dictFind(src[j].dict,dictGetEntryKey(de));
                    if (other) {
                        value = src[j].weight * zunionInterDictValue(other);
                        zunionInterAggregate(&score,value,aggregate);
                    }
                }

                robj *o = dictGetEntryKey(de);
                znode = zslInsert(dstzset->zsl,score,o);
                incrRefCount(o); /* added to skiplist */
                dictAdd(dstzset->dict,o,&znode->score);
                incrRefCount(o); /* added to dictionary */
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
            zsl->header->level[0].forward : zslistTypeGetElementByRank(zsl, start+1);
    }

    /* Return the result in form of a multi-bulk reply */
    addReplyMultiBulkLen(c,withscores ? (rangelen*2) : rangelen);
    for (j = 0; j < rangelen; j++) {
        ele = ln->obj;
        addReplyBulk(c,ele);
        if (withscores)
            addReplyDouble(c,ln->score);
        ln = reverse ? ln->backward : ln->level[0].forward;
    }
}

void zrangeCommand(redisClient *c) {
    zrangeGenericCommand(c,0);
}

void zrevrangeCommand(redisClient *c) {
    zrangeGenericCommand(c,1);
}

/* This command implements ZRANGEBYSCORE, ZREVRANGEBYSCORE and ZCOUNT.
 * If "justcount", only the number of elements in the range is returned. */
void genericZrangebyscoreCommand(redisClient *c, int reverse, int justcount) {
    list *operations;
    zrangespec range;
    robj *o, *emptyreply;
    zset *zsetobj;
    zskiplist *zsl;
    zskiplistNode *ln;
    int offset = 0, limit = -1;
    int getop = 0, withscores = 0;
    unsigned long rangelen = 0, outputlen = 0;
    void *replylen = NULL;

    /* Parse the range arguments. */
    if (zslParseRange(c->argv[2],c->argv[3],&range) != REDIS_OK) {
        addReplyError(c,"min or max is not a double");
        return;
    }

    operations = listCreate();
    listSetFreeMethod(operations,zfree);

    /* Parse optional extra arguments. Note that ZCOUNT will exactly have
     * 4 arguments, so we'll never enter the following code path. */
    if (c->argc > 4) {
        int remaining = c->argc - 4;
        int pos = 4;

        while (remaining) {
            if (remaining >= 1 && !strcasecmp(c->argv[pos]->ptr,"withscores")) {
                pos++; remaining--;
                withscores = 1;
            } else if (remaining >= 3 && !strcasecmp(c->argv[pos]->ptr,"limit")) {
                offset = atoi(c->argv[pos+1]->ptr);
                limit = atoi(c->argv[pos+2]->ptr);
                pos += 3; remaining -= 3;
            } else if (remaining >= 2 && !strcasecmp(c->argv[pos]->ptr,"get")) {
                listAddNodeTail(operations,createSortOperation(
                    REDIS_SORT_GET,c->argv[pos+1]));
                getop++;
                pos += 2; remaining -= 2;
            } else {
                listRelease(operations);
                addReply(c,shared.syntaxerr);
                return;
            }
        }
    }

    /* Ok, lookup the key and get the range */
    emptyreply = justcount ? shared.czero : shared.emptymultibulk;
    if ((o = lookupKeyReadOrReply(c,c->argv[1],emptyreply)) == NULL ||
        checkType(c,o,REDIS_ZSET)) {
        listRelease(operations);
        return;
    }
    zsetobj = o->ptr;
    zsl = zsetobj->zsl;

    /* If reversed, assume the elements are sorted from high to low score. */
    ln = zslFirstWithScore(zsl,range.min);
    if (reverse) {
        /* If range.min is out of range, ln will be NULL and we need to use
         * the tail of the skiplist as first node of the range. */
        if (ln == NULL) ln = zsl->tail;

        /* zslFirstWithScore returns the first element with where with
         * score >= range.min, so backtrack to make sure the element we use
         * here has score <= range.min. */
        while (ln && ln->score > range.min) ln = ln->backward;

        /* Move to the right element according to the range spec. */
        if (range.minex) {
            /* Find last element with score < range.min */
            while (ln && ln->score == range.min) ln = ln->backward;
        } else {
            /* Find last element with score <= range.min */
            while (ln && ln->level[0].forward &&
                         ln->level[0].forward->score == range.min)
                ln = ln->level[0].forward;
        }
    } else {
        if (range.minex) {
            /* Find first element with score > range.min */
            while (ln && ln->score == range.min) ln = ln->level[0].forward;
        }
    }

    /* No "first" element in the specified interval. */
    if (ln == NULL) {
        listRelease(operations);
        addReply(c,emptyreply);
        return;
    }

    /* We don't know in advance how many matching elements there
     * are in the list, so we push this object that will represent
     * the multi-bulk length in the output buffer, and will "fix"
     * it later */
    if (!justcount)
        replylen = addDeferredMultiBulkLength(c);

    /* If there is an offset, just traverse the number of elements without
     * checking the score because that is done in the next loop. */
    while(ln && offset--) {
        if (reverse)
            ln = ln->backward;
        else
            ln = ln->level[0].forward;
    }

    while (ln && limit--) {
        listNode *oln; // operations list node
        listIter oli; // operations list iter
        /* Check if this this element is in range. */
        if (reverse) {
            if (range.maxex) {
                /* Element should have score > range.max */
                if (ln->score <= range.max) break;
            } else {
                /* Element should have score >= range.max */
                if (ln->score < range.max) break;
            }
        } else {
            if (range.maxex) {
                /* Element should have score < range.max */
                if (ln->score >= range.max) break;
            } else {
                /* Element should have score <= range.max */
                if (ln->score > range.max) break;
            }
        }

        /* Do our magic */
        rangelen++;
        if (!justcount) {
            if (!getop) addReplyBulk(c,ln->obj);
            else {
                listRewind(operations,&oli);
                while((oln = listNext(&oli))) {
                    redisSortOperation *sop = oln->value;
                    robj *val = lookupKeyByPattern(c->db,sop->pattern,
                        ln->obj);

                    if (sop->type == REDIS_SORT_GET) {
                        if (!val) {
                            addReply(c,shared.nullbulk);
                        } else {
                            addReplyBulk(c,val);
                            decrRefCount(val);
                        }
                    } else {
                        redisAssert(sop->type == REDIS_SORT_GET); /* always fails */
                    }
                }
            }
            if (withscores)
                addReplyDouble(c,ln->score);
        }

        if (reverse)
            ln = ln->backward;
        else
            ln = ln->level[0].forward;
    }

    if (justcount) {
        addReplyLongLong(c,(long)rangelen);
    } else {
        outputlen = getop ? rangelen*getop : rangelen;
        outputlen += withscores ? rangelen : 0;
        setDeferredMultiBulkLength(c,replylen, outputlen);
    }
    listRelease(operations);
}

void zrangebyscoreCommand(redisClient *c) {
    genericZrangebyscoreCommand(c,0,0);
}

void zrevrangebyscoreCommand(redisClient *c) {
    genericZrangebyscoreCommand(c,1,0);
}

void zcountCommand(redisClient *c) {
    genericZrangebyscoreCommand(c,0,1);
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
    c->argv[2] = tryObjectEncoding(c->argv[2]);
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
    c->argv[2] = tryObjectEncoding(c->argv[2]);
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
