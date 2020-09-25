/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2009-2012, Pieter Noordhuis <pcnoordhuis at gmail dot com>
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

/*-----------------------------------------------------------------------------
* Sorted set API
*----------------------------------------------------------------------------*/

/* ZSETs are ordered sets using two data structures to hold the same elements
 * in order to get O(log(N)) INSERT and REMOVE operations into a sorted
 * data structure.
 *
 * The elements are added to a hash table mapping Redis objects to scores.
 * At the same time the elements are added to a skip list mapping scores
 * to Redis objects (so objects are sorted by scores in this "view").
 *
 * Note that the SDS string representing the element is the same in both
 * the hash table and skiplist in order to save memory. What we do in order
 * to manage the shared SDS string more easily is to free the SDS string
 * only in zslFreeNode(). The dictionary has no value free method set.
 * So we should always remove an element from the dictionary, and later from
 * the skiplist.
 *
 * This skiplist implementation is almost a C translation of the original
 * algorithm described by William Pugh in "Skip Lists: A Probabilistic
 * Alternative to Balanced Trees", modified in three ways:
 * a) this implementation allows for repeated scores.
 * b) the comparison is not just by key (our 'score') but by satellite data.
 * c) there is a back pointer, so it's a doubly linked list with the back
 * pointers being only at "level 1". This allows to traverse the list
 * from tail to head, useful for ZREVRANGE. */

/* ========================================================================= */
/* -- INCLUSIONS ----------------------------------------------------------- */
/* ========================================================================= */

#include "server.h"
#include <math.h>

/* ========================================================================= */
/* -- PRIVATE DEFINITIONS -------------------------------------------------- */
/* ========================================================================= */

/*
 * Use dirty flags for pointers that need to be cleaned up in the next
 * iteration over the zsetopval. The dirty flag for the long long value is
 * special, since long long values don't need cleanup. Instead, it means that
 * we already checked that "ell" holds a long long, or tried to convert another
 * representation into a long long value. When this was successful,
 * OPVAL_VALID_LL is set as well.
 */
#define OPVAL_DIRTY_SDS    1
#define OPVAL_DIRTY_LL     2
#define OPVAL_VALID_LL     4

/* ------------------------------------------------------------------------- */

/**
 * The direction of the scan - FORWARD for ZRANGE* commands and REVERSE for all
 * ZREVRANGE* commands.
 */
typedef enum {
  ZSCAN_DIRECTION_FORWARD = 0,
  ZSCAN_DIRECTION_REVERSE = 1
} zscan_direction;

/* ------------------------------------------------------------------------- */

/**
 * The type of zrange command, representing RANGE, ZRANGEBYSCORE, and
 * RANGEBYLEX, respectively.
 */
typedef enum {
  ZRANGE_TYPE_RANK  = 0,
  ZRANGE_TYPE_SCORE = 1,
  ZRANGE_TYPE_LEX   = 2
} zrange_type;

/* ------------------------------------------------------------------------- */

/**
 * The type of sorted set aggreagte type.
 */
typedef enum {
  REDIS_AGGREGATE_TYPE_SUM = 1,
  REDIS_AGGREGATE_TYPE_MIN = 2,
  REDIS_AGGREGATE_TYPE_MAX = 3
} redis_aggregate_type;

/* ------------------------------------------------------------------------- */

/**
 * The type of sorted set aggreagte type.
 */
typedef enum {
  ZRANGE_CONSUMER_TYPE_CLIENT = 0,
  ZRANGE_CONSUMER_TYPE_INTERNAL
} zrange_consumer_type;

/* ========================================================================= */
/* -- PRIVATE MACROS ------------------------------------------------------- */
/* ========================================================================= */

#define ZSET_UNUSED(var)    ((void) var)

/* ========================================================================= */
/* -- PRIVATE TYPEDEFS ----------------------------------------------------- */
/* ========================================================================= */

typedef struct zrange_result_handler   zrange_result_handler;

typedef union _iterset                 iterset;
typedef union _iterzset                iterzset;

typedef void (*zrangeResultBeginFunction)(
  zrange_result_handler *c, int encoding);
typedef void (*zrangeResultFinalizeFunction)(
  zrange_result_handler *c, size_t result_count);
typedef void (*zrangeResultEmitCBufferFunction)(
  zrange_result_handler *c, const void *p, size_t len, double score);
typedef void (*zrangeResultEmitLongLongFunction)(
  zrange_result_handler *c, long long ll, double score);

/* ========================================================================= */
/* -- PRIVATE STRUCTURES --------------------------------------------------- */
/* ========================================================================= */

struct zrange_result_handler {
  zrange_consumer_type                 type;
  client                              *client;
  robj                                *dstkey;
  robj                                *dstobj;
  void                                *userdata;
  int                                  touched;
  int                                  withscores;
  int                                  should_emit_array_length;
  zrangeResultBeginFunction            beginResultEmission;
  zrangeResultFinalizeFunction         finalizeResultEmission;
  zrangeResultEmitCBufferFunction      emitResultFromCBuffer;
  zrangeResultEmitLongLongFunction     emitResultFromLongLong;
};

/* ------------------------------------------------------------------------- */

typedef struct {
  robj  *subject;
  int    type; /* Set, sorted set */
  int    encoding;
  double weight;

  union {
    /* Set iterators. */
    union _iterset {
      struct {
        intset *is;
        int     ii;
      } is;
      struct {
        dict         *dict;
        dictIterator *di;
        dictEntry    *de;
      } ht;
    } set;

    /* Sorted set iterators. */
    union _iterzset {
      struct {
        unsigned char *zl;
        unsigned char *eptr, *sptr;
      } zl;
      struct {
        zset          *zs;
        zskiplistNode *node;
      } sl;
    } zset;
  } iter;
} zsetopsrc;

/* ------------------------------------------------------------------------- */

/* Store value retrieved from the iterator. */
typedef struct {
  int            flags;
  unsigned char  _buf[32];  /* Private buffer. */
  sds            ele;
  unsigned char *estr;
  unsigned int   elen;
  long long      ell;
  double         score;
} zsetopval;

/* ------------------------------------------------------------------------- */

/* ========================================================================= */
/* -- IMPORTED FUNCTION PROTOTYPES ----------------------------------------- */
/* ========================================================================= */

uint64_t dictSdsHash(const void *key);
int dictSdsKeyCompare(void *privdata, const void *key1, const void *key2);

/* ========================================================================= */
/* -- STATIC FUNCTION PROTOTYPES ------------------------------------------- */
/* ========================================================================= */

static void blockingGenericZpopCommand(client *c, int where);
static void genericZrangebylexCommand(zrange_result_handler *handler,
  zlexrangespec *range, robj *zobj, long offset, long limit, int reverse);
static void genericZrangebyrankCommand(zrange_result_handler *handler,
  robj *zobj, long start, long end, int withscores, int reverse);
static void genericZrangebyscoreCommand(zrange_result_handler *handler,
  zrangespec *range, robj *zobj, int withscores, long offset, long limit,
  int reverse);
static int sdscmplex(sds a, sds b);
static void zaddGenericCommand(client *c, int flags);
static void zrankGenericCommand(client *c, int reverse);
static void zremrangeGenericCommand(client *c, int rangetype);
static void zrangeGenericCommand(zrange_result_handler *handler,
  int argc_start, zrange_type rangetype, zscan_direction direction);
static void zrangeResultBeginClient(zrange_result_handler *handler,
  int encoding);
static void zrangeResultEmitCBufferToClient(zrange_result_handler *handler,
  const void *value, size_t value_length_in_bytes, double score);
static void zrangeResultEmitLongLongToClient(
  zrange_result_handler *handler, long long value, double score);
static void zrangeResultFinalizeClient(zrange_result_handler *handler,
  size_t result_count);
static void zrangeResultBeginStore(zrange_result_handler *handler,
  int encoding);
static void zrangeResultEmitCBufferForStore(zrange_result_handler *handler,
  const void *value, size_t value_length_in_bytes, double score);
static void zrangeResultEmitLongLongForStore(
  zrange_result_handler *handler, long long value, double score);
static void zrangeResultFinalizeStore(zrange_result_handler *handler,
  size_t result_count);
static void zrangeResultHandlerInit(zrange_result_handler *handler,
  client *client, zrange_consumer_type type);
static void zrangeResultHandlerScoreEmissionEnable(
  zrange_result_handler *handler);
static void zrangeResultHandlerDestinationKeySet(
  zrange_result_handler *handler, robj *dstkey);
static zskiplistNode *zslCreateNode(int level, double score, sds ele);
static void zslDeleteNode(zskiplist *zsl, zskiplistNode *x,
  zskiplistNode **update);
static unsigned long zslDeleteRangeByLex(zskiplist *zsl, zlexrangespec *range,
  dict *dict);
static unsigned long zslDeleteRangeByRank(zskiplist *zsl, unsigned int start,
  unsigned int end, dict *dict);
static unsigned long zslDeleteRangeByScore(zskiplist *zsl, zrangespec *range,
  dict *dict);
static void zslFreeNode(zskiplistNode *node);
static int zslIsInLexRange(zskiplist *zsl, zlexrangespec *range);
static int zslIsInRange(zskiplist *zsl, zrangespec *range);
static int zslParseLexRangeItem(robj *item, sds *dest, int *ex);
static int zslParseRange(robj *min, robj *max, zrangespec *spec);
static int zslRandomLevel(void);
static zskiplistNode *zslUpdateScore(zskiplist *zsl, double curscore, sds ele,
  double newscore);
static void zuiClearIterator(zsetopsrc *op);
static int zuiCompareByCardinality(const void *s1, const void *s2);
static int zuiFind(zsetopsrc *op, zsetopval *val, double *score);
static void zuiInitIterator(zsetopsrc *op);
static unsigned long zuiLength(zsetopsrc *op);
static int zuiLongLongFromValue(zsetopval *val);
static sds zuiNewSdsFromValue(zsetopval *val);
static int zuiNext(zsetopsrc *op, zsetopval *val);
static sds zuiSdsFromValue(zsetopval *val);
static void zunionInterGenericCommand(client *c, robj *dstkey,
  int numkeysIndex, int op);
static unsigned char *zzlDelete(unsigned char *zl, unsigned char *eptr);
static unsigned char *zzlDeleteRangeByLex(unsigned char *zl,
  zlexrangespec *range, unsigned long *deleted);
static unsigned char *zzlDeleteRangeByRank(unsigned char *zl,
  unsigned int start, unsigned int end, unsigned long *deleted);
static unsigned char *zzlDeleteRangeByScore(unsigned char *zl,
  zrangespec *range, unsigned long *deleted);
static unsigned char *zzlFind(unsigned char *zl, sds ele, double *score);
static unsigned char *zzlInsertAt(unsigned char *zl, unsigned char *eptr,
  sds ele, double score);
static int zzlIsInLexRange(unsigned char *zl, zlexrangespec *range);
static int zzlIsInRange(unsigned char *zl, zrangespec *range);
static unsigned int zzlLength(unsigned char *zl);

/* ========================================================================= */
/* -- PRIVATE DATA --------------------------------------------------------- */
/* ========================================================================= */

static dictType setAccumulatorDictType =
{
  dictSdsHash,                 /* hash function */
  NULL,                        /* key dup */
  NULL,                        /* val dup */
  dictSdsKeyCompare,           /* key compare */
  NULL,                        /* key destructor */
  NULL                         /* val destructor */
};

/* ========================================================================= */
/* -- EXPORTED DATA -------------------------------------------------------- */
/* ========================================================================= */

/* ========================================================================= */
/* -- STATIC ASSERTIONS ---------------------------------------------------- */
/* ========================================================================= */

/* ========================================================================= */
/* -- EXPORTED FUNCTION DEFINITIONS ---------------------------------------- */
/* ========================================================================= */

/* Create a new skiplist. */
void
bzpopmaxCommand (
  client *c
) {
  blockingGenericZpopCommand(c, ZSET_MAX);
} /* bzpopmaxCommand() */

/* ------------------------------------------------------------------------- */

void
bzpopminCommand (
  client *c
) {
  blockingGenericZpopCommand(c, ZSET_MIN);
} /* bzpopminCommand() */

/* ------------------------------------------------------------------------- */

void
genericZpopCommand (
  client *c,
  robj  **keyv,
  int     keyc,
  int     where,
  int     emitkey,
  robj   *countarg
) {
  int idx;
  robj *key = NULL;
  robj *zobj = NULL;
  sds ele;
  double score;
  long count = 1;

  /* If a count argument as passed, parse it or return an error. */
  if (countarg) {
    if (getLongFromObjectOrReply(c, countarg, &count, NULL) != C_OK) {
      return;
    }
    if (count <= 0) {
      addReply(c, shared.emptyarray);
      return;
    }
  }

  /* Check type and break on the first error, otherwise identify candidate. */
  idx = 0;
  while (idx < keyc) {
    key = keyv[idx++];
    zobj = lookupKeyWrite(c->db, key);
    if (!zobj) {
      continue;
    }
    if (checkType(c, zobj, OBJ_ZSET)) {
      return;
    }
    break;
  }

  /* No candidate for zpopping, return empty. */
  if (!zobj) {
    addReply(c, shared.emptyarray);
    return;
  }

  void *arraylen_ptr = addReplyDeferredLen(c);
  long arraylen = 0;

  /* We emit the key only for the blocking variant. */
  if (emitkey) {
    addReplyBulk(c, key);
  }

  /* Remove the element. */
  do {
    if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
      unsigned char *zl = zobj->ptr;
      unsigned char *eptr, *sptr;
      unsigned char *vstr;
      unsigned int vlen;
      long long vlong;

      /* Get the first or last element in the sorted set. */
      eptr = ziplistIndex(zl, where == ZSET_MAX ? -2 : 0);
      serverAssertWithInfo(c, zobj, eptr != NULL);
      serverAssertWithInfo(c, zobj, ziplistGet(eptr, &vstr, &vlen, &vlong));
      if (vstr == NULL) {
        ele = sdsfromlonglong(vlong);
      } else {
        ele = sdsnewlen(vstr, vlen);
      }

      /* Get the score. */
      sptr = ziplistNext(zl, eptr);
      serverAssertWithInfo(c, zobj, sptr != NULL);
      score = zzlGetScore(sptr);
    } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
      zset *zs = zobj->ptr;
      zskiplist *zsl = zs->zsl;
      zskiplistNode *zln;

      /* Get the first or last element in the sorted set. */
      zln = (where == ZSET_MAX ? zsl->tail :
        zsl->header->level[0].forward);

      /* There must be an element in the sorted set. */
      serverAssertWithInfo(c, zobj, zln != NULL);
      ele = sdsdup(zln->ele);
      score = zln->score;
    } else {
      serverPanic("Unknown sorted set encoding");
    }

    serverAssertWithInfo(c, zobj, zsetDel(zobj, ele));
    server.dirty++;

    if (arraylen == 0) {     /* Do this only for the first iteration. */
      char *events[2] = { "zpopmin", "zpopmax" };
      notifyKeyspaceEvent(NOTIFY_ZSET, events[where], key, c->db->id);
      signalModifiedKey(c, c->db, key);
    }

    addReplyBulkCBuffer(c, ele, sdslen(ele));
    addReplyDouble(c, score);
    sdsfree(ele);
    arraylen += 2;

    /* Remove the key, if indeed needed. */
    if (zsetLength(zobj) == 0) {
      dbDelete(c->db, key);
      notifyKeyspaceEvent(NOTIFY_GENERIC, "del", key, c->db->id);
      break;
    }
  } while (--count);

  setDeferredArrayLen(c, arraylen_ptr, arraylen + (emitkey != 0));
} /* genericZpopCommand() */

/* ------------------------------------------------------------------------- */

void
zaddCommand (
  client *c
) {
  zaddGenericCommand(c, ZADD_NONE);
} /* zaddCommand() */

/* ------------------------------------------------------------------------- */

void
zcardCommand (
  client *c
) {
  robj *key = c->argv[1];
  robj *zobj;

  if (((zobj = lookupKeyReadOrReply(c, key, shared.czero)) == NULL) ||
    checkType(c, zobj, OBJ_ZSET)) {
    return;
  }

  addReplyLongLong(c, zsetLength(zobj));
} /* zcardCommand() */

/* ------------------------------------------------------------------------- */

void
zcountCommand (
  client *c
) {
  robj *key = c->argv[1];
  robj *zobj;
  zrangespec range;
  unsigned long count = 0;

  /* Parse the range arguments */
  if (zslParseRange(c->argv[2], c->argv[3], &range) != C_OK) {
    addReplyError(c, "min or max is not a float");
    return;
  }

  /* Lookup the sorted set */
  if (((zobj = lookupKeyReadOrReply(c, key, shared.czero)) == NULL) ||
    checkType(c, zobj, OBJ_ZSET)) {
    return;
  }

  if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
    unsigned char *zl = zobj->ptr;
    unsigned char *eptr, *sptr;
    double score;

    /* Use the first element in range as the starting point */
    eptr = zzlFirstInRange(zl, &range);

    /* No "first" element */
    if (eptr == NULL) {
      addReply(c, shared.czero);
      return;
    }

    /* First element is in range */
    sptr = ziplistNext(zl, eptr);
    score = zzlGetScore(sptr);
    serverAssertWithInfo(c, zobj, zslValueLteMax(score, &range));

    /* Iterate over elements in range */
    while (eptr) {
      score = zzlGetScore(sptr);

      /* Abort when the node is no longer in range. */
      if (!zslValueLteMax(score, &range)) {
        break;
      } else {
        count++;
        zzlNext(zl, &eptr, &sptr);
      }
    }
  } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
    zset *zs = zobj->ptr;
    zskiplist *zsl = zs->zsl;
    zskiplistNode *zn;
    unsigned long rank;

    /* Find first element in range */
    zn = zslFirstInRange(zsl, &range);

    /* Use rank of first element, if any, to determine preliminary count */
    if (zn != NULL) {
      rank = zslGetRank(zsl, zn->score, zn->ele);
      count = (zsl->length - (rank - 1));

      /* Find last element in range */
      zn = zslLastInRange(zsl, &range);

      /* Use rank of last element, if any, to determine the actual count */
      if (zn != NULL) {
        rank = zslGetRank(zsl, zn->score, zn->ele);
        count -= (zsl->length - rank);
      }
    }
  } else {
    serverPanic("Unknown sorted set encoding");
  }

  addReplyLongLong(c, count);
} /* zcountCommand() */

/* ------------------------------------------------------------------------- */

void
zincrbyCommand (
  client *c
) {
  zaddGenericCommand(c, ZADD_INCR);
} /* zincrbyCommand() */

/* ------------------------------------------------------------------------- */

void
zinterCommand (
  client *c
) {
  zunionInterGenericCommand(c, NULL, 1, SET_OP_INTER);
} /* zinterCommand() */

/* ------------------------------------------------------------------------- */

void
zinterstoreCommand (
  client *c
) {
  zunionInterGenericCommand(c, c->argv[1], 2, SET_OP_INTER);
} /* zinterstoreCommand() */

/* ------------------------------------------------------------------------- */

sds
ziplistGetObject (
  unsigned char *sptr
) {
  unsigned char *vstr;
  unsigned int vlen;
  long long vlong;

  serverAssert(sptr != NULL);
  serverAssert(ziplistGet(sptr, &vstr, &vlen, &vlong));

  if (vstr) {
    return (sdsnewlen((char *) vstr, vlen));
  } else {
    return (sdsfromlonglong(vlong));
  }
} /* ziplistGetObject() */

/* ------------------------------------------------------------------------- */

void
zlexcountCommand (
  client *c
) {
  robj *key = c->argv[1];
  robj *zobj;
  zlexrangespec range;
  unsigned long count = 0;

  /* Parse the range arguments */
  if (zslParseLexRange(c->argv[2], c->argv[3], &range) != C_OK) {
    addReplyError(c, "min or max not valid string range item");
    return;
  }

  /* Lookup the sorted set */
  if (((zobj = lookupKeyReadOrReply(c, key, shared.czero)) == NULL) ||
    checkType(c, zobj, OBJ_ZSET)) {
    zslFreeLexRange(&range);
    return;
  }

  if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
    unsigned char *zl = zobj->ptr;
    unsigned char *eptr, *sptr;

    /* Use the first element in range as the starting point */
    eptr = zzlFirstInLexRange(zl, &range);

    /* No "first" element */
    if (eptr == NULL) {
      zslFreeLexRange(&range);
      addReply(c, shared.czero);
      return;
    }

    /* First element is in range */
    sptr = ziplistNext(zl, eptr);
    serverAssertWithInfo(c, zobj, zzlLexValueLteMax(eptr, &range));

    /* Iterate over elements in range */
    while (eptr) {
      /* Abort when the node is no longer in range. */
      if (!zzlLexValueLteMax(eptr, &range)) {
        break;
      } else {
        count++;
        zzlNext(zl, &eptr, &sptr);
      }
    }
  } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
    zset *zs = zobj->ptr;
    zskiplist *zsl = zs->zsl;
    zskiplistNode *zn;
    unsigned long rank;

    /* Find first element in range */
    zn = zslFirstInLexRange(zsl, &range);

    /* Use rank of first element, if any, to determine preliminary count */
    if (zn != NULL) {
      rank = zslGetRank(zsl, zn->score, zn->ele);
      count = (zsl->length - (rank - 1));

      /* Find last element in range */
      zn = zslLastInLexRange(zsl, &range);

      /* Use rank of last element, if any, to determine the actual count */
      if (zn != NULL) {
        rank = zslGetRank(zsl, zn->score, zn->ele);
        count -= (zsl->length - rank);
      }
    }
  } else {
    serverPanic("Unknown sorted set encoding");
  }

  zslFreeLexRange(&range);
  addReplyLongLong(c, count);
} /* zlexcountCommand() */

/* ------------------------------------------------------------------------- */

void
zmscoreCommand (
  client *c
) {
  robj *key = c->argv[1];
  robj *zobj;
  double score;

  zobj = lookupKeyRead(c->db, key);
  if (checkType(c, zobj, OBJ_ZSET)) {
    return;
  }

  addReplyArrayLen(c, c->argc - 2);
  for (int j = 2; j < c->argc; j++) {
    /* Treat a missing set the same way as an empty set */
    if ((zobj == NULL) ||
      (zsetScore(zobj, c->argv[j]->ptr, &score) == C_ERR)) {
      addReplyNull(c);
    } else {
      addReplyDouble(c, score);
    }
  }
} /* zmscoreCommand() */

/* ------------------------------------------------------------------------- */

void
zpopmaxCommand (
  client *c
) {
  if (c->argc > 3) {
    addReply(c, shared.syntaxerr);
    return;
  }
  genericZpopCommand(c, &c->argv[1], 1, ZSET_MAX, 0,
    c->argc == 3 ? c->argv[2] : NULL);
} /* zpopmaxCommand() */

/* ------------------------------------------------------------------------- */

void
zpopminCommand (
  client *c
) {
  if (c->argc > 3) {
    addReply(c, shared.syntaxerr);
    return;
  }
  genericZpopCommand(c, &c->argv[1], 1, ZSET_MIN, 0,
    c->argc == 3 ? c->argv[2] : NULL);
} /* zpopminCommand() */

/* ------------------------------------------------------------------------- */

void
zrangeCommand (
  client *c
) {
  zrange_result_handler handler;

  zrangeResultHandlerInit(&handler, c, ZRANGE_CONSUMER_TYPE_CLIENT);
  zrangeGenericCommand(&handler, 0, ZRANGE_TYPE_RANK, ZSCAN_DIRECTION_FORWARD);
} /* zrangeCommand() */

/* ------------------------------------------------------------------------- */

void
zrangebylexCommand (
  client *c
) {
  zrange_result_handler handler;

  zrangeResultHandlerInit(&handler, c, ZRANGE_CONSUMER_TYPE_CLIENT);
  zrangeGenericCommand(&handler, 0, ZRANGE_TYPE_LEX, ZSCAN_DIRECTION_FORWARD);
} /* zrangebylexCommand() */

/* ------------------------------------------------------------------------- */

void
zrangebyscoreCommand (
  client *c
) {
  zrange_result_handler handler;

  zrangeResultHandlerInit(&handler, c, ZRANGE_CONSUMER_TYPE_CLIENT);
  zrangeGenericCommand(&handler, 0, ZRANGE_TYPE_SCORE,
    ZSCAN_DIRECTION_FORWARD);
} /* zrangebyscoreCommand() */

/* ------------------------------------------------------------------------- */

void
zrangestoreCommand (
  client *c
) {
  static struct {
    char           *command;
    zrange_type     rangetype;
    zscan_direction direction;
  }
  zrange_commands[] =
  {
    {
      .command = "zrange",
      .rangetype = ZRANGE_TYPE_RANK,
      .direction = ZSCAN_DIRECTION_FORWARD
    },
    {
      .command = "zrevrange",
      .rangetype = ZRANGE_TYPE_RANK,
      .direction = ZSCAN_DIRECTION_REVERSE
    },
    {
      .command = "zrangebyscore",
      .rangetype = ZRANGE_TYPE_SCORE,
      .direction = ZSCAN_DIRECTION_FORWARD
    },
    {
      .command = "zrevrangebyscore",
      .rangetype = ZRANGE_TYPE_SCORE,
      .direction = ZSCAN_DIRECTION_REVERSE
    },
    {
      .command = "zrangebylex",
      .rangetype = ZRANGE_TYPE_LEX,
      .direction = ZSCAN_DIRECTION_FORWARD
    },
    {
      .command = "zrevrangebylex",
      .rangetype = ZRANGE_TYPE_LEX,
      .direction = ZSCAN_DIRECTION_REVERSE
    },
  };

  robj *dstkey = c->argv[1];
  int command = -1;
  zrange_result_handler handler;

  for (size_t i = 0
    ; i < (sizeof(zrange_commands)/sizeof(zrange_commands[0]))
    ; ++i) {
    if (!strcasecmp(c->argv[2]->ptr, zrange_commands[i].command)) {
      command = (int) i;
      break;
    }
  }

  if (0 > command) {
    addReply(c, shared.syntaxerr);
    return;
  }

  zrangeResultHandlerInit(&handler, c, ZRANGE_CONSUMER_TYPE_INTERNAL);
  zrangeResultHandlerDestinationKeySet(&handler, dstkey);
  zrangeGenericCommand(&handler, 2, zrange_commands[command].rangetype,
    zrange_commands[command].direction);
} /* zrangestoreCommand() */

/* ------------------------------------------------------------------------- */

void
zrankCommand (
  client *c
) {
  zrankGenericCommand(c, 0);
} /* zrankCommand() */

/* ------------------------------------------------------------------------- */

void
zremCommand (
  client *c
) {
  robj *key = c->argv[1];
  robj *zobj;
  int deleted = 0, keyremoved = 0, j;

  if (((zobj = lookupKeyWriteOrReply(c, key, shared.czero)) == NULL) ||
    checkType(c, zobj, OBJ_ZSET)) {
    return;
  }

  for (j = 2; j < c->argc; j++) {
    if (zsetDel(zobj, c->argv[j]->ptr)) {
      deleted++;
    }
    if (zsetLength(zobj) == 0) {
      dbDelete(c->db, key);
      keyremoved = 1;
      break;
    }
  }

  if (deleted) {
    notifyKeyspaceEvent(NOTIFY_ZSET, "zrem", key, c->db->id);
    if (keyremoved) {
      notifyKeyspaceEvent(NOTIFY_GENERIC, "del", key, c->db->id);
    }
    signalModifiedKey(c, c->db, key);
    server.dirty += deleted;
  }
  addReplyLongLong(c, deleted);
} /* zremCommand() */

/* ------------------------------------------------------------------------- */

void
zremrangebylexCommand (
  client *c
) {
  zremrangeGenericCommand(c, ZRANGE_TYPE_LEX);
} /* zremrangebylexCommand() */

/* ------------------------------------------------------------------------- */

void
zremrangebyrankCommand (
  client *c
) {
  zremrangeGenericCommand(c, ZRANGE_TYPE_RANK);
} /* zremrangebyrankCommand() */

/* ------------------------------------------------------------------------- */

void
zremrangebyscoreCommand (
  client *c
) {
  zremrangeGenericCommand(c, ZRANGE_TYPE_SCORE);
} /* zremrangebyscoreCommand() */

/* ------------------------------------------------------------------------- */

void
zrevrangeCommand (
  client *c
) {
  zrange_result_handler handler;

  zrangeResultHandlerInit(&handler, c, ZRANGE_CONSUMER_TYPE_CLIENT);
  zrangeGenericCommand(&handler, 0, ZRANGE_TYPE_RANK, ZSCAN_DIRECTION_REVERSE);
} /* zrevrangeCommand() */

/* ------------------------------------------------------------------------- */

void
zrevrangebylexCommand (
  client *c
) {
  zrange_result_handler handler;

  zrangeResultHandlerInit(&handler, c, ZRANGE_CONSUMER_TYPE_CLIENT);
  zrangeGenericCommand(&handler, 0, ZRANGE_TYPE_LEX, ZSCAN_DIRECTION_REVERSE);
} /* zrevrangebylexCommand() */

/* ------------------------------------------------------------------------- */

void
zrevrangebyscoreCommand (
  client *c
) {
  zrange_result_handler handler;

  zrangeResultHandlerInit(&handler, c, ZRANGE_CONSUMER_TYPE_CLIENT);
  zrangeGenericCommand(&handler, 0, ZRANGE_TYPE_SCORE,
    ZSCAN_DIRECTION_REVERSE);
} /* zrevrangebyscoreCommand() */

/* ------------------------------------------------------------------------- */

void
zrevrankCommand (
  client *c
) {
  zrankGenericCommand(c, 1);
} /* zrevrankCommand() */

/* ------------------------------------------------------------------------- */

void
zscanCommand (
  client *c
) {
  robj *o;
  unsigned long cursor;

  if (parseScanCursorOrReply(c, c->argv[2], &cursor) == C_ERR) {
    return;
  }
  if (((o = lookupKeyReadOrReply(c, c->argv[1], shared.emptyscan)) == NULL) ||
    checkType(c, o, OBJ_ZSET)) {
    return;
  }
  scanGenericCommand(c, o, cursor);
} /* zscanCommand() */

/* ------------------------------------------------------------------------- */

void
zscoreCommand (
  client *c
) {
  robj *key = c->argv[1];
  robj *zobj;
  double score;

  if (((zobj = lookupKeyReadOrReply(c, key, shared.null[c->resp])) == NULL) ||
    checkType(c, zobj, OBJ_ZSET)) {
    return;
  }

  if (zsetScore(zobj, c->argv[2]->ptr, &score) == C_ERR) {
    addReplyNull(c);
  } else {
    addReplyDouble(c, score);
  }
} /* zscoreCommand() */

/* ------------------------------------------------------------------------- */

int
zsetAdd (
  robj   *zobj,
  double  score,
  sds     ele,
  int    *flags,
  double *newscore
) {
  /* Turn options into simple to check vars. */
  int incr = (*flags & ZADD_INCR) != 0;
  int nx = (*flags & ZADD_NX) != 0;
  int xx = (*flags & ZADD_XX) != 0;
  int gt = (*flags & ZADD_GT) != 0;
  int lt = (*flags & ZADD_LT) != 0;

  *flags = 0;   /* We'll return our response flags. */
  double curscore;

  /* NaN as input is an error regardless of all the other parameters. */
  if (isnan(score)) {
    *flags = ZADD_NAN;
    return (0);
  }

  /* Update the sorted set according to its encoding. */
  if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
    unsigned char *eptr;

    if ((eptr = zzlFind(zobj->ptr, ele, &curscore)) != NULL) {
      /* NX? Return, same element already exists. */
      if (nx) {
        *flags |= ZADD_NOP;
        return (1);
      }

      /* Prepare the score for the increment if needed. */
      if (incr) {
        score += curscore;
        if (isnan(score)) {
          *flags |= ZADD_NAN;
          return (0);
        }
        if (newscore) {
          *newscore = score;
        }
      }

      /* Remove and re-insert when score changed. */
      if (score != curscore &&
        /* LT? Only update if score is less than current. */
        (!lt || score < curscore) &&
        /* GT? Only update if score is greater than current. */
        (!gt || score > curscore)) {
        zobj->ptr = zzlDelete(zobj->ptr, eptr);
        zobj->ptr = zzlInsert(zobj->ptr, ele, score);
        *flags |= ZADD_UPDATED;
      }
      return (1);
    } else if (!xx) {
      /* Optimize: check if the element is too large or the list
       * becomes too long *before* executing zzlInsert. */
      zobj->ptr = zzlInsert(zobj->ptr, ele, score);
      if ((zzlLength(zobj->ptr) > server.zset_max_ziplist_entries) ||
        (sdslen(ele) > server.zset_max_ziplist_value)) {
        zsetConvert(zobj, OBJ_ENCODING_SKIPLIST);
      }
      if (newscore) {
        *newscore = score;
      }
      *flags |= ZADD_ADDED;
      return (1);
    } else {
      *flags |= ZADD_NOP;
      return (1);
    }
  } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
    zset *zs = zobj->ptr;
    zskiplistNode *znode;
    dictEntry *de;

    de = dictFind(zs->dict, ele);
    if (de != NULL) {
      /* NX? Return, same element already exists. */
      if (nx) {
        *flags |= ZADD_NOP;
        return (1);
      }
      curscore = *(double *) dictGetVal(de);

      /* Prepare the score for the increment if needed. */
      if (incr) {
        score += curscore;
        if (isnan(score)) {
          *flags |= ZADD_NAN;
          return (0);
        }
        if (newscore) {
          *newscore = score;
        }
      }

      /* Remove and re-insert when score changes. */
      if (score != curscore &&
        /* LT? Only update if score is less than current. */
        (!lt || score < curscore) &&
        /* GT? Only update if score is greater than current. */
        (!gt || score > curscore)) {
        znode = zslUpdateScore(zs->zsl, curscore, ele, score);

        /* Note that we did not removed the original element from
         * the hash table representing the sorted set, so we just
         * update the score. */
        dictGetVal(de) = &znode->score;         /* Update score ptr. */
        *flags |= ZADD_UPDATED;
      }
      return (1);
    } else if (!xx) {
      ele = sdsdup(ele);
      znode = zslInsert(zs->zsl, score, ele);
      serverAssert(dictAdd(zs->dict, ele, &znode->score) == DICT_OK);
      *flags |= ZADD_ADDED;
      if (newscore) {
        *newscore = score;
      }
      return (1);
    } else {
      *flags |= ZADD_NOP;
      return (1);
    }
  } else {
    serverPanic("Unknown sorted set encoding");
  }
  return (0); /* Never reached. */
} /* zsetAdd() */

/* ------------------------------------------------------------------------- */

void
zsetConvert (
  robj *zobj,
  int   encoding
) {
  zset *zs;
  zskiplistNode *node, *next;
  sds ele;
  double score;

  if (zobj->encoding == encoding) {
    return;
  }
  if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
    unsigned char *zl = zobj->ptr;
    unsigned char *eptr, *sptr;
    unsigned char *vstr;
    unsigned int vlen;
    long long vlong;

    if (encoding != OBJ_ENCODING_SKIPLIST) {
      serverPanic("Unknown target encoding");
    }

    zs = zmalloc(sizeof(*zs));
    zs->dict = dictCreate(&zsetDictType, NULL);
    zs->zsl = zslCreate();

    eptr = ziplistIndex(zl, 0);
    serverAssertWithInfo(NULL, zobj, eptr != NULL);
    sptr = ziplistNext(zl, eptr);
    serverAssertWithInfo(NULL, zobj, sptr != NULL);

    while (eptr != NULL) {
      score = zzlGetScore(sptr);
      serverAssertWithInfo(NULL, zobj, ziplistGet(eptr, &vstr, &vlen, &vlong));
      if (vstr == NULL) {
        ele = sdsfromlonglong(vlong);
      } else {
        ele = sdsnewlen((char *) vstr, vlen);
      }

      node = zslInsert(zs->zsl, score, ele);
      serverAssert(dictAdd(zs->dict, ele, &node->score) == DICT_OK);
      zzlNext(zl, &eptr, &sptr);
    }

    zfree(zobj->ptr);
    zobj->ptr = zs;
    zobj->encoding = OBJ_ENCODING_SKIPLIST;
  } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
    unsigned char *zl = ziplistNew();

    if (encoding != OBJ_ENCODING_ZIPLIST) {
      serverPanic("Unknown target encoding");
    }

    /* Approach similar to zslFree(), since we want to free the skiplist at
     * the same time as creating the ziplist. */
    zs = zobj->ptr;
    dictRelease(zs->dict);
    node = zs->zsl->header->level[0].forward;
    zfree(zs->zsl->header);
    zfree(zs->zsl);

    while (node) {
      zl = zzlInsertAt(zl, NULL, node->ele, node->score);
      next = node->level[0].forward;
      zslFreeNode(node);
      node = next;
    }

    zfree(zs);
    zobj->ptr = zl;
    zobj->encoding = OBJ_ENCODING_ZIPLIST;
  } else {
    serverPanic("Unknown sorted set encoding");
  }
} /* zsetConvert() */

/* ------------------------------------------------------------------------- */

void
zsetConvertToZiplistIfNeeded (
  robj  *zobj,
  size_t maxelelen
) {
  if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
    return;
  }
  zset *zset = zobj->ptr;

  if ((zset->zsl->length <= server.zset_max_ziplist_entries) &&
    (maxelelen <= server.zset_max_ziplist_value)) {
    zsetConvert(zobj, OBJ_ENCODING_ZIPLIST);
  }
} /* zsetConvertToZiplistIfNeeded() */

/* ------------------------------------------------------------------------- */

int
zsetDel (
  robj *zobj,
  sds   ele
) {
  if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
    unsigned char *eptr;

    if ((eptr = zzlFind(zobj->ptr, ele, NULL)) != NULL) {
      zobj->ptr = zzlDelete(zobj->ptr, eptr);
      return (1);
    }
  } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
    zset *zs = zobj->ptr;
    dictEntry *de;
    double score;

    de = dictUnlink(zs->dict, ele);
    if (de != NULL) {
      /* Get the score in order to delete from the skiplist later. */
      score = *(double *) dictGetVal(de);

      /* Delete from the hash table and later from the skiplist.
       * Note that the order is important: deleting from the skiplist
       * actually releases the SDS string representing the element,
       * which is shared between the skiplist and the hash table, so
       * we need to delete from the skiplist as the final step. */
      dictFreeUnlinkedEntry(zs->dict, de);

      /* Delete from skiplist. */
      int retval = zslDelete(zs->zsl, score, ele, NULL);
      serverAssert(retval);

      if (htNeedsResize(zs->dict)) {
        dictResize(zs->dict);
      }
      return (1);
    }
  } else {
    serverPanic("Unknown sorted set encoding");
  }
  return (0); /* No such element found. */
} /* zsetDel() */

/* ------------------------------------------------------------------------- */

unsigned long
zsetLength (
  const robj *zobj
) {
  unsigned long length = 0;

  if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
    length = zzlLength(zobj->ptr);
  } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
    length = ((const zset *) zobj->ptr)->zsl->length;
  } else {
    serverPanic("Unknown sorted set encoding");
  }
  return (length);
} /* zsetLength() */

/* ------------------------------------------------------------------------- */

long
zsetRank (
  robj *zobj,
  sds   ele,
  int   reverse
) {
  unsigned long llen;
  unsigned long rank;

  llen = zsetLength(zobj);

  if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
    unsigned char *zl = zobj->ptr;
    unsigned char *eptr, *sptr;

    eptr = ziplistIndex(zl, 0);
    serverAssert(eptr != NULL);
    sptr = ziplistNext(zl, eptr);
    serverAssert(sptr != NULL);

    rank = 1;
    while (eptr != NULL) {
      if (ziplistCompare(eptr, (unsigned char *) ele, sdslen(ele))) {
        break;
      }
      rank++;
      zzlNext(zl, &eptr, &sptr);
    }

    if (eptr != NULL) {
      if (reverse) {
        return (llen-rank);
      } else {
        return (rank-1);
      }
    } else {
      return (-1);
    }
  } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
    zset *zs = zobj->ptr;
    zskiplist *zsl = zs->zsl;
    dictEntry *de;
    double score;

    de = dictFind(zs->dict, ele);
    if (de != NULL) {
      score = *(double *) dictGetVal(de);
      rank = zslGetRank(zsl, score, ele);
      /* Existing elements always have a rank. */
      serverAssert(rank != 0);
      if (reverse) {
        return (llen-rank);
      } else {
        return (rank-1);
      }
    } else {
      return (-1);
    }
  } else {
    serverPanic("Unknown sorted set encoding");
  }
} /* zsetRank() */

/* ------------------------------------------------------------------------- */

int
zsetScore (
  robj   *zobj,
  sds     member,
  double *score
) {
  if (!zobj || !member) {
    return (C_ERR);
  }

  if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
    if (zzlFind(zobj->ptr, member, score) == NULL) {
      return (C_ERR);
    }
  } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
    zset *zs = zobj->ptr;
    dictEntry *de = dictFind(zs->dict, member);
    if (de == NULL) {
      return (C_ERR);
    }
    *score = *(double *) dictGetVal(de);
  } else {
    serverPanic("Unknown sorted set encoding");
  }
  return (C_OK);
} /* zsetScore() */

/* ------------------------------------------------------------------------- */

zskiplist *
zslCreate (
  void
) {
  int j;
  zskiplist *zsl;

  zsl = zmalloc(sizeof(*zsl));
  zsl->level = 1;
  zsl->length = 0;
  zsl->header = zslCreateNode(ZSKIPLIST_MAXLEVEL, 0, NULL);
  for (j = 0; j < ZSKIPLIST_MAXLEVEL; j++) {
    zsl->header->level[j].forward = NULL;
    zsl->header->level[j].span = 0;
  }
  zsl->header->backward = NULL;
  zsl->tail = NULL;
  return (zsl);
} /* zslCreate() */

/* ------------------------------------------------------------------------- */

int
zslDelete (
  zskiplist      *zsl,
  double          score,
  sds             ele,
  zskiplistNode **node
) {
  zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
  int i;

  x = zsl->header;
  for (i = zsl->level-1; i >= 0; i--) {
    while (x->level[i].forward &&
      (x->level[i].forward->score < score ||
      (x->level[i].forward->score == score &&
      sdscmp(x->level[i].forward->ele, ele) < 0))) {
      x = x->level[i].forward;
    }
    update[i] = x;
  }

  /* We may have multiple elements with the same score, what we need
   * is to find the element with both the right score and object. */
  x = x->level[0].forward;
  if (x && (score == x->score) && (sdscmp(x->ele, ele) == 0)) {
    zslDeleteNode(zsl, x, update);
    if (!node) {
      zslFreeNode(x);
    } else {
      *node = x;
    }
    return (1);
  }
  return (0); /* not found */
} /* zslDelete() */

/* ------------------------------------------------------------------------- */

zskiplistNode *
zslFirstInLexRange (
  zskiplist     *zsl,
  zlexrangespec *range
) {
  zskiplistNode *x;
  int i;

  /* If everything is out of range, return early. */
  if (!zslIsInLexRange(zsl, range)) {
    return (NULL);
  }

  x = zsl->header;
  for (i = zsl->level-1; i >= 0; i--) {
    /* Go forward while *OUT* of range. */
    while (x->level[i].forward &&
      !zslLexValueGteMin(x->level[i].forward->ele, range)) {
      x = x->level[i].forward;
    }
  }

  /* This is an inner range, so the next node cannot be NULL. */
  x = x->level[0].forward;
  serverAssert(x != NULL);

  /* Check if score <= max. */
  if (!zslLexValueLteMax(x->ele, range)) {
    return (NULL);
  }
  return (x);
} /* zslFirstInLexRange() */

/* ------------------------------------------------------------------------- */

zskiplistNode *
zslFirstInRange (
  zskiplist  *zsl,
  zrangespec *range
) {
  zskiplistNode *x;
  int i;

  /* If everything is out of range, return early. */
  if (!zslIsInRange(zsl, range)) {
    return (NULL);
  }

  x = zsl->header;
  for (i = zsl->level-1; i >= 0; i--) {
    /* Go forward while *OUT* of range. */
    while (x->level[i].forward &&
      !zslValueGteMin(x->level[i].forward->score, range)) {
      x = x->level[i].forward;
    }
  }

  /* This is an inner range, so the next node cannot be NULL. */
  x = x->level[0].forward;
  serverAssert(x != NULL);

  /* Check if score <= max. */
  if (!zslValueLteMax(x->score, range)) {
    return (NULL);
  }
  return (x);
} /* zslFirstInRange() */

/* ------------------------------------------------------------------------- */

void
zslFree (
  zskiplist *zsl
) {
  zskiplistNode *node = zsl->header->level[0].forward, *next;

  zfree(zsl->header);
  while (node) {
    next = node->level[0].forward;
    zslFreeNode(node);
    node = next;
  }
  zfree(zsl);
} /* zslFree() */

/* ------------------------------------------------------------------------- */

void
zslFreeLexRange (
  zlexrangespec *spec
) {
  if ((spec->min != shared.minstring) &&
    (spec->min != shared.maxstring)) {
    sdsfree(spec->min);
  }
  if ((spec->max != shared.minstring) &&
    (spec->max != shared.maxstring)) {
    sdsfree(spec->max);
  }
} /* zslFreeLexRange() */

/* ------------------------------------------------------------------------- */

zskiplistNode *
zslGetElementByRank (
  zskiplist    *zsl,
  unsigned long rank
) {
  zskiplistNode *x;
  unsigned long traversed = 0;
  int i;

  x = zsl->header;
  for (i = zsl->level-1; i >= 0; i--) {
    while (x->level[i].forward && (traversed + x->level[i].span) <= rank) {
      traversed += x->level[i].span;
      x = x->level[i].forward;
    }
    if (traversed == rank) {
      return (x);
    }
  }
  return (NULL);
} /* zslGetElementByRank() */

/* ------------------------------------------------------------------------- */

unsigned long
zslGetRank (
  zskiplist *zsl,
  double     score,
  sds        ele
) {
  zskiplistNode *x;
  unsigned long rank = 0;
  int i;

  x = zsl->header;
  for (i = zsl->level-1; i >= 0; i--) {
    while (x->level[i].forward &&
      (x->level[i].forward->score < score ||
      (x->level[i].forward->score == score &&
      sdscmp(x->level[i].forward->ele, ele) <= 0))) {
      rank += x->level[i].span;
      x = x->level[i].forward;
    }

    /* x might be equal to zsl->header, so test if obj is non-NULL */
    if (x->ele && (sdscmp(x->ele, ele) == 0)) {
      return (rank);
    }
  }
  return (0);
} /* zslGetRank() */

/* ------------------------------------------------------------------------- */

zskiplistNode *
zslInsert (
  zskiplist *zsl,
  double     score,
  sds        ele
) {
  zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
  unsigned int rank[ZSKIPLIST_MAXLEVEL];
  int i, level;

  serverAssert(!isnan(score));
  x = zsl->header;
  for (i = zsl->level-1; i >= 0; i--) {
    /* store rank that is crossed to reach the insert position */
    rank[i] = i == (zsl->level-1) ? 0 : rank[i+1];
    while (x->level[i].forward &&
      (x->level[i].forward->score < score ||
      (x->level[i].forward->score == score &&
      sdscmp(x->level[i].forward->ele, ele) < 0))) {
      rank[i] += x->level[i].span;
      x = x->level[i].forward;
    }
    update[i] = x;
  }

  /* we assume the element is not already inside, since we allow duplicated
   * scores, reinserting the same element should never happen since the
   * caller of zslInsert() should test in the hash table if the element is
   * already inside or not. */
  level = zslRandomLevel();
  if (level > zsl->level) {
    for (i = zsl->level; i < level; i++) {
      rank[i] = 0;
      update[i] = zsl->header;
      update[i]->level[i].span = zsl->length;
    }
    zsl->level = level;
  }
  x = zslCreateNode(level, score, ele);
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
  if (x->level[0].forward) {
    x->level[0].forward->backward = x;
  } else {
    zsl->tail = x;
  }
  zsl->length++;
  return (x);
} /* zslInsert() */

/* ------------------------------------------------------------------------- */

zskiplistNode *
zslLastInLexRange (
  zskiplist     *zsl,
  zlexrangespec *range
) {
  zskiplistNode *x;
  int i;

  /* If everything is out of range, return early. */
  if (!zslIsInLexRange(zsl, range)) {
    return (NULL);
  }

  x = zsl->header;
  for (i = zsl->level-1; i >= 0; i--) {
    /* Go forward while *IN* range. */
    while (x->level[i].forward &&
      zslLexValueLteMax(x->level[i].forward->ele, range)) {
      x = x->level[i].forward;
    }
  }

  /* This is an inner range, so this node cannot be NULL. */
  serverAssert(x != NULL);

  /* Check if score >= min. */
  if (!zslLexValueGteMin(x->ele, range)) {
    return (NULL);
  }
  return (x);
} /* zslLastInLexRange() */

/* ------------------------------------------------------------------------- */

zskiplistNode *
zslLastInRange (
  zskiplist  *zsl,
  zrangespec *range
) {
  zskiplistNode *x;
  int i;

  /* If everything is out of range, return early. */
  if (!zslIsInRange(zsl, range)) {
    return (NULL);
  }

  x = zsl->header;
  for (i = zsl->level-1; i >= 0; i--) {
    /* Go forward while *IN* range. */
    while (x->level[i].forward &&
      zslValueLteMax(x->level[i].forward->score, range)) {
      x = x->level[i].forward;
    }
  }

  /* This is an inner range, so this node cannot be NULL. */
  serverAssert(x != NULL);

  /* Check if score >= min. */
  if (!zslValueGteMin(x->score, range)) {
    return (NULL);
  }
  return (x);
} /* zslLastInRange() */

/* ------------------------------------------------------------------------- */

int
zslLexValueGteMin (
  sds            value,
  zlexrangespec *spec
) {
  return (spec->minex ?
         (sdscmplex(value, spec->min) > 0) :
         (sdscmplex(value, spec->min) >= 0));
} /* zslLexValueGteMin() */

/* ------------------------------------------------------------------------- */

int
zslLexValueLteMax (
  sds            value,
  zlexrangespec *spec
) {
  return (spec->maxex ?
         (sdscmplex(value, spec->max) < 0) :
         (sdscmplex(value, spec->max) <= 0));
} /* zslLexValueLteMax() */

/* ------------------------------------------------------------------------- */

int
zslParseLexRange (
  robj          *min,
  robj          *max,
  zlexrangespec *spec
) {
  /* The range can't be valid if objects are integer encoded.
   * Every item must start with ( or [. */
  if ((min->encoding == OBJ_ENCODING_INT) ||
    (max->encoding == OBJ_ENCODING_INT)) {
    return (C_ERR);
  }

  spec->min = spec->max = NULL;
  if ((zslParseLexRangeItem(min, &spec->min, &spec->minex) == C_ERR) ||
    (zslParseLexRangeItem(max, &spec->max, &spec->maxex) == C_ERR)) {
    zslFreeLexRange(spec);
    return (C_ERR);
  } else {
    return (C_OK);
  }
} /* zslParseLexRange() */

/* ------------------------------------------------------------------------- */

int
zslValueGteMin (
  double      value,
  zrangespec *spec
) {
  return (spec->minex ? (value > spec->min) : (value >= spec->min));
} /* zslValueGteMin() */

/* ------------------------------------------------------------------------- */

int
zslValueLteMax (
  double      value,
  zrangespec *spec
) {
  return (spec->maxex ? (value < spec->max) : (value <= spec->max));
} /* zslValueLteMax() */

/* ------------------------------------------------------------------------- */

void
zunionCommand (
  client *c
) {
  zunionInterGenericCommand(c, NULL, 1, SET_OP_UNION);
} /* zunionCommand() */

/* ------------------------------------------------------------------------- */

void
zunionstoreCommand (
  client *c
) {
  zunionInterGenericCommand(c, c->argv[1], 2, SET_OP_UNION);
} /* zunionstoreCommand() */

/* ------------------------------------------------------------------------- */

int
zzlCompareElements (
  unsigned char *eptr,
  unsigned char *cstr,
  unsigned int   clen
) {
  unsigned char *vstr;
  unsigned int vlen;
  long long vlong;
  unsigned char vbuf[32];
  int minlen, cmp;

  serverAssert(ziplistGet(eptr, &vstr, &vlen, &vlong));
  if (vstr == NULL) {
    /* Store string representation of long long in buf. */
    vlen = ll2string((char *) vbuf, sizeof(vbuf), vlong);
    vstr = vbuf;
  }

  minlen = (vlen < clen) ? vlen : clen;
  cmp = memcmp(vstr, cstr, minlen);
  if (cmp == 0) {
    return (vlen-clen);
  }
  return (cmp);
} /* zzlCompareElements() */

/* ------------------------------------------------------------------------- */

unsigned char *
zzlFirstInLexRange (
  unsigned char *zl,
  zlexrangespec *range
) {
  unsigned char *eptr = ziplistIndex(zl, 0), *sptr;

  /* If everything is out of range, return early. */
  if (!zzlIsInLexRange(zl, range)) {
    return (NULL);
  }

  while (eptr != NULL) {
    if (zzlLexValueGteMin(eptr, range)) {
      /* Check if score <= max. */
      if (zzlLexValueLteMax(eptr, range)) {
        return (eptr);
      }
      return (NULL);
    }

    /* Move to next element. */
    sptr = ziplistNext(zl, eptr);    /* This element score. Skip it. */
    serverAssert(sptr != NULL);
    eptr = ziplistNext(zl, sptr);    /* Next element. */
  }

  return (NULL);
} /* zzlFirstInLexRange() */

/* ------------------------------------------------------------------------- */

unsigned char *
zzlFirstInRange (
  unsigned char *zl,
  zrangespec    *range
) {
  unsigned char *eptr = ziplistIndex(zl, 0), *sptr;
  double score;

  /* If everything is out of range, return early. */
  if (!zzlIsInRange(zl, range)) {
    return (NULL);
  }

  while (eptr != NULL) {
    sptr = ziplistNext(zl, eptr);
    serverAssert(sptr != NULL);

    score = zzlGetScore(sptr);
    if (zslValueGteMin(score, range)) {
      /* Check if score <= max. */
      if (zslValueLteMax(score, range)) {
        return (eptr);
      }
      return (NULL);
    }

    /* Move to next element. */
    eptr = ziplistNext(zl, sptr);
  }

  return (NULL);
} /* zzlFirstInRange() */

/* ------------------------------------------------------------------------- */

double
zzlGetScore (
  unsigned char *sptr
) {
  unsigned char *vstr;
  unsigned int vlen;
  long long vlong;
  char buf[128];
  double score;

  serverAssert(sptr != NULL);
  serverAssert(ziplistGet(sptr, &vstr, &vlen, &vlong));

  if (vstr) {
    memcpy(buf, vstr, vlen);
    buf[vlen] = '\0';
    score = strtod(buf, NULL);
  } else {
    score = vlong;
  }

  return (score);
} /* zzlGetScore() */

/* ------------------------------------------------------------------------- */

unsigned char *
zzlInsert (
  unsigned char *zl,
  sds            ele,
  double         score
) {
  unsigned char *eptr = ziplistIndex(zl, 0), *sptr;
  double s;

  while (eptr != NULL) {
    sptr = ziplistNext(zl, eptr);
    serverAssert(sptr != NULL);
    s = zzlGetScore(sptr);

    if (s > score) {
      /* First element with score larger than score for element to be
       * inserted. This means we should take its spot in the list to
       * maintain ordering. */
      zl = zzlInsertAt(zl, eptr, ele, score);
      break;
    } else if (s == score) {
      /* Ensure lexicographical ordering for elements. */
      if (zzlCompareElements(eptr, (unsigned char *) ele, sdslen(ele)) > 0) {
        zl = zzlInsertAt(zl, eptr, ele, score);
        break;
      }
    }

    /* Move to next element. */
    eptr = ziplistNext(zl, sptr);
  }

  /* Push on tail of list when it was not yet inserted. */
  if (eptr == NULL) {
    zl = zzlInsertAt(zl, NULL, ele, score);
  }
  return (zl);
} /* zzlInsert() */

/* ------------------------------------------------------------------------- */

unsigned char *
zzlLastInLexRange (
  unsigned char *zl,
  zlexrangespec *range
) {
  unsigned char *eptr = ziplistIndex(zl, -2), *sptr;

  /* If everything is out of range, return early. */
  if (!zzlIsInLexRange(zl, range)) {
    return (NULL);
  }

  while (eptr != NULL) {
    if (zzlLexValueLteMax(eptr, range)) {
      /* Check if score >= min. */
      if (zzlLexValueGteMin(eptr, range)) {
        return (eptr);
      }
      return (NULL);
    }

    /* Move to previous element by moving to the score of previous element.
     * When this returns NULL, we know there also is no element. */
    sptr = ziplistPrev(zl, eptr);
    if (sptr != NULL) {
      serverAssert((eptr = ziplistPrev(zl, sptr)) != NULL);
    } else {
      eptr = NULL;
    }
  }

  return (NULL);
} /* zzlLastInLexRange() */

/* ------------------------------------------------------------------------- */

unsigned char *
zzlLastInRange (
  unsigned char *zl,
  zrangespec    *range
) {
  unsigned char *eptr = ziplistIndex(zl, -2), *sptr;
  double score;

  /* If everything is out of range, return early. */
  if (!zzlIsInRange(zl, range)) {
    return (NULL);
  }

  while (eptr != NULL) {
    sptr = ziplistNext(zl, eptr);
    serverAssert(sptr != NULL);

    score = zzlGetScore(sptr);
    if (zslValueLteMax(score, range)) {
      /* Check if score >= min. */
      if (zslValueGteMin(score, range)) {
        return (eptr);
      }
      return (NULL);
    }

    /* Move to previous element by moving to the score of previous element.
     * When this returns NULL, we know there also is no element. */
    sptr = ziplistPrev(zl, eptr);
    if (sptr != NULL) {
      serverAssert((eptr = ziplistPrev(zl, sptr)) != NULL);
    } else {
      eptr = NULL;
    }
  }

  return (NULL);
} /* zzlLastInRange() */

/* ------------------------------------------------------------------------- */

int
zzlLexValueGteMin (
  unsigned char *p,
  zlexrangespec *spec
) {
  sds value = ziplistGetObject(p);
  int res = zslLexValueGteMin(value, spec);

  sdsfree(value);
  return (res);
} /* zzlLexValueGteMin() */

/* ------------------------------------------------------------------------- */

int
zzlLexValueLteMax (
  unsigned char *p,
  zlexrangespec *spec
) {
  sds value = ziplistGetObject(p);
  int res = zslLexValueLteMax(value, spec);

  sdsfree(value);
  return (res);
} /* zzlLexValueLteMax() */

/* ------------------------------------------------------------------------- */

void
zzlNext (
  unsigned char  *zl,
  unsigned char **eptr,
  unsigned char **sptr
) {
  unsigned char *_eptr, *_sptr;

  serverAssert(*eptr != NULL && *sptr != NULL);

  _eptr = ziplistNext(zl, *sptr);
  if (_eptr != NULL) {
    _sptr = ziplistNext(zl, _eptr);
    serverAssert(_sptr != NULL);
  } else {
    /* No next entry. */
    _sptr = NULL;
  }

  *eptr = _eptr;
  *sptr = _sptr;
} /* zzlNext() */

/* ------------------------------------------------------------------------- */

void
zzlPrev (
  unsigned char  *zl,
  unsigned char **eptr,
  unsigned char **sptr
) {
  unsigned char *_eptr, *_sptr;

  serverAssert(*eptr != NULL && *sptr != NULL);

  _sptr = ziplistPrev(zl, *eptr);
  if (_sptr != NULL) {
    _eptr = ziplistPrev(zl, _sptr);
    serverAssert(_eptr != NULL);
  } else {
    /* No previous entry. */
    _eptr = NULL;
  }

  *eptr = _eptr;
  *sptr = _sptr;
} /* zzlPrev() */

/* ========================================================================= */
/* -- STATIC FUNCTION DEFINITIONS ------------------------------------------ */
/* ========================================================================= */

static void
blockingGenericZpopCommand (
  client *c,
  int     where
) {
  robj *o;
  mstime_t timeout;
  int j;

  if (getTimeoutFromObjectOrReply(c, c->argv[c->argc-1], &timeout,
    UNIT_SECONDS)
    != C_OK) {
    return;
  }

  for (j = 1; j < c->argc-1; j++) {
    o = lookupKeyWrite(c->db, c->argv[j]);
    if (checkType(c, o, OBJ_ZSET)) {
      return;
    }
    if (o != NULL) {
      if (zsetLength(o) != 0) {
        /* Non empty zset, this is like a normal ZPOP[MIN|MAX]. */
        genericZpopCommand(c, &c->argv[j], 1, where, 1, NULL);
        /* Replicate it as an ZPOP[MIN|MAX] instead of BZPOP[MIN|MAX]. */
        rewriteClientCommandVector(c, 2,
          where == ZSET_MAX ? shared.zpopmax : shared.zpopmin,
          c->argv[j]);
        return;
      }
    }
  }

  /* If we are inside a MULTI/EXEC and the zset is empty the only thing
   * we can do is treating it as a timeout (even with timeout 0). */
  if (c->flags & CLIENT_MULTI) {
    addReplyNullArray(c);
    return;
  }

  /* If the keys do not exist we must block */
  blockForKeys(c, BLOCKED_ZSET, c->argv + 1, c->argc - 2, timeout, NULL, NULL);
} /* blockingGenericZpopCommand() */

/* ------------------------------------------------------------------------- */

static void
genericZrangebylexCommand (
  zrange_result_handler *handler,
  zlexrangespec         *range,
  robj                  *zobj,
  long                   offset,
  long                   limit,
  int                    reverse
) {
  client *c = handler->client;
  unsigned long rangelen = 0;

  /* XXX FIXME - Add argument checks */

  if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
    unsigned char *zl = zobj->ptr;
    unsigned char *eptr, *sptr;
    unsigned char *vstr;
    unsigned int vlen;
    long long vlong;

    /* If reversed, get the last node in range as starting point. */
    if (reverse) {
      eptr = zzlLastInLexRange(zl, range);
    } else {
      eptr = zzlFirstInLexRange(zl, range);
    }

    /* No "first" element in the specified interval. */
    if (eptr == NULL) {
      addReply(c, shared.emptyarray);
      return;
    }

    /* Get score pointer for the first element. */
    serverAssertWithInfo(c, zobj, eptr != NULL);
    sptr = ziplistNext(zl, eptr);

    handler->beginResultEmission(handler, OBJ_ENCODING_ZIPLIST);

    /* If there is an offset, just traverse the number of elements without
     * checking the score because that is done in the next loop. */
    while (eptr && offset--) {
      if (reverse) {
        zzlPrev(zl, &eptr, &sptr);
      } else {
        zzlNext(zl, &eptr, &sptr);
      }
    }

    while (eptr && limit--) {
      /* Abort when the node is no longer in range. */
      if (reverse) {
        if (!zzlLexValueGteMin(eptr, range)) {
          break;
        }
      } else {
        if (!zzlLexValueLteMax(eptr, range)) {
          break;
        }
      }

      /* We know the element exists, so ziplistGet should always
       * succeed. */
      serverAssertWithInfo(c, zobj, ziplistGet(eptr, &vstr, &vlen, &vlong));

      rangelen++;

      if (vstr == NULL) {
        handler->emitResultFromLongLong(handler, vlong, 0.0);
      } else {
        handler->emitResultFromCBuffer(handler, vstr, vlen, 0.0);
      }

      /* Move to next node */
      if (reverse) {
        zzlPrev(zl, &eptr, &sptr);
      } else {
        zzlNext(zl, &eptr, &sptr);
      }
    }
  } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
    zset *zs = zobj->ptr;
    zskiplist *zsl = zs->zsl;
    zskiplistNode *ln;

    /* If reversed, get the last node in range as starting point. */
    if (reverse) {
      ln = zslLastInLexRange(zsl, range);
    } else {
      ln = zslFirstInLexRange(zsl, range);
    }

    /* No "first" element in the specified interval. */
    if (ln == NULL) {
      addReply(c, shared.emptyarray);
      return;
    }

    handler->beginResultEmission(handler, OBJ_ENCODING_SKIPLIST);

    /* If there is an offset, just traverse the number of elements without
     * checking the score because that is done in the next loop. */
    while (ln && offset--) {
      if (reverse) {
        ln = ln->backward;
      } else {
        ln = ln->level[0].forward;
      }
    }

    while (ln && limit--) {
      /* Abort when the node is no longer in range. */
      if (reverse) {
        if (!zslLexValueGteMin(ln->ele, range)) {
          break;
        }
      } else {
        if (!zslLexValueLteMax(ln->ele, range)) {
          break;
        }
      }

      rangelen++;
      handler->emitResultFromCBuffer(handler, ln->ele, sdslen(ln->ele), 0.0);

      /* Move to next node */
      if (reverse) {
        ln = ln->backward;
      } else {
        ln = ln->level[0].forward;
      }
    }
  } else {
    serverPanic("Unknown sorted set encoding");
  }

  handler->finalizeResultEmission(handler, rangelen);
} /* genericZrangebylexCommand() */

/* ------------------------------------------------------------------------- */

static void
genericZrangebyrankCommand (
  zrange_result_handler *handler,
  robj                  *zobj,
  long                   start,
  long                   end,
  int                    withscores,
  int                    reverse
) {
  client *c = handler->client;
  long llen;
  long rangelen;
  size_t result_cardinality;

  /* XXX FIXME - Add argument checks */

  /* Sanitize indexes. */
  llen = zsetLength(zobj);
  if (start < 0) {
    start = llen+start;
  }
  if (end < 0) {
    end = llen+end;
  }
  if (start < 0) {
    start = 0;
  }

  /* Invariant: start >= 0, so this test will be true when end < 0.
   * The range is empty when start > end or start >= length. */
  if ((start > end) || (start >= llen)) {
    addReply(c, shared.emptyarray);
    return;
  }
  if (end >= llen) {
    end = llen-1;
  }
  rangelen = (end-start)+1;
  result_cardinality = rangelen;

  if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
    unsigned char *zl = zobj->ptr;
    unsigned char *eptr, *sptr;
    unsigned char *vstr;
    unsigned int vlen;
    long long vlong;
    double score = 0.0;

    if (reverse) {
      eptr = ziplistIndex(zl, -2-(2*start));
    } else {
      eptr = ziplistIndex(zl, 2*start);
    }

    serverAssertWithInfo(c, zobj, eptr != NULL);
    sptr = ziplistNext(zl, eptr);

    handler->beginResultEmission(handler, OBJ_ENCODING_ZIPLIST);

    while (rangelen--) {
      serverAssertWithInfo(c, zobj, eptr != NULL && sptr != NULL);
      serverAssertWithInfo(c, zobj, ziplistGet(eptr, &vstr, &vlen, &vlong));

      if (withscores) {
        score = zzlGetScore(sptr);
      }

      if (vstr == NULL) {
        handler->emitResultFromLongLong(handler, vlong, score);
      } else {
        handler->emitResultFromCBuffer(handler, vstr, vlen, score);
      }

      if (reverse) {
        zzlPrev(zl, &eptr, &sptr);
      } else {
        zzlNext(zl, &eptr, &sptr);
      }
    }
  } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
    zset *zs = zobj->ptr;
    zskiplist *zsl = zs->zsl;
    zskiplistNode *ln;
    sds ele;
    double score = 0.0;

    /* Check if starting point is trivial, before doing log(N) lookup. */
    if (reverse) {
      ln = zsl->tail;
      if (start > 0) {
        ln = zslGetElementByRank(zsl, llen-start);
      }
    } else {
      ln = zsl->header->level[0].forward;
      if (start > 0) {
        ln = zslGetElementByRank(zsl, start+1);
      }
    }

    handler->beginResultEmission(handler, OBJ_ENCODING_SKIPLIST);

    while (rangelen--) {
      serverAssertWithInfo(c, zobj, ln != NULL);
      ele = ln->ele;

      if (withscores) {
        score = ln->score;
      }

      handler->emitResultFromCBuffer(handler, ele, sdslen(ele), score);

      ln = reverse ? ln->backward : ln->level[0].forward;
    }
  } else {
    serverPanic("Unknown sorted set encoding");
  }

  handler->finalizeResultEmission(handler, result_cardinality);
} /* genericZrangebyrankCommand() */

/* ------------------------------------------------------------------------- */

static void
genericZrangebyscoreCommand (
  zrange_result_handler *handler,
  zrangespec            *range,
  robj                  *zobj,
  int                    withscores,
  long                   offset,
  long                   limit,
  int                    reverse
) {
  client *c = handler->client;
  unsigned long rangelen = 0;

  /* XXX FIXME - Add argument checks */

  if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
    unsigned char *zl = zobj->ptr;
    unsigned char *eptr, *sptr;
    unsigned char *vstr;
    unsigned int vlen;
    long long vlong;
    double score;

    /* If reversed, get the last node in range as starting point. */
    if (reverse) {
      eptr = zzlLastInRange(zl, range);
    } else {
      eptr = zzlFirstInRange(zl, range);
    }

    /* No "first" element in the specified interval. */
    if (eptr == NULL) {
      addReply(c, shared.emptyarray);
      return;
    }

    /* Get score pointer for the first element. */
    serverAssertWithInfo(c, zobj, eptr != NULL);
    sptr = ziplistNext(zl, eptr);

    handler->beginResultEmission(handler, OBJ_ENCODING_ZIPLIST);

    /* If there is an offset, just traverse the number of elements without
     * checking the score because that is done in the next loop. */
    while (eptr && offset--) {
      if (reverse) {
        zzlPrev(zl, &eptr, &sptr);
      } else {
        zzlNext(zl, &eptr, &sptr);
      }
    }

    while (eptr && limit--) {
      score = zzlGetScore(sptr);

      /* Abort when the node is no longer in range. */
      if (reverse) {
        if (!zslValueGteMin(score, range)) {
          break;
        }
      } else {
        if (!zslValueLteMax(score, range)) {
          break;
        }
      }

      /* We know the element exists, so ziplistGet should always
       * succeed */
      serverAssertWithInfo(c, zobj, ziplistGet(eptr, &vstr, &vlen, &vlong));

      rangelen++;

      if (vstr == NULL) {
        handler->emitResultFromLongLong(handler, vlong,
          ((withscores) ? score : 0.0));
      } else {
        handler->emitResultFromCBuffer(handler, vstr, vlen,
          ((withscores) ? score : 0.0));
      }

      /* Move to next node */
      if (reverse) {
        zzlPrev(zl, &eptr, &sptr);
      } else {
        zzlNext(zl, &eptr, &sptr);
      }
    }
  } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
    zset *zs = zobj->ptr;
    zskiplist *zsl = zs->zsl;
    zskiplistNode *ln;

    /* If reversed, get the last node in range as starting point. */
    if (reverse) {
      ln = zslLastInRange(zsl, range);
    } else {
      ln = zslFirstInRange(zsl, range);
    }

    /* No "first" element in the specified interval. */
    if (ln == NULL) {
      addReply(c, shared.emptyarray);
      return;
    }

    handler->beginResultEmission(handler, OBJ_ENCODING_SKIPLIST);

    /* If there is an offset, just traverse the number of elements without
     * checking the score because that is done in the next loop. */
    while (ln && offset--) {
      if (reverse) {
        ln = ln->backward;
      } else {
        ln = ln->level[0].forward;
      }
    }

    while (ln && limit--) {
      /* Abort when the node is no longer in range. */
      if (reverse) {
        if (!zslValueGteMin(ln->score, range)) {
          break;
        }
      } else {
        if (!zslValueLteMax(ln->score, range)) {
          break;
        }
      }

      rangelen++;

      handler->emitResultFromCBuffer(handler, ln->ele, sdslen(ln->ele),
        ((withscores) ? ln->score : 0.0));

      /* Move to next node */
      if (reverse) {
        ln = ln->backward;
      } else {
        ln = ln->level[0].forward;
      }
    }
  } else {
    serverPanic("Unknown sorted set encoding");
  }

  handler->finalizeResultEmission(handler, rangelen);
} /* genericZrangebyscoreCommand() */

/* ------------------------------------------------------------------------- */

static int
sdscmplex (
  sds a,
  sds b
) {
  if (a == b) {
    return (0);
  }
  if ((a == shared.minstring) || (b == shared.maxstring)) {
    return (-1);
  }
  if ((a == shared.maxstring) || (b == shared.minstring)) {
    return (1);
  }
  return (sdscmp(a, b));
} /* sdscmplex() */

/* ------------------------------------------------------------------------- */

static void
zaddGenericCommand (
  client *c,
  int     flags
) {
  static char *nanerr = "resulting score is not a number (NaN)";
  robj *key = c->argv[1];
  robj *zobj;
  sds ele;
  double score = 0, *scores = NULL;
  int j, elements;
  int scoreidx = 0;

  /* The following vars are used in order to track what the command actually
   * did during the execution, to reply to the client and to trigger the
   * notification of keyspace change. */
  int added = 0;        /* Number of new elements added. */
  int updated = 0;      /* Number of elements with updated score. */
  int processed = 0;    /* Number of elements processed, may remain zero with
                         * options like XX. */

  /* Parse options. At the end 'scoreidx' is set to the argument position
   * of the score of the first score-element pair. */

  scoreidx = 2;
  while (scoreidx < c->argc) {
    char *opt = c->argv[scoreidx]->ptr;
    if (!strcasecmp(opt, "nx")) {
      flags |= ZADD_NX;
    } else if (!strcasecmp(opt, "xx")) {
      flags |= ZADD_XX;
    } else if (!strcasecmp(opt, "ch")) {
      flags |= ZADD_CH;
    } else if (!strcasecmp(opt, "incr")) {
      flags |= ZADD_INCR;
    } else if (!strcasecmp(opt, "gt")) {
      flags |= ZADD_GT;
    } else if (!strcasecmp(opt, "lt")) {
      flags |= ZADD_LT;
    } else {
      break;
    }
    scoreidx++;
  }

  /* Turn options into simple to check vars. */
  int incr = (flags & ZADD_INCR) != 0;
  int nx = (flags & ZADD_NX) != 0;
  int xx = (flags & ZADD_XX) != 0;
  int ch = (flags & ZADD_CH) != 0;
  int gt = (flags & ZADD_GT) != 0;
  int lt = (flags & ZADD_LT) != 0;

  /* After the options, we expect to have an even number of args, since
   * we expect any number of score-element pairs. */
  elements = c->argc-scoreidx;
  if (elements % 2 || !elements) {
    addReply(c, shared.syntaxerr);
    return;
  }
  elements /= 2;   /* Now this holds the number of score-element pairs. */

  /* Check for incompatible options. */
  if (nx && xx) {
    addReplyError(c,
      "XX and NX options at the same time are not compatible");
    return;
  }

  if ((gt && nx) || (lt && nx) || (gt && lt)) {
    addReplyError(c,
      "GT, LT, and/or NX options at the same time are not compatible");
    return;
  }

  /* Note that XX is compatible with either GT or LT */

  if (incr && (elements > 1)) {
    addReplyError(c,
      "INCR option supports a single increment-element pair");
    return;
  }

  /* Start parsing all the scores, we need to emit any syntax error
   * before executing additions to the sorted set, as the command should
   * either execute fully or nothing at all. */
  scores = zmalloc(sizeof(double)*elements);
  for (j = 0; j < elements; j++) {
    if (getDoubleFromObjectOrReply(c, c->argv[scoreidx+j*2], &scores[j], NULL)
      != C_OK) {
      goto cleanup;
    }
  }

  /* Lookup the key and create the sorted set if does not exist. */
  zobj = lookupKeyWrite(c->db, key);
  if (checkType(c, zobj, OBJ_ZSET)) {
    goto cleanup;
  }
  if (zobj == NULL) {
    if (xx) {
      goto reply_to_client;           /* No key + XX option: nothing to do. */
    }
    if ((server.zset_max_ziplist_entries == 0) ||
      (server.zset_max_ziplist_value < sdslen(c->argv[scoreidx+1]->ptr))) {
      zobj = createZsetObject();
    } else {
      zobj = createZsetZiplistObject();
    }
    dbAdd(c->db, key, zobj);
  }

  for (j = 0; j < elements; j++) {
    double newscore;
    score = scores[j];
    int retflags = flags;

    ele = c->argv[scoreidx+1+j*2]->ptr;
    int retval = zsetAdd(zobj, score, ele, &retflags, &newscore);
    if (retval == 0) {
      addReplyError(c, nanerr);
      goto cleanup;
    }
    if (retflags & ZADD_ADDED) {
      added++;
    }
    if (retflags & ZADD_UPDATED) {
      updated++;
    }
    if (!(retflags & ZADD_NOP)) {
      processed++;
    }
    score = newscore;
  }
  server.dirty += (added+updated);

reply_to_client:
  if (incr) {   /* ZINCRBY or INCR option. */
    if (processed) {
      addReplyDouble(c, score);
    } else {
      addReplyNull(c);
    }
  } else {   /* ZADD. */
    addReplyLongLong(c, ch ? added+updated : added);
  }

cleanup:
  zfree(scores);
  if (added || updated) {
    signalModifiedKey(c, c->db, key);
    notifyKeyspaceEvent(NOTIFY_ZSET,
      incr ? "zincr" : "zadd", key, c->db->id);
  }
} /* zaddGenericCommand() */

/* ------------------------------------------------------------------------- */

static void
zrankGenericCommand (
  client *c,
  int     reverse
) {
  robj *key = c->argv[1];
  robj *ele = c->argv[2];
  robj *zobj;
  long rank;

  if (((zobj = lookupKeyReadOrReply(c, key, shared.null[c->resp])) == NULL) ||
    checkType(c, zobj, OBJ_ZSET)) {
    return;
  }

  serverAssertWithInfo(c, ele, sdsEncodedObject(ele));
  rank = zsetRank(zobj, ele->ptr, reverse);
  if (rank >= 0) {
    addReplyLongLong(c, rank);
  } else {
    addReplyNull(c);
  }
} /* zrankGenericCommand() */

/* ------------------------------------------------------------------------- */

static void
zremrangeGenericCommand (
  client *c,
  int     rangetype
) {
  robj *key = c->argv[1];
  robj *zobj;
  int keyremoved = 0;
  unsigned long deleted = 0;
  zrangespec range;
  zlexrangespec lexrange;
  long start, end, llen;

  /* Step 1: Parse the range. */
  if (rangetype == ZRANGE_TYPE_RANK) {
    if ((getLongFromObjectOrReply(c, c->argv[2], &start, NULL) != C_OK) ||
      (getLongFromObjectOrReply(c, c->argv[3], &end, NULL) != C_OK)) {
      return;
    }
  } else if (rangetype == ZRANGE_TYPE_SCORE) {
    if (zslParseRange(c->argv[2], c->argv[3], &range) != C_OK) {
      addReplyError(c, "min or max is not a float");
      return;
    }
  } else if (rangetype == ZRANGE_TYPE_LEX) {
    if (zslParseLexRange(c->argv[2], c->argv[3], &lexrange) != C_OK) {
      addReplyError(c, "min or max not valid string range item");
      return;
    }
  }

  /* Step 2: Lookup & range sanity checks if needed. */
  if (((zobj = lookupKeyWriteOrReply(c, key, shared.czero)) == NULL) ||
    checkType(c, zobj, OBJ_ZSET)) {
    goto cleanup;
  }

  if (rangetype == ZRANGE_TYPE_RANK) {
    /* Sanitize indexes. */
    llen = zsetLength(zobj);
    if (start < 0) {
      start = llen+start;
    }
    if (end < 0) {
      end = llen+end;
    }
    if (start < 0) {
      start = 0;
    }

    /* Invariant: start >= 0, so this test will be true when end < 0.
     * The range is empty when start > end or start >= length. */
    if ((start > end) || (start >= llen)) {
      addReply(c, shared.czero);
      goto cleanup;
    }
    if (end >= llen) {
      end = llen-1;
    }
  }

  /* Step 3: Perform the range deletion operation. */
  if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
    switch (rangetype)
    {
      case ZRANGE_TYPE_RANK:
        {
          zobj->ptr =
            zzlDeleteRangeByRank(zobj->ptr, start+1, end+1, &deleted);
        }
        break;

      case ZRANGE_TYPE_SCORE:
        {
          zobj->ptr = zzlDeleteRangeByScore(zobj->ptr, &range, &deleted);
        }
        break;

      case ZRANGE_TYPE_LEX:
        {
          zobj->ptr = zzlDeleteRangeByLex(zobj->ptr, &lexrange, &deleted);
        }
        break;
    }
    if (zzlLength(zobj->ptr) == 0) {
      dbDelete(c->db, key);
      keyremoved = 1;
    }
  } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
    zset *zs = zobj->ptr;
    switch (rangetype)
    {
      case ZRANGE_TYPE_RANK:
        {
          deleted = zslDeleteRangeByRank(zs->zsl, start+1, end+1, zs->dict);
        }
        break;

      case ZRANGE_TYPE_SCORE:
        {
          deleted = zslDeleteRangeByScore(zs->zsl, &range, zs->dict);
        }
        break;

      case ZRANGE_TYPE_LEX:
        {
          deleted = zslDeleteRangeByLex(zs->zsl, &lexrange, zs->dict);
        }
        break;
    }
    if (htNeedsResize(zs->dict)) {
      dictResize(zs->dict);
    }
    if (dictSize(zs->dict) == 0) {
      dbDelete(c->db, key);
      keyremoved = 1;
    }
  } else {
    serverPanic("Unknown sorted set encoding");
  }

  /* Step 4: Notifications and reply. */
  if (deleted) {
    char *event[3] =
    {
      "zremrangebyrank", "zremrangebyscore", "zremrangebylex"
    };
    signalModifiedKey(c, c->db, key);
    notifyKeyspaceEvent(NOTIFY_ZSET, event[rangetype], key, c->db->id);
    if (keyremoved) {
      notifyKeyspaceEvent(NOTIFY_GENERIC, "del", key, c->db->id);
    }
  }
  server.dirty += deleted;
  addReplyLongLong(c, deleted);

cleanup:
  if (rangetype == ZRANGE_TYPE_LEX) {
    zslFreeLexRange(&lexrange);
  }
} /* zremrangeGenericCommand() */

/* ------------------------------------------------------------------------- */

/**
 * This function handles the top-level processing of Z[REV]RANGE[BYPOS|BYLEX]
 * commands and has been factored in such a way that ZSTORE can pass-through
 * its remaining arguments as if the original command had been called directly.
 */
static void
zrangeGenericCommand (
  zrange_result_handler *handler,

  /**
   * As Redis command processing assumes argv[0] is the command, argc_start is
   * similarly expected to be 0-based, which is kinda nasty.
   */
  int             argc_start,
  zrange_type     rangetype,
  zscan_direction direction
) {
  client *c = handler->client;
  robj *key = c->argv[argc_start + 1];
  robj *zobj;
  zrangespec range;
  zlexrangespec lexrange;
  int reverse = (direction == ZSCAN_DIRECTION_REVERSE) ? 1 : 0;

  /*
   * Required arguments are parsed specifically for each range type. As several
   * optional clauses are shared between ZRANGE* commands, however, a general
   * parser is used to parse remaining arguments based solely on the validity
   * of the clauses for that command. While parsing and semantic validation are
   * generally be separate from execution, this has been written to match the
   * intermixed processing common to Redis command handlers.
   */
#define OPTIONAL_CLAUSE_WITHSCORES    0x01
#define OPTIONAL_CLAUSE_LIMIT         0x02
  int valid_optional_clauses = 0;
  int argc_processed = 0;
  int argc_remaining = 0;

  int minidx = 2;
  int maxidx = 3;

  /* Options common to all */
  long opt_start = 0;
  long opt_end = 0;
  int opt_withscores = 0;
  long opt_offset = 0;
  long opt_limit = -1;

  if (reverse && ((ZRANGE_TYPE_SCORE == rangetype) ||
    (ZRANGE_TYPE_LEX == rangetype))) {
    /* Range is given as [max,min] */
    maxidx = 2;
    minidx = 3;
  }

  minidx += argc_start;
  maxidx += argc_start;

  /* Step 1: Parse the range. */
  switch (rangetype)
  {
    /* Z[REV]RANGE, ZRANGESTORE [REV]RANGE */
    case ZRANGE_TYPE_RANK:
      {
        if ((getLongFromObjectOrReply(c, c->argv[(argc_start + 2)], &opt_start,
          NULL) != C_OK) ||
          (getLongFromObjectOrReply(c, c->argv[(argc_start + 3)], &opt_end,
          NULL) != C_OK)) {
          return;
        }
        argc_processed = (argc_start + 4);
        valid_optional_clauses = OPTIONAL_CLAUSE_WITHSCORES;
      }
      break;

    /* Z[REV]RANGEBYSCORE, ZRANGESTORE [REV]RANGEBYSCORE */
    case ZRANGE_TYPE_SCORE:
      {
        if (zslParseRange(c->argv[minidx], c->argv[maxidx], &range) != C_OK) {
          addReplyError(c, "min or max is not a float");
          return;
        }
        argc_processed = (argc_start + 4);
        valid_optional_clauses =
          (OPTIONAL_CLAUSE_WITHSCORES | OPTIONAL_CLAUSE_LIMIT);
      }
      break;

    /* Z[REV]RANGEBYLEX, ZRANGESTORE [REV]RANGEBYLEX */
    case ZRANGE_TYPE_LEX:
      {
        if (zslParseLexRange(c->argv[minidx], c->argv[maxidx],
          &lexrange) != C_OK) {
          addReplyError(c, "min or max not valid string range item");
          return;
        }
        argc_processed = (argc_start + 4);
        valid_optional_clauses = OPTIONAL_CLAUSE_LIMIT;
      }
      break;
  }

  /* Step 2: Parse remaining optional arguments. */
  argc_remaining = (c->argc - argc_processed);
  if (0 < argc_remaining) {
    int pos = argc_processed;
    while (argc_remaining) {
      if ((valid_optional_clauses & OPTIONAL_CLAUSE_WITHSCORES) &&
        (1 <= argc_remaining) &&
        !strcasecmp(c->argv[pos]->ptr, "withscores")) {
        ++pos;
        --argc_remaining;
        opt_withscores = 1;
      } else if ((valid_optional_clauses & OPTIONAL_CLAUSE_LIMIT) &&
        (3 <= argc_remaining) && !strcasecmp(c->argv[pos]->ptr, "limit")) {
        if ((getLongFromObjectOrReply(c, c->argv[pos+1], &opt_offset, NULL)
          != C_OK) ||
          (getLongFromObjectOrReply(c, c->argv[pos+2], &opt_limit, NULL)
          != C_OK)) {
          break;
        }
        pos += 3;
        argc_remaining -= 3;
      } else {
        break;
      }
    }

    if (0 < argc_remaining) {
      addReply(c, shared.syntaxerr);
      goto cleanup;
    }
  }
#undef OPTIONAL_CLAUSE_LIMIT
#undef OPTIONAL_CLAUSE_WITHSCORES

  if (opt_withscores) {
    zrangeResultHandlerScoreEmissionEnable(handler);
  }

  /* Step 3: Lookup the key and get the range. */
  if (((zobj = lookupKeyReadOrReply(c, key, shared.emptyarray)) == NULL) ||
    checkType(c, zobj, OBJ_ZSET)) {
    goto cleanup;
  }

  /* Step 4: Pass this to the command-specific handler. */
  switch (rangetype)
  {
    case ZRANGE_TYPE_RANK:
      {
        genericZrangebyrankCommand(handler, zobj, opt_start, opt_end,
          opt_withscores, reverse);
      }
      break;

    case ZRANGE_TYPE_SCORE:
      {
        genericZrangebyscoreCommand(handler, &range, zobj, opt_withscores,
          opt_offset, opt_limit, reverse);
      }
      break;

    case ZRANGE_TYPE_LEX:
      {
        genericZrangebylexCommand(handler, &lexrange, zobj, opt_offset,
          opt_limit, reverse);
        zslFreeLexRange(&lexrange);
      }
      break;
  }

  return;

cleanup:

  switch (rangetype)
  {
    case ZRANGE_TYPE_RANK:
      {
        // nothing to see here
      }
      break;

    case ZRANGE_TYPE_SCORE:
      {
        // nothing to see here
      }
      break;

    case ZRANGE_TYPE_LEX:
      {
        zslFreeLexRange(&lexrange);
      }
      break;
  }
} /* zremrangeGenericCommand() */

/* ------------------------------------------------------------------------- */

static void
zrangeResultBeginClient (
  zrange_result_handler *handler,
  int                    encoding
) {
  ZSET_UNUSED(encoding);

  handler->userdata = addReplyDeferredLen(handler->client);
} /* zrangeResultBeginClient() */

/* ------------------------------------------------------------------------- */

static void
zrangeResultEmitCBufferToClient (
  zrange_result_handler *handler,
  const void            *value,
  size_t                 value_length_in_bytes,
  double                 score
) {
  if (handler->should_emit_array_length) {
    addReplyArrayLen(handler->client, 2);
  }

  addReplyBulkCBuffer(handler->client, value, value_length_in_bytes);

  if (handler->withscores) {
    addReplyDouble(handler->client, score);
  }
}

/* ------------------------------------------------------------------------- */

static void
zrangeResultEmitLongLongToClient (
  zrange_result_handler *handler,
  long long              value,
  double                 score
) {
  if (handler->should_emit_array_length) {
    addReplyArrayLen(handler->client, 2);
  }

  addReplyBulkLongLong(handler->client, value);

  if (handler->withscores) {
    addReplyDouble(handler->client, score);
  }
} /* zrangeResultEmitLongLongToClient() */

/* ------------------------------------------------------------------------- */

static void
zrangeResultFinalizeClient (
  zrange_result_handler *handler,
  size_t                 result_count
) {
  if (handler->withscores && (handler->client->resp == 2)) {
    result_count *= 2;
  }

  setDeferredArrayLen(handler->client, handler->userdata, result_count);
} /* zrangeResultFinalizeClient() */

/* ------------------------------------------------------------------------- */

static void
zrangeResultBeginStore (
  zrange_result_handler *handler,
  int                    encoding
) {
  if (dbDelete(handler->client->db, handler->dstkey)) {
    signalModifiedKey(handler->client, handler->client->db, handler->dstkey);
    handler->touched = 1;
    ++server.dirty;
  }

  /* This is a cheap version of a strategy method... */
  switch (encoding)
  {
    case OBJ_ENCODING_ZIPLIST:
      {
        handler->dstobj = createZsetZiplistObject();
      }
      break;

    case OBJ_ENCODING_SKIPLIST: /* Fall-through */

    default:
      {
        handler->dstobj = createZsetObject();
      }
  }

  dbAdd(handler->client->db, handler->dstkey, handler->dstobj);
} /* zrangeResultBeginClient() */

/* ------------------------------------------------------------------------- */

static void
zrangeResultEmitCBufferForStore (
  zrange_result_handler *handler,
  const void            *value,
  size_t                 value_length_in_bytes,
  double                 score
) {
  double newscore;
  int retflags = 0;
  sds ele = sdsnewlen(value, value_length_in_bytes);
  int retval = zsetAdd(handler->dstobj, score, ele, &retflags, &newscore);

  if (retval == 0) {
    printf("yikers...\n");
  }
} /* zrangeResultEmitCBufferForStore() */

/* ------------------------------------------------------------------------- */

static void
zrangeResultEmitLongLongForStore (
  zrange_result_handler *handler,
  long long              value,
  double                 score
) {
  double newscore;
  int retflags = 0;
  sds ele = sdsfromlonglong(value);
  int retval = zsetAdd(handler->dstobj, score, ele, &retflags, &newscore);

  if (retval == 0) {
    printf("yikers...\n");
  }
} /* zrangeResultEmitLongLongForStore() */

/* ------------------------------------------------------------------------- */

static void
zrangeResultFinalizeStore (
  zrange_result_handler *handler,
  size_t                 result_count
) {
  if (0 < result_count) {
    addReplyLongLong(handler->client, result_count);
    if (!handler->touched) {
      signalModifiedKey(handler->client, handler->client->db, handler->dstkey);
    }
    notifyKeyspaceEvent(NOTIFY_ZSET, "zrangestore", handler->dstkey,
      handler->client->db->id);
    ++server.dirty;
  } else {
    addReply(handler->client, shared.czero);
    if (handler->touched) {
      notifyKeyspaceEvent(NOTIFY_GENERIC, "del", handler->dstkey,
        handler->client->db->id);
    }
  }
} /* zrangeResultFinalizeStore() */

/* ------------------------------------------------------------------------- */

static void
zrangeResultHandlerInit (
  zrange_result_handler *handler,
  client                *client,
  zrange_consumer_type   type
) {
  memset(handler, 0, sizeof(*handler));

  handler->client = client;

  switch (type)
  {
    case ZRANGE_CONSUMER_TYPE_CLIENT:
      {
        handler->beginResultEmission = zrangeResultBeginClient;
        handler->finalizeResultEmission = zrangeResultFinalizeClient;
        handler->emitResultFromCBuffer = zrangeResultEmitCBufferToClient;
        handler->emitResultFromLongLong = zrangeResultEmitLongLongToClient;
      }
      break;

    case ZRANGE_CONSUMER_TYPE_INTERNAL:
      {
        handler->beginResultEmission = zrangeResultBeginStore;
        handler->finalizeResultEmission = zrangeResultFinalizeStore;
        handler->emitResultFromCBuffer = zrangeResultEmitCBufferForStore;
        handler->emitResultFromLongLong = zrangeResultEmitLongLongForStore;
      }
      break;
  }
} /* zrangeResultHandlerInit() */

/* ------------------------------------------------------------------------- */

static void
zrangeResultHandlerScoreEmissionEnable (
  zrange_result_handler *handler
) {
  handler->withscores = 1;
  handler->should_emit_array_length = (handler->client->resp > 2);
} /* zrangeResultHandlerScoreEmissionEnable() */

/* ------------------------------------------------------------------------- */

static void
zrangeResultHandlerDestinationKeySet (
  zrange_result_handler *handler,
  robj                  *dstkey
) {
  handler->dstkey = dstkey;
} /* zrangeResultHandlerDestinationKeySet() */

/* ------------------------------------------------------------------------- */

static zskiplistNode *
zslCreateNode (
  int    level,
  double score,
  sds    ele
) {
  zskiplistNode *zn =
    zmalloc(sizeof(*zn)+level*sizeof(struct zskiplistLevel));

  zn->score = score;
  zn->ele = ele;
  return (zn);
} /* zslCreateNode() */

/* ------------------------------------------------------------------------- */

static void
zslDeleteNode (
  zskiplist      *zsl,
  zskiplistNode  *x,
  zskiplistNode **update
) {
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
  while (zsl->level > 1 && zsl->header->level[zsl->level-1].forward == NULL) {
    zsl->level--;
  }
  zsl->length--;
} /* zslDeleteNode() */

/* ------------------------------------------------------------------------- */

static unsigned long
zslDeleteRangeByLex (
  zskiplist     *zsl,
  zlexrangespec *range,
  dict          *dict
) {
  zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
  unsigned long removed = 0;
  int i;

  x = zsl->header;
  for (i = zsl->level-1; i >= 0; i--) {
    while (x->level[i].forward &&
      !zslLexValueGteMin(x->level[i].forward->ele, range)) {
      x = x->level[i].forward;
    }
    update[i] = x;
  }

  /* Current node is the last with score < or <= min. */
  x = x->level[0].forward;

  /* Delete nodes while in range. */
  while (x && zslLexValueLteMax(x->ele, range)) {
    zskiplistNode *next = x->level[0].forward;
    zslDeleteNode(zsl, x, update);
    dictDelete(dict, x->ele);
    zslFreeNode(x);     /* Here is where x->ele is actually released. */
    removed++;
    x = next;
  }
  return (removed);
} /* zslDeleteRangeByLex() */

/* ------------------------------------------------------------------------- */

static unsigned long
zslDeleteRangeByRank (
  zskiplist   *zsl,
  unsigned int start,
  unsigned int end,
  dict        *dict
) {
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
    zslDeleteNode(zsl, x, update);
    dictDelete(dict, x->ele);
    zslFreeNode(x);
    removed++;
    traversed++;
    x = next;
  }
  return (removed);
} /* zslDeleteRangeByRank() */

/* ------------------------------------------------------------------------- */

static unsigned long
zslDeleteRangeByScore (
  zskiplist  *zsl,
  zrangespec *range,
  dict       *dict
) {
  zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
  unsigned long removed = 0;
  int i;

  x = zsl->header;
  for (i = zsl->level-1; i >= 0; i--) {
    while (x->level[i].forward && (range->minex ?
      x->level[i].forward->score <= range->min :
      x->level[i].forward->score < range->min)) {
      x = x->level[i].forward;
    }
    update[i] = x;
  }

  /* Current node is the last with score < or <= min. */
  x = x->level[0].forward;

  /* Delete nodes while in range. */
  while (x &&
    (range->maxex ? x->score < range->max : x->score <= range->max)) {
    zskiplistNode *next = x->level[0].forward;
    zslDeleteNode(zsl, x, update);
    dictDelete(dict, x->ele);
    zslFreeNode(x);     /* Here is where x->ele is actually released. */
    ++removed;
    x = next;
  }
  return (removed);
} /* zslDeleteRangeByScore() */

/* ------------------------------------------------------------------------- */

static void
zslFreeNode (
  zskiplistNode *node
) {
  sdsfree(node->ele);
  zfree(node);
} /* zslFreeNode() */

/* ------------------------------------------------------------------------- */

static int
zslIsInLexRange (
  zskiplist     *zsl,
  zlexrangespec *range
) {
  zskiplistNode *x;

  /* Test for ranges that will always be empty. */
  int cmp = sdscmplex(range->min, range->max);

  if ((cmp > 0) || ((cmp == 0) && (range->minex || range->maxex))) {
    return (0);
  }
  x = zsl->tail;
  if ((x == NULL) || !zslLexValueGteMin(x->ele, range)) {
    return (0);
  }
  x = zsl->header->level[0].forward;
  if ((x == NULL) || !zslLexValueLteMax(x->ele, range)) {
    return (0);
  }
  return (1);
} /* zslIsInLexRange() */

/* ------------------------------------------------------------------------- */

static int
zslIsInRange (
  zskiplist  *zsl,
  zrangespec *range
) {
  zskiplistNode *x;

  /* Test for ranges that will always be empty. */
  if ((range->min > range->max) ||
    ((range->min == range->max) && (range->minex || range->maxex))) {
    return (0);
  }
  x = zsl->tail;
  if ((x == NULL) || !zslValueGteMin(x->score, range)) {
    return (0);
  }
  x = zsl->header->level[0].forward;
  if ((x == NULL) || !zslValueLteMax(x->score, range)) {
    return (0);
  }
  return (1);
} /* zslIsInRange() */

/* ------------------------------------------------------------------------- */

static int
zslParseLexRangeItem (
  robj *item,
  sds  *dest,
  int  *ex
) {
  char *c = item->ptr;

  switch (c[0])
  {
    case '+':
      {
        if (c[1] != '\0') {
          return (C_ERR);
        }
        *ex = 1;
        *dest = shared.maxstring;
        return (C_OK);
      }

    case '-':
      {
        if (c[1] != '\0') {
          return (C_ERR);
        }
        *ex = 1;
        *dest = shared.minstring;
        return (C_OK);
      }

    case '(':
      {
        *ex = 1;
        *dest = sdsnewlen(c+1, sdslen(c)-1);
        return (C_OK);
      }

    case '[':
      {
        *ex = 0;
        *dest = sdsnewlen(c+1, sdslen(c)-1);
        return (C_OK);
      }

    default:
      return (C_ERR);
  }
} /* zslParseLexRangeItem() */

/* ------------------------------------------------------------------------- */

static int
zslParseRange (
  robj       *min,
  robj       *max,
  zrangespec *spec
) {
  char *eptr;

  spec->minex = spec->maxex = 0;

  /* Parse the min-max interval. If one of the values is prefixed
   * by the "(" character, it's considered "open". For instance
   * ZRANGEBYSCORE zset (1.5 (2.5 will match min < x < max
   * ZRANGEBYSCORE zset 1.5 2.5 will instead match min <= x <= max */
  if (min->encoding == OBJ_ENCODING_INT) {
    spec->min = (long) min->ptr;
  } else {
    if (((char *) min->ptr)[0] == '(') {
      spec->min = strtod((char *) min->ptr+1, &eptr);
      if ((eptr[0] != '\0') || isnan(spec->min)) {
        return (C_ERR);
      }
      spec->minex = 1;
    } else {
      spec->min = strtod((char *) min->ptr, &eptr);
      if ((eptr[0] != '\0') || isnan(spec->min)) {
        return (C_ERR);
      }
    }
  }
  if (max->encoding == OBJ_ENCODING_INT) {
    spec->max = (long) max->ptr;
  } else {
    if (((char *) max->ptr)[0] == '(') {
      spec->max = strtod((char *) max->ptr+1, &eptr);
      if ((eptr[0] != '\0') || isnan(spec->max)) {
        return (C_ERR);
      }
      spec->maxex = 1;
    } else {
      spec->max = strtod((char *) max->ptr, &eptr);
      if ((eptr[0] != '\0') || isnan(spec->max)) {
        return (C_ERR);
      }
    }
  }

  return (C_OK);
} /* zslParseRange() */

/* ------------------------------------------------------------------------- */

static int
zslRandomLevel (
  void
) {
  int level = 1;

  while ((random() & 0xFFFF) < (ZSKIPLIST_P * 0xFFFF)) {
    ++level;
  }
  return ((level < ZSKIPLIST_MAXLEVEL) ? level : ZSKIPLIST_MAXLEVEL);
} /* zslRandomLevel() */

/* ------------------------------------------------------------------------- */

static zskiplistNode *
zslUpdateScore (
  zskiplist *zsl,
  double     curscore,
  sds        ele,
  double     newscore
) {
  zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
  int i;

  /* We need to seek to element to update to start: this is useful anyway,
   * we'll have to update or remove it. */
  x = zsl->header;
  for (i = zsl->level-1; i >= 0; i--) {
    while (x->level[i].forward &&
      (x->level[i].forward->score < curscore ||
      (x->level[i].forward->score == curscore &&
      sdscmp(x->level[i].forward->ele, ele) < 0))) {
      x = x->level[i].forward;
    }
    update[i] = x;
  }

  /* Jump to our element: note that this function assumes that the
   * element with the matching score exists. */
  x = x->level[0].forward;
  serverAssert(x && curscore == x->score && sdscmp(x->ele, ele) == 0);

  /* If the node, after the score update, would be still exactly
   * at the same position, we can just update the score without
   * actually removing and re-inserting the element in the skiplist. */
  if (((x->backward == NULL) || (x->backward->score < newscore)) &&
    ((x->level[0].forward == NULL) ||
    (x->level[0].forward->score > newscore))) {
    x->score = newscore;
    return (x);
  }

  /* No way to reuse the old node: we need to remove and insert a new
   * one at a different place. */
  zslDeleteNode(zsl, x, update);
  zskiplistNode *newnode = zslInsert(zsl, newscore, x->ele);

  /* We reused the old node x->ele SDS string, free the node now
   * since zslInsert created a new one. */
  x->ele = NULL;
  zslFreeNode(x);
  return (newnode);
} /* zslUpdateScore() */

/* ------------------------------------------------------------------------- */

static void
zuiClearIterator (
  zsetopsrc *op
) {
  if (op->subject == NULL) {
    return;
  }

  if (op->type == OBJ_SET) {
    iterset *it = &op->iter.set;
    if (op->encoding == OBJ_ENCODING_INTSET) {
      UNUSED(it);       /* skip */
    } else if (op->encoding == OBJ_ENCODING_HT) {
      dictReleaseIterator(it->ht.di);
    } else {
      serverPanic("Unknown set encoding");
    }
  } else if (op->type == OBJ_ZSET) {
    iterzset *it = &op->iter.zset;
    if (op->encoding == OBJ_ENCODING_ZIPLIST) {
      UNUSED(it);       /* skip */
    } else if (op->encoding == OBJ_ENCODING_SKIPLIST) {
      UNUSED(it);       /* skip */
    } else {
      serverPanic("Unknown sorted set encoding");
    }
  } else {
    serverPanic("Unsupported type");
  }
} /* zuiClearIterator() */

/* ------------------------------------------------------------------------- */

static int
zuiCompareByCardinality (
  const void *s1,
  const void *s2
) {
  unsigned long first = zuiLength((zsetopsrc *) s1);
  unsigned long second = zuiLength((zsetopsrc *) s2);

  if (first > second) {
    return (1);
  }
  if (first < second) {
    return (-1);
  }
  return (0);
} /* zuiCompareByCardinality() */

/* ------------------------------------------------------------------------- */

static int
zuiFind (
  zsetopsrc *op,
  zsetopval *val,
  double    *score
) {
  if (op->subject == NULL) {
    return (0);
  }

  if (op->type == OBJ_SET) {
    if (op->encoding == OBJ_ENCODING_INTSET) {
      if (zuiLongLongFromValue(val) &&
        intsetFind(op->subject->ptr, val->ell)) {
        *score = 1.0;
        return (1);
      } else {
        return (0);
      }
    } else if (op->encoding == OBJ_ENCODING_HT) {
      dict *ht = op->subject->ptr;
      zuiSdsFromValue(val);
      if (dictFind(ht, val->ele) != NULL) {
        *score = 1.0;
        return (1);
      } else {
        return (0);
      }
    } else {
      serverPanic("Unknown set encoding");
    }
  } else if (op->type == OBJ_ZSET) {
    zuiSdsFromValue(val);

    if (op->encoding == OBJ_ENCODING_ZIPLIST) {
      if (zzlFind(op->subject->ptr, val->ele, score) != NULL) {
        /* Score is already set by zzlFind. */
        return (1);
      } else {
        return (0);
      }
    } else if (op->encoding == OBJ_ENCODING_SKIPLIST) {
      zset *zs = op->subject->ptr;
      dictEntry *de;
      if ((de = dictFind(zs->dict, val->ele)) != NULL) {
        *score = *(double *) dictGetVal(de);
        return (1);
      } else {
        return (0);
      }
    } else {
      serverPanic("Unknown sorted set encoding");
    }
  } else {
    serverPanic("Unsupported type");
  }
} /* zuiFind() */

/* ------------------------------------------------------------------------- */

static void
zuiInitIterator (
  zsetopsrc *op
) {
  if (op->subject == NULL) {
    return;
  }

  if (op->type == OBJ_SET) {
    iterset *it = &op->iter.set;
    if (op->encoding == OBJ_ENCODING_INTSET) {
      it->is.is = op->subject->ptr;
      it->is.ii = 0;
    } else if (op->encoding == OBJ_ENCODING_HT) {
      it->ht.dict = op->subject->ptr;
      it->ht.di = dictGetIterator(op->subject->ptr);
      it->ht.de = dictNext(it->ht.di);
    } else {
      serverPanic("Unknown set encoding");
    }
  } else if (op->type == OBJ_ZSET) {
    iterzset *it = &op->iter.zset;
    if (op->encoding == OBJ_ENCODING_ZIPLIST) {
      it->zl.zl = op->subject->ptr;
      it->zl.eptr = ziplistIndex(it->zl.zl, 0);
      if (it->zl.eptr != NULL) {
        it->zl.sptr = ziplistNext(it->zl.zl, it->zl.eptr);
        serverAssert(it->zl.sptr != NULL);
      }
    } else if (op->encoding == OBJ_ENCODING_SKIPLIST) {
      it->sl.zs = op->subject->ptr;
      it->sl.node = it->sl.zs->zsl->header->level[0].forward;
    } else {
      serverPanic("Unknown sorted set encoding");
    }
  } else {
    serverPanic("Unsupported type");
  }
} /* zuiInitIterator() */

/* ------------------------------------------------------------------------- */

static unsigned long
zuiLength (
  zsetopsrc *op
) {
  if (op->subject == NULL) {
    return (0);
  }

  if (op->type == OBJ_SET) {
    if (op->encoding == OBJ_ENCODING_INTSET) {
      return (intsetLen(op->subject->ptr));
    } else if (op->encoding == OBJ_ENCODING_HT) {
      dict *ht = op->subject->ptr;
      return (dictSize(ht));
    } else {
      serverPanic("Unknown set encoding");
    }
  } else if (op->type == OBJ_ZSET) {
    if (op->encoding == OBJ_ENCODING_ZIPLIST) {
      return (zzlLength(op->subject->ptr));
    } else if (op->encoding == OBJ_ENCODING_SKIPLIST) {
      zset *zs = op->subject->ptr;
      return (zs->zsl->length);
    } else {
      serverPanic("Unknown sorted set encoding");
    }
  } else {
    serverPanic("Unsupported type");
  }
} /* zuiLength() */

/* ------------------------------------------------------------------------- */

static int
zuiLongLongFromValue (
  zsetopval *val
) {
  if (!(val->flags & OPVAL_DIRTY_LL)) {
    val->flags |= OPVAL_DIRTY_LL;

    if (val->ele != NULL) {
      if (string2ll(val->ele, sdslen(val->ele), &val->ell)) {
        val->flags |= OPVAL_VALID_LL;
      }
    } else if (val->estr != NULL) {
      if (string2ll((char *) val->estr, val->elen, &val->ell)) {
        val->flags |= OPVAL_VALID_LL;
      }
    } else {
      /* The long long was already set, flag as valid. */
      val->flags |= OPVAL_VALID_LL;
    }
  }
  return (val->flags & OPVAL_VALID_LL);
} /* zuiLongLongFromValue() */

/* ------------------------------------------------------------------------- */

static sds
zuiNewSdsFromValue (
  zsetopval *val
) {
  if (val->flags & OPVAL_DIRTY_SDS) {
    /* We have already one to return! */
    sds ele = val->ele;
    val->flags &= ~OPVAL_DIRTY_SDS;
    val->ele = NULL;
    return (ele);
  } else if (val->ele) {
    return (sdsdup(val->ele));
  } else if (val->estr) {
    return (sdsnewlen((char *) val->estr, val->elen));
  } else {
    return (sdsfromlonglong(val->ell));
  }
} /* zuiNewSdsFromValue() */

/* ------------------------------------------------------------------------- */

static int
zuiNext (
  zsetopsrc *op,
  zsetopval *val
) {
  if (op->subject == NULL) {
    return (0);
  }

  if (val->flags & OPVAL_DIRTY_SDS) {
    sdsfree(val->ele);
  }

  memset(val, 0, sizeof(zsetopval));

  if (op->type == OBJ_SET) {
    iterset *it = &op->iter.set;
    if (op->encoding == OBJ_ENCODING_INTSET) {
      int64_t ell;

      if (!intsetGet(it->is.is, it->is.ii, &ell)) {
        return (0);
      }
      val->ell = ell;
      val->score = 1.0;

      /* Move to next element. */
      it->is.ii++;
    } else if (op->encoding == OBJ_ENCODING_HT) {
      if (it->ht.de == NULL) {
        return (0);
      }
      val->ele = dictGetKey(it->ht.de);
      val->score = 1.0;

      /* Move to next element. */
      it->ht.de = dictNext(it->ht.di);
    } else {
      serverPanic("Unknown set encoding");
    }
  } else if (op->type == OBJ_ZSET) {
    iterzset *it = &op->iter.zset;
    if (op->encoding == OBJ_ENCODING_ZIPLIST) {
      /* No need to check both, but better be explicit. */
      if ((it->zl.eptr == NULL) || (it->zl.sptr == NULL)) {
        return (0);
      }
      serverAssert(ziplistGet(it->zl.eptr, &val->estr, &val->elen, &val->ell));
      val->score = zzlGetScore(it->zl.sptr);

      /* Move to next element. */
      zzlNext(it->zl.zl, &it->zl.eptr, &it->zl.sptr);
    } else if (op->encoding == OBJ_ENCODING_SKIPLIST) {
      if (it->sl.node == NULL) {
        return (0);
      }
      val->ele = it->sl.node->ele;
      val->score = it->sl.node->score;

      /* Move to next element. */
      it->sl.node = it->sl.node->level[0].forward;
    } else {
      serverPanic("Unknown sorted set encoding");
    }
  } else {
    serverPanic("Unsupported type");
  }
  return (1);
} /* zuiNext() */

/* ------------------------------------------------------------------------- */

static sds
zuiSdsFromValue (
  zsetopval *val
) {
  if (val->ele == NULL) {
    if (val->estr != NULL) {
      val->ele = sdsnewlen((char *) val->estr, val->elen);
    } else {
      val->ele = sdsfromlonglong(val->ell);
    }
    val->flags |= OPVAL_DIRTY_SDS;
  }
  return (val->ele);
} /* zuiSdsFromValue() */

/* ------------------------------------------------------------------------- */

inline static void
zunionInterAggregate (
  double *target,
  double  val,
  int     aggregate
) {
  if (aggregate == REDIS_AGGREGATE_TYPE_SUM) {
    *target = *target + val;

    /* The result of adding two doubles is NaN when one variable
     * is +inf and the other is -inf. When these numbers are added,
     * we maintain the convention of the result being 0.0. */
    if (isnan(*target)) {
      *target = 0.0;
    }
  } else if (aggregate == REDIS_AGGREGATE_TYPE_MIN) {
    *target = val < *target ? val : *target;
  } else if (aggregate == REDIS_AGGREGATE_TYPE_MAX) {
    *target = val > *target ? val : *target;
  } else {
    /* safety net */
    serverPanic("Unknown ZUNION/INTER aggregate type");
  }
} /* zunionInterAggregate() */

/* ------------------------------------------------------------------------- */

/* The zunionInterGenericCommand() function is called in order to implement the
 * following commands: ZUNION, ZINTER, ZUNIONSTORE, ZINTERSTORE.
 *
 * 'numkeysIndex' parameter position of key number. for ZUNION/ZINTER command, this
 * value is 1, for ZUNIONSTORE/ZINTERSTORE command, this value is 2.
 *
 * 'op' SET_OP_INTER or SET_OP_UNION.
 */
static void
zunionInterGenericCommand (
  client *c,
  robj   *dstkey,
  int     numkeysIndex,
  int     op
) {
  int i, j;
  long setnum;
  int aggregate = REDIS_AGGREGATE_TYPE_SUM;
  zsetopsrc *src;
  zsetopval zval;
  sds tmp;
  size_t maxelelen = 0;
  robj *dstobj;
  zset *dstzset;
  zskiplistNode *znode;
  int withscores = 0;

  /* expect setnum input keys to be given */
  if ((getLongFromObjectOrReply(c, c->argv[numkeysIndex], &setnum,
    NULL) != C_OK)) {
    return;
  }

  if (setnum < 1) {
    addReplyError(c,
      "at least 1 input key is needed for ZUNIONSTORE/ZINTERSTORE");
    return;
  }

  /* test if the expected number of keys would overflow */
  if (setnum > (c->argc-(numkeysIndex+1))) {
    addReply(c, shared.syntaxerr);
    return;
  }

  /* read keys to be used for input */
  src = zcalloc(sizeof(zsetopsrc) * setnum);
  for (i = 0, j = numkeysIndex+1; i < setnum; i++, j++) {
    robj *obj = lookupKeyWrite(c->db, c->argv[j]);
    if (obj != NULL) {
      if ((obj->type != OBJ_ZSET) && (obj->type != OBJ_SET)) {
        zfree(src);
        addReply(c, shared.wrongtypeerr);
        return;
      }

      src[i].subject = obj;
      src[i].type = obj->type;
      src[i].encoding = obj->encoding;
    } else {
      src[i].subject = NULL;
    }

    /* Default all weights to 1. */
    src[i].weight = 1.0;
  }

  /* parse optional extra arguments */
  if (j < c->argc) {
    int remaining = c->argc - j;

    while (remaining) {
      if ((remaining >= (setnum + 1)) &&
        !strcasecmp(c->argv[j]->ptr, "weights")) {
        j++;
        remaining--;
        for (i = 0; i < setnum; i++, j++, remaining--) {
          if (getDoubleFromObjectOrReply(c, c->argv[j], &src[i].weight,
            "weight value is not a float") != C_OK) {
            zfree(src);
            return;
          }
        }
      } else if ((remaining >= 2) &&
        !strcasecmp(c->argv[j]->ptr, "aggregate")) {
        j++;
        remaining--;
        if (!strcasecmp(c->argv[j]->ptr, "sum")) {
          aggregate = REDIS_AGGREGATE_TYPE_SUM;
        } else if (!strcasecmp(c->argv[j]->ptr, "min")) {
          aggregate = REDIS_AGGREGATE_TYPE_MIN;
        } else if (!strcasecmp(c->argv[j]->ptr, "max")) {
          aggregate = REDIS_AGGREGATE_TYPE_MAX;
        } else {
          zfree(src);
          addReply(c, shared.syntaxerr);
          return;
        }
        j++;
        remaining--;
      } else if ((remaining >= 1) &&
        !strcasecmp(c->argv[j]->ptr, "withscores")) {
        j++;
        remaining--;
        withscores = 1;
      } else {
        zfree(src);
        addReply(c, shared.syntaxerr);
        return;
      }
    }
  }

  /* sort sets from the smallest to largest, this will improve our
   * algorithm's performance */
  qsort(src, setnum, sizeof(zsetopsrc), zuiCompareByCardinality);

  dstobj = createZsetObject();
  dstzset = dstobj->ptr;
  memset(&zval, 0, sizeof(zval));

  if (op == SET_OP_INTER) {
    /* Skip everything if the smallest input is empty. */
    if (zuiLength(&src[0]) > 0) {
      /* Precondition: as src[0] is non-empty and the inputs are ordered
       * by size, all src[i > 0] are non-empty too. */
      zuiInitIterator(&src[0]);
      while (zuiNext(&src[0], &zval)) {
        double score, value;

        score = src[0].weight * zval.score;
        if (isnan(score)) {
          score = 0;
        }

        for (j = 1; j < setnum; j++) {
          /* It is not safe to access the zset we are
           * iterating, so explicitly check for equal object. */
          if (src[j].subject == src[0].subject) {
            value = zval.score*src[j].weight;
            zunionInterAggregate(&score, value, aggregate);
          } else if (zuiFind(&src[j], &zval, &value)) {
            value *= src[j].weight;
            zunionInterAggregate(&score, value, aggregate);
          } else {
            break;
          }
        }

        /* Only continue when present in every input. */
        if (j == setnum) {
          tmp = zuiNewSdsFromValue(&zval);
          znode = zslInsert(dstzset->zsl, score, tmp);
          dictAdd(dstzset->dict, tmp, &znode->score);
          if (sdslen(tmp) > maxelelen) {
            maxelelen = sdslen(tmp);
          }
        }
      }
      zuiClearIterator(&src[0]);
    }
  } else if (op == SET_OP_UNION) {
    dict *accumulator = dictCreate(&setAccumulatorDictType, NULL);
    dictIterator *di;
    dictEntry *de, *existing;
    double score;

    if (setnum) {
      /* Our union is at least as large as the largest set.
       * Resize the dictionary ASAP to avoid useless rehashing. */
      dictExpand(accumulator, zuiLength(&src[setnum-1]));
    }

    /* Step 1: Create a dictionary of elements -> aggregated-scores
     * by iterating one sorted set after the other. */
    for (i = 0; i < setnum; i++) {
      if (zuiLength(&src[i]) == 0) {
        continue;
      }

      zuiInitIterator(&src[i]);
      while (zuiNext(&src[i], &zval)) {
        /* Initialize value */
        score = src[i].weight * zval.score;
        if (isnan(score)) {
          score = 0;
        }

        /* Search for this element in the accumulating dictionary. */
        de = dictAddRaw(accumulator, zuiSdsFromValue(&zval), &existing);
        /* If we don't have it, we need to create a new entry. */
        if (!existing) {
          tmp = zuiNewSdsFromValue(&zval);

          /* Remember the longest single element encountered,
           * to understand if it's possible to convert to ziplist
           * at the end. */
          if (sdslen(tmp) > maxelelen) {
            maxelelen = sdslen(tmp);
          }
          /* Update the element with its initial score. */
          dictSetKey(accumulator, de, tmp);
          dictSetDoubleVal(de, score);
        } else {
          /* Update the score with the score of the new instance
           * of the element found in the current sorted set.
           *
           * Here we access directly the dictEntry double
           * value inside the union as it is a big speedup
           * compared to using the getDouble/setDouble API. */
          zunionInterAggregate(&existing->v.d, score, aggregate);
        }
      }
      zuiClearIterator(&src[i]);
    }

    /* Step 2: convert the dictionary into the final sorted set. */
    di = dictGetIterator(accumulator);

    /* We now are aware of the final size of the resulting sorted set,
     * let's resize the dictionary embedded inside the sorted set to the
     * right size, in order to save rehashing time. */
    dictExpand(dstzset->dict, dictSize(accumulator));

    while ((de = dictNext(di)) != NULL) {
      sds ele = dictGetKey(de);
      score = dictGetDoubleVal(de);
      znode = zslInsert(dstzset->zsl, score, ele);
      dictAdd(dstzset->dict, ele, &znode->score);
    }
    dictReleaseIterator(di);
    dictRelease(accumulator);
  } else {
    serverPanic("Unknown operator");
  }

  if (dstkey) {
    if (dstzset->zsl->length) {
      zsetConvertToZiplistIfNeeded(dstobj, maxelelen);
      setKey(c, c->db, dstkey, dstobj);
      addReplyLongLong(c, zsetLength(dstobj));
      notifyKeyspaceEvent(NOTIFY_ZSET,
        (op == SET_OP_UNION) ? "zunionstore" : "zinterstore",
        dstkey, c->db->id);
      server.dirty++;
    } else {
      addReply(c, shared.czero);
      if (dbDelete(c->db, dstkey)) {
        signalModifiedKey(c, c->db, dstkey);
        notifyKeyspaceEvent(NOTIFY_GENERIC, "del", dstkey, c->db->id);
        server.dirty++;
      }
    }
  } else {
    unsigned long length = dstzset->zsl->length;
    zskiplist *zsl = dstzset->zsl;
    zskiplistNode *zn = zsl->header->level[0].forward;
    if (withscores && (c->resp == 2)) {
      addReplyArrayLen(c, length*2);
    } else {
      addReplyArrayLen(c, length);
    }

    while (zn != NULL) {
      if (withscores && (c->resp > 2)) {
        addReplyArrayLen(c, 2);
      }
      addReplyBulkCBuffer(c, zn->ele, sdslen(zn->ele));
      if (withscores) {
        addReplyDouble(c, zn->score);
      }
      zn = zn->level[0].forward;
    }
  }
  decrRefCount(dstobj);
  zfree(src);
} /* zunionInterGenericCommand() */

/* ------------------------------------------------------------------------- */

static unsigned char *
zzlDelete (
  unsigned char *zl,
  unsigned char *eptr
) {
  unsigned char *p = eptr;

  /* TODO: add function to ziplist API to delete N elements from offset. */
  zl = ziplistDelete(zl, &p);
  zl = ziplistDelete(zl, &p);
  return (zl);
} /* zzlDelete() */

/* ------------------------------------------------------------------------- */

static unsigned char *
zzlDeleteRangeByLex (
  unsigned char *zl,
  zlexrangespec *range,
  unsigned long *deleted
) {
  unsigned char *eptr, *sptr;
  unsigned long num = 0;

  if (deleted != NULL) {
    *deleted = 0;
  }

  eptr = zzlFirstInLexRange(zl, range);
  if (eptr == NULL) {
    return (zl);
  }

  /* When the tail of the ziplist is deleted, eptr will point to the sentinel
   * byte and ziplistNext will return NULL. */
  while ((sptr = ziplistNext(zl, eptr)) != NULL) {
    if (zzlLexValueLteMax(eptr, range)) {
      /* Delete both the element and the score. */
      zl = ziplistDelete(zl, &eptr);
      zl = ziplistDelete(zl, &eptr);
      num++;
    } else {
      /* No longer in range. */
      break;
    }
  }

  if (deleted != NULL) {
    *deleted = num;
  }
  return (zl);
} /* zzlDeleteRangeByLex() */

/* ------------------------------------------------------------------------- */

static unsigned char *
zzlDeleteRangeByRank (
  unsigned char *zl,
  unsigned int   start,
  unsigned int   end,
  unsigned long *deleted
) {
  unsigned int num = (end-start)+1;

  if (deleted) {
    *deleted = num;
  }
  zl = ziplistDeleteRange(zl, 2*(start-1), 2*num);
  return (zl);
} /* zzlDeleteRangeByRank() */

/* ------------------------------------------------------------------------- */

static unsigned char *
zzlDeleteRangeByScore (
  unsigned char *zl,
  zrangespec    *range,
  unsigned long *deleted
) {
  unsigned char *eptr, *sptr;
  double score;
  unsigned long num = 0;

  if (deleted != NULL) {
    *deleted = 0;
  }

  eptr = zzlFirstInRange(zl, range);
  if (eptr == NULL) {
    return (zl);
  }

  /* When the tail of the ziplist is deleted, eptr will point to the sentinel
   * byte and ziplistNext will return NULL. */
  while ((sptr = ziplistNext(zl, eptr)) != NULL) {
    score = zzlGetScore(sptr);
    if (zslValueLteMax(score, range)) {
      /* Delete both the element and the score. */
      zl = ziplistDelete(zl, &eptr);
      zl = ziplistDelete(zl, &eptr);
      num++;
    } else {
      /* No longer in range. */
      break;
    }
  }

  if (deleted != NULL) {
    *deleted = num;
  }
  return (zl);
} /* zzlDeleteRangeByScore() */

/* ------------------------------------------------------------------------- */

static unsigned char *
zzlFind (
  unsigned char *zl,
  sds            ele,
  double        *score
) {
  unsigned char *eptr = ziplistIndex(zl, 0), *sptr;

  while (eptr != NULL) {
    sptr = ziplistNext(zl, eptr);
    serverAssert(sptr != NULL);

    if (ziplistCompare(eptr, (unsigned char *) ele, sdslen(ele))) {
      /* Matching element, pull out score. */
      if (score != NULL) {
        *score = zzlGetScore(sptr);
      }
      return (eptr);
    }

    /* Move to next element. */
    eptr = ziplistNext(zl, sptr);
  }
  return (NULL);
} /* zzlFind() */

/* ------------------------------------------------------------------------- */

static unsigned char *
zzlInsertAt (
  unsigned char *zl,
  unsigned char *eptr,
  sds            ele,
  double         score
) {
  unsigned char *sptr;
  char scorebuf[128];
  int scorelen;
  size_t offset;

  scorelen = d2string(scorebuf, sizeof(scorebuf), score);
  if (eptr == NULL) {
    zl = ziplistPush(zl, (unsigned char *) ele, sdslen(ele), ZIPLIST_TAIL);
    zl = ziplistPush(zl, (unsigned char *) scorebuf, scorelen, ZIPLIST_TAIL);
  } else {
    /* Keep offset relative to zl, as it might be re-allocated. */
    offset = eptr-zl;
    zl = ziplistInsert(zl, eptr, (unsigned char *) ele, sdslen(ele));
    eptr = zl+offset;

    /* Insert score after the element. */
    serverAssert((sptr = ziplistNext(zl, eptr)) != NULL);
    zl = ziplistInsert(zl, sptr, (unsigned char *) scorebuf, scorelen);
  }
  return (zl);
} /* zzlInsertAt() */

/* ------------------------------------------------------------------------- */

static int
zzlIsInLexRange (
  unsigned char *zl,
  zlexrangespec *range
) {
  unsigned char *p;

  /* Test for ranges that will always be empty. */
  int cmp = sdscmplex(range->min, range->max);

  if ((cmp > 0) || ((cmp == 0) && (range->minex || range->maxex))) {
    return (0);
  }

  p = ziplistIndex(zl, -2);  /* Last element. */
  if (p == NULL) {
    return (0);
  }
  if (!zzlLexValueGteMin(p, range)) {
    return (0);
  }

  p = ziplistIndex(zl, 0);  /* First element. */
  serverAssert(p != NULL);
  if (!zzlLexValueLteMax(p, range)) {
    return (0);
  }

  return (1);
} /* zzlIsInLexRange() */

/* ------------------------------------------------------------------------- */

static int
zzlIsInRange (
  unsigned char *zl,
  zrangespec    *range
) {
  unsigned char *p;
  double score;

  /* Test for ranges that will always be empty. */
  if ((range->min > range->max) ||
    ((range->min == range->max) && (range->minex || range->maxex))) {
    return (0);
  }

  p = ziplistIndex(zl, -1);  /* Last score. */
  if (p == NULL) {
    return (0);              /* Empty sorted set */
  }
  score = zzlGetScore(p);
  if (!zslValueGteMin(score, range)) {
    return (0);
  }

  p = ziplistIndex(zl, 1);  /* First score. */
  serverAssert(p != NULL);
  score = zzlGetScore(p);
  if (!zslValueLteMax(score, range)) {
    return (0);
  }

  return (1);
} /* zzlIsInRange() */

/* ------------------------------------------------------------------------- */

static unsigned int
zzlLength (
  unsigned char *zl
) {
  return (ziplistLen(zl) / 2);
} /* zzlLength() */

/* :vi set ts=2 et sw=2: */
