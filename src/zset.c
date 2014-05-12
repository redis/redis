#include "zset.h"

/* t_zset.c prototypes (there's no t_zset.h) */
unsigned char *zzlFirstInRange(unsigned char *zl, zrangespec *range);
unsigned char *zzlFind(unsigned char *zl, robj *ele, double *score);
int zzlLexValueLteMax(unsigned char *p, zlexrangespec *spec);

/* Converted from static in t_zset.c: */
int zslValueLteMax(double value, zrangespec *spec);

/* ====================================================================
 * Direct Redis DB Interaction
 * ==================================================================== */

/* zset access is mostly a copy/paste from zscoreCommand() */
bool zsetScore(robj *zobj, robj *member, double *score) {
    if (!zobj || !member)
        return false;

    if (zobj->encoding == REDIS_ENCODING_ZIPLIST) {
        if (zzlFind(zobj->ptr, member, score) == NULL)
            return false;
    } else if (zobj->encoding == REDIS_ENCODING_SKIPLIST) {
        zset *zs = zobj->ptr;
        dictEntry *de;

        member = tryObjectEncoding(member);
        de = dictFind(zs->dict, member);
        if (de != NULL) {
            *score = *(double *)dictGetVal(de);
        } else
            return false;
    } else {
        return false;
    }
    return true;
}

/* Largely extracted from genericZrangebyscoreCommand() in t_zset.c */
/* The zrangebyscoreCommand expects to only operate on a live redisClient,
 * but we need results returned to us, not sent over an async socket. */
list *geozrangebyscore(robj *zobj, double min, double max, int limit) {
    /* minex 0 = include min in range; maxex 1 = exclude max in range */
    /* That's: min <= val < max */
    zrangespec range = { .min = min, .max = max, .minex = 0, .maxex = 1 };
    list *l = NULL; /* result list */

    if (zobj->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char *zl = zobj->ptr;
        unsigned char *eptr, *sptr;
        unsigned char *vstr = NULL;
        unsigned int vlen = 0;
        long long vlong = 0;
        double score = 0;

        if ((eptr = zzlFirstInRange(zl, &range)) == NULL) {
            /* Nothing exists starting at our min.  No results. */
            return NULL;
        }

        l = listCreate();

        sptr = ziplistNext(zl, eptr);

        while (eptr && limit--) {
            score = zzlGetScore(sptr);

            /* If we fell out of range, break. */
            if (!zslValueLteMax(score, &range))
                break;

            /* We know the element exists. ziplistGet should always succeed */
            ziplistGet(eptr, &vstr, &vlen, &vlong);
            if (vstr == NULL) {
                listAddNodeTail(l, result_long(score, vlong));
            } else {
                listAddNodeTail(l, result_str(score, vstr, vlen));
            }
            zzlNext(zl, &eptr, &sptr);
        }
    } else if (zobj->encoding == REDIS_ENCODING_SKIPLIST) {
        zset *zs = zobj->ptr;
        zskiplist *zsl = zs->zsl;
        zskiplistNode *ln;

        if ((ln = zslFirstInRange(zsl, &range)) == NULL) {
            /* Nothing exists starting at our min.  No results. */
            return NULL;
        }

        l = listCreate();

        while (ln && limit--) {
            robj *o = ln->obj;
            /* Abort when the node is no longer in range. */
            if (!zslValueLteMax(ln->score, &range))
                break;

            if (o->encoding == REDIS_ENCODING_INT) {
                listAddNodeTail(l, result_long(ln->score, (long)o->ptr));
            } else {
                listAddNodeTail(l,
                                result_str(ln->score, o->ptr, sdslen(o->ptr)));
            }

            ln = ln->level[0].forward;
        }
    }
    if (l) {
        listSetFreeMethod(l, (void (*)(void *ptr)) & free_zipresult);
    }

    return l;
}

/* ====================================================================
 * Helpers
 * ==================================================================== */

/* join 'join' to 'join_to' and free 'join' container */
void listJoin(list *join_to, list *join) {
    /* If the current list has zero size, move join to become join_to.
     * If not, append the new list to the current list. */
    if (join_to->len == 0) {
        join_to->head = join->head;
    } else {
        join_to->tail->next = join->head;
        join->head->prev = join_to->tail;
        join_to->tail = join->tail;
    }

    /* Update total element count */
    join_to->len += join->len;

    /* Release original list container. Internal nodes were transferred over. */
    zfree(join);
}

/* A ziplist member may be either a long long or a string.  We create the
 * contents of our return zipresult based on what the ziplist contained. */
static struct zipresult *result(double score, long long v, unsigned char *s,
                                int len) {
    struct zipresult *r = zmalloc(sizeof(*r));

    /* If string and length, become a string. */
    /* Else, if not string or no length, become a long. */
    if (s && len >= 0)
        r->type = ZR_STRING;
    else if (!s || len < 0)
        r->type = ZR_LONG;

    r->score = score;
    switch (r->type) {
    case(ZR_LONG) :
        r->val.v = v;
        break;
    case(ZR_STRING) :
        r->val.s = sdsnewlen(s, len);
        break;
    }
    return r;
}

struct zipresult *result_long(double score, long long v) {
    return result(score, v, NULL, -1);
}

struct zipresult *result_str(double score, unsigned char *str, int len) {
    return result(score, 0, str, len);
}

void free_zipresult(struct zipresult *r) {
    if (!r)
        return;

    switch (r->type) {
    case(ZR_LONG) :
        break;
    case(ZR_STRING) :
        sdsfree(r->val.s);
        break;
    }

    zfree(r);
}
