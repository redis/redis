/*
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

/* Set member expiration (SME): commands and encoding-independent expiry logic.
 *
 * The set<->subexpires functions near the end of the file are placeholders:
 * no set carries an expiration yet, so they must never be reached with a
 * registered set.
 *
 * Every set can hold member expirations: a set that cannot as it is, is converted
 * to the listpack with expirations or to a hashtable (see
 * setTypeConvertToExpireEncoding()). */

#include "server.h"
#include "t_set_encoding.h"
#include "vector.h"

/* Retrieve the ExpireMeta associated with the set, used by db->subexpires.
 * The caller is responsible for ensuring that it is indeed attached. */
ExpireMeta *setGetExpireMeta(const eItem set) {
    const robj *o = (const robj *)set;
    if (o->encoding == OBJ_ENCODING_LISTPACK_EX)
        return setListpackExGetExpireMeta(o);
    if (o->encoding == OBJ_ENCODING_HT)
        return setHashtableGetExpireMeta(o);
    serverPanic("Unexpected set encoding in subexpires: %d", o->encoding);
}

/* Returns the earliest member expiration time of the set, or
 * EB_EXPIRE_TIME_INVALID if no member has one. 'accurate' requests an exact
 * answer instead of the cached one. */
uint64_t setTypeGetMinExpire(robj *o, int accurate) {
    serverAssert(o->type == OBJ_SET);
    if (!setTypeHasExpireSupport(o)) return EB_EXPIRE_TIME_INVALID;
    return setTypeGetOps(o->encoding)->minExpire(o, accurate);
}

/* Returns 1 if the set currently has an ExpireMeta attached and can be
 * registered in db->subexpires. */
int setHasSubexpiry(const kvobj *o) {
    serverAssert(o->type == OBJ_SET);
    return o->encoding == OBJ_ENCODING_LISTPACK_EX ||
           (o->encoding == OBJ_ENCODING_HT && setHashtableHasExpire(o));
}

/* Returns 1 if the set is registered in db->subexpires, which then holds a
 * pointer to the key object (the active defrag must update it when it moves). */
int setIsRegisteredInSubexpires(const kvobj *set) {
    return setHasSubexpiry(set) &&
           ebGetExpireTime(&subexpiresBucketsType, (kvobj *)set) != EB_EXPIRE_TIME_INVALID;
}

/* Returns 1 if the set can hold member expirations as it is: it is a listpack
 * with expirations, or a hashtable that has the metadata of the expirations. */
int setTypeHasExpireSupport(const robj *set) {
    if (set->encoding == OBJ_ENCODING_HT)
        return setHashtableHasExpire(set);
    return setTypeGetOps(set->encoding)->getExpire != NULL;
}

/* Returns 1 if an expiration time (absolute, in milliseconds) has passed and the
 * member it belongs to must be treated as if it did not exist. Like for hash
 * fields, the expiration time itself is not yet expired, and nothing is expired
 * when access to expired data is allowed (a debug switch). */
int setTypeExpireTimeElapsed(uint64_t expire) {
    if (expire == EB_EXPIRE_TIME_INVALID) return 0;
    if (server.allow_access_expired) return 0;
    return expire < (uint64_t)commandTimeSnapshot();
}

/* Looks up a member. Returns 0 if it does not exist and 1 if it does, and then
 * sets *expire to its expiration time, EB_EXPIRE_TIME_INVALID if it has none.
 * A member that is logically expired but not removed yet is found. */
int setTypeGetExpire(robj *set, sds member, uint64_t *expire) {
    if (!setTypeHasExpireSupport(set)) {
        *expire = EB_EXPIRE_TIME_INVALID;
        return setTypeIsMember(set, member);
    }
    return setTypeGetOps(set->encoding)->getExpire(set, member, sdslen(member), 0, 1, expire);
}

/* Returns 1 if the set can hold member expirations now or after being converted.
 * Every set can: a hashtable gets the metadata of the expirations, and a set that
 * is too big for a listpack is converted to a hashtable. */
int setTypeCanHoldExpire(const robj *set) {
    UNUSED(set);
    return 1;
}

/* Prepares a set to hold member expirations: a set that cannot hold them as it is
 * (an intset or a listpack) is converted to the listpack with expirations
 * encoding if its members fit in a listpack, or to a hashtable otherwise, and a
 * hashtable gets the metadata of the expirations. */
void setTypeConvertToExpireEncoding(robj *set) {
    if (setTypeHasExpireSupport(set)) return;

    if (set->encoding != OBJ_ENCODING_HT) {
        /* The listpack limits apply: members count, not listpack elements. */
        int fitsListpack = setTypeSize(set) <= server.set_max_listpack_entries;
        /* An intset member is at most 20 characters long. */
        if (set->encoding == OBJ_ENCODING_INTSET && server.set_max_listpack_value < 20)
            fitsListpack = 0;
        if (fitsListpack) {
            setTypeConvertAndExpand(set, OBJ_ENCODING_LISTPACK_EX, setTypeSize(set), 1);
            return;
        }
        setTypeConvert(set, OBJ_ENCODING_HT);
    }
    setHashtableAddExpireSupport(set);
}

/*-----------------------------------------------------------------------------
 * Flags and reply codes
 *----------------------------------------------------------------------------*/

/* Conditions of the SEXPIRE family (evaluated per member). */
#define SME_NX       (1<<0) /* Set only if the member has no expiration */
#define SME_XX       (1<<1) /* Set only if the member has an expiration */
#define SME_GT       (1<<2) /* Set only if the new expiration is greater */
#define SME_LT       (1<<3) /* Set only if the new expiration is less */

/* Flags of SADDEX. */
#define SME_EX       (1<<0) /* Expiration time in seconds */
#define SME_PX       (1<<1) /* Expiration time in milliseconds */
#define SME_EXAT     (1<<2) /* Expiration time in unix seconds */
#define SME_PXAT     (1<<3) /* Expiration time in unix milliseconds */
#define SME_KEEPTTL  (1<<4) /* Do not discard the member ttl on add */
#define SME_MXX      (1<<5) /* Add only if all the members already exist */
#define SME_MNX      (1<<6) /* Add only if none of the members exist */

/* Reply codes per member, the same as the hash field expiration commands. */
#define SME_NO_MEMBER   (-2) /* The member does not exist */
#define SME_NO_TTL      (-1) /* The member exists but has no expiration */
#define SME_COND_NOT_MET 0   /* The condition (NX, XX, GT, LT) was not met */
#define SME_UPDATED      1   /* The expiration was set or updated */
#define SME_DELETED      2   /* The member was deleted: the time is in the past */
#define SME_PERSISTED    1   /* The expiration was removed */

/*-----------------------------------------------------------------------------
 * Helpers
 *----------------------------------------------------------------------------*/

#define SME_STACK_SIZE 64

/* A vec with an embedded stack buffer, used to collect the member robj pointers
 * for subkey notifications without heap allocation in the common case. */
typedef struct memvec { vec v; void *buf[SME_STACK_SIZE]; } memvec;

static inline vec *memvecInit(memvec *mv, size_t cap) {
    vecInit(&mv->v, mv->buf, SME_STACK_SIZE);
    vecReserve(&mv->v, cap);
    return &mv->v;
}

/* Propagates the removal of a member as an explicit SREM, whatever the reason
 * for the removal is (an expiration time in the past, lazy or active expiry). A
 * replica never decides on its own that a member expired, it only applies the
 * SREM of its master. */
static void propagateSetMemberDeletion(redisDb *db, sds key, char *member, size_t len) {
    robj *argv[] = {
        shared.srem,
        createStringObject((char*) key, sdslen(key)),
        createStringObject(member, len)
    };

    enterExecutionUnit(1, 0);
    /* The expiration is decided by the server, so it must be propagated even if
     * the command that triggered it asked not to propagate. */
    alsoPropagateForced(db->id, argv, 3, PROPAGATE_AOF|PROPAGATE_REPL);
    exitExecutionUnit();
    postExecutionUnitOperations();

    decrRefCount(argv[1]);
    decrRefCount(argv[2]);
}

/* The reply for a member that is logically expired but was not removed yet:
 * treated as if the member did not exist. */
static inline int setMemberIsGone(int exists, uint64_t expire) {
    return !exists || (expire != EB_EXPIRE_TIME_INVALID && (long long)expire < commandTimeSnapshot());
}

/*-----------------------------------------------------------------------------
 * Lazy expiry
 *
 * Like for hash fields, there is no sweep before a command: a command that
 * touches a member checks whether it expired, and a command that samples
 * members removes all the expired ones first.
 *----------------------------------------------------------------------------*/

/* Returns 1 if this node must not remove expired members itself right now: a
 * replica waits for the SREM of its master, nothing is removed while loading or
 * while the expiration actions are paused, and a master link is trusted. *deletable
 * is set to 0 if the expired member must be reported as missing but kept, and
 * *trusted to 1 if it must be reported as present (the master link applying
 * commands the master already decided on). */
static void setLazyExpiryGuard(int *deletable, int *trusted) {
    *deletable = 1;
    *trusted = 0;
    if (server.masterhost || server.cluster_enabled) {
        /* If CLIENT_MASTER, assume valid as long as it was not deleted. In
         * cluster mode, also while importing data from the source (a fake master
         * client with the CLIENT_MASTER flag), to not delete members still in use. */
        if (server.current_client && (server.current_client->flags & CLIENT_MASTER)) {
            *trusted = 1;
            return;
        }
        /* For a replica, if a user client, act as if expired but do not delete. */
        if (server.masterhost) {
            *deletable = 0;
            return;
        }
    }
    if (server.loading || isPausedActionsWithUpdate(PAUSE_ACTION_EXPIRE))
        *deletable = 0;
}

/* Removes a member that was found logically expired: removes it, propagates the
 * SREM, and does the bookkeeping, as per the flags. Deletes the key if the set
 * became empty, unless SET_LAZY_AVOID_SET_DEL. Returns 1 if the key was deleted. */
static int setLazyDeleteExpiredMember(redisDb *db, kvobj *set, sds member, int flags) {
    size_t oldsize = 0;
    sds key = kvobjGetKey(set);
    int64_t oldlen = (int64_t)setTypeSize(set);

    if (server.memory_tracking_enabled && !(flags & SET_LAZY_NO_UPDATE_ALLOCSIZES))
        oldsize = kvobjAllocSize(set);
    serverAssert(setTypeRemove(set, member) == 1);
    if (server.memory_tracking_enabled && !(flags & SET_LAZY_NO_UPDATE_ALLOCSIZES))
        updateSlotAllocSize(db, getKeySlot(key), set, oldsize, kvobjAllocSize(set));
    propagateSetMemberDeletion(db, key, member, sdslen(member));
    server.stat_expired_subkeys++;

    if (!(flags & SET_LAZY_NO_UPDATE_KEYSIZES))
        updateKeysizesHist(db, OBJ_SET, oldlen, oldlen - 1);

    int deleted = 0;
    robj *keyObj = createStringObject(key, sdslen(key));
    unsigned long length = setTypeSize(set);
    if (length != 0 && !(flags & SET_LAZY_NO_NOTIFICATION)) {
        robj mobj, *marr[1] = {&mobj};
        initStaticStringObject(mobj, member);
        notifyKeyspaceEventWithSubkeys(NOTIFY_SET, "sexpired", keyObj, db->id, marr, 1);
    }
    if (length == 0 && !(flags & SET_LAZY_AVOID_SET_DEL)) {
        if (!(flags & SET_LAZY_NO_NOTIFICATION))
            notifyKeyspaceEvent(NOTIFY_GENERIC, "del", keyObj, db->id);
        dbDelete(db, keyObj);
        set = NULL;
        deleted = 1;
    }
    keyModified(NULL, db, keyObj, set, !(flags & SET_LAZY_NO_SIGNAL));
    decrRefCount(keyObj);
    return deleted;
}

/* Checks whether a member is in the set, removing it if it is logically expired
 * and this node is allowed to. Returns 1 if the member is in the set, 0 if it is
 * not or is expired. *setDeleted (if not NULL) is set to 1 if the whole key got
 * deleted, because the expired member was the last one; the set must not be used
 * afterwards. This function also updates the allocation size accounting (a read
 * can move memory, for example by rehashing) unless suppressed. */
int setTypeIsMemberLazy(redisDb *db, kvobj *set, sds member, int flags, int *setDeleted) {
    size_t oldsize = 0;
    const int track = server.memory_tracking_enabled && !(flags & SET_LAZY_NO_UPDATE_ALLOCSIZES);
    if (setDeleted) *setDeleted = 0;
    if (track) oldsize = kvobjAllocSize(set);

    uint64_t expire = EB_EXPIRE_TIME_INVALID;
    int res = setTypeGetExpire(set, member, &expire);
    int remove = 0;

    if (res && setTypeExpireTimeElapsed(expire)) {
        int deletable, trusted;
        setLazyExpiryGuard(&deletable, &trusted);
        if (trusted) {
            res = 1;
        } else {
            res = 0;
            remove = deletable;
        }
    }

    if (remove) {
        int deleted = setLazyDeleteExpiredMember(db, set, member,
                                                 flags | SET_LAZY_NO_UPDATE_ALLOCSIZES);
        /* The accounting is updated before the key is deleted, if it is. */
        if (track && !deleted)
            updateSlotAllocSize(db, getKeySlot(kvobjGetKey(set)), set, oldsize, kvobjAllocSize(set));
        if (setDeleted) *setDeleted = deleted;
        return 0;
    }

    if (track)
        updateSlotAllocSize(db, getKeySlot(kvobjGetKey(set)), set, oldsize, kvobjAllocSize(set));
    return res;
}

/* Callback of the expire() op: propagates the removal of a member and collects
 * it for the keyspace event. */
typedef struct SetExpireCtx {
    redisDb *db;
    sds key;
    vec *vexpired;     /* Collects the expired members, NULL if not needed */
    int activeEx;      /* The expiration is done by the active expire cycle */
} SetExpireCtx;

static void setExpireMemberCb(void *c, char *str, size_t len, int64_t llval) {
    SetExpireCtx *ctx = c;
    char buf[LONG_STR_SIZE];
    if (str == NULL) {
        len = ll2string(buf, sizeof(buf), llval);
        str = buf;
    }
    if (ctx->vexpired)
        vecPush(ctx->vexpired, createStringObject(str, len));
    propagateSetMemberDeletion(ctx->db, ctx->key, str, len);
    server.stat_expired_subkeys++;
    if (ctx->activeEx) server.stat_expired_subkeys_active++;
}

/* Deletes the expired members of the set, up to the quota, and deletes the set
 * if it is left empty.
 *
 * Returns the next expiration time of the set:
 * - 0 if the set was deleted
 * - EB_EXPIRE_TIME_INVALID if no more members have an expiration */
uint64_t setTypeExpire(redisDb *db, kvobj *set, uint32_t *quota, int updateSubexpires, int activeEx) {
    const setTypeOps *ops = setTypeGetOps(set->encoding);
    serverAssert(ops->expire != NULL);
    sds keystr = kvobjGetKey(set);

    /* Collect the expired members for a batched subkey notification, unless the
     * subkey notifications are disabled. */
    memvec mvexpired;
    vec *vexpired = isSubkeyNotifyEnabled(NOTIFY_SET) ?
                        memvecInit(&mvexpired, SME_STACK_SIZE) : NULL;
    SetExpireCtx ctx = {.db = db, .key = keystr, .vexpired = vexpired, .activeEx = activeEx};

    size_t oldsize = 0;
    if (server.memory_tracking_enabled)
        oldsize = kvobjAllocSize(set);
    int64_t oldlen = (int64_t)setTypeSize(set);

    unsigned long expired = ops->expire(set, (uint64_t)commandTimeSnapshot(), *quota,
                                        setExpireMemberCb, &ctx);
    *quota -= expired;

    uint64_t res = EB_EXPIRE_TIME_INVALID;
    if (expired) {
        if (server.memory_tracking_enabled)
            updateSlotAllocSize(db, getKeySlot(keystr), set, oldsize, kvobjAllocSize(set));
        int64_t newlen = (int64_t)setTypeSize(set);
        updateKeysizesHist(db, OBJ_SET, oldlen, newlen);

        robj *key = createStringObject(keystr, sdslen(keystr));
        notifyKeyspaceEventWithSubkeys(NOTIFY_SET, "sexpired", key, db->id,
            vexpired ? (robj**)vecData(vexpired) : NULL, vexpired ? vecSize(vexpired) : 0);

        int deleted = 0;
        if (newlen == 0) {
            notifyKeyspaceEvent(NOTIFY_GENERIC, "del", key, db->id);
            dbDelete(db, key);
            res = 0;
            deleted = 1;
        }
        keyModified(NULL, db, key, deleted ? NULL : set, 1);
        decrRefCount(key);
        if (!deleted) {
            res = ops->minExpire(set, 1);
            /* Not for the active expiry cycle: it updates the registration itself,
             * with the time that this function returns. */
            if (updateSubexpires) setTypeUpdateSubexpiry(db, set);
        }
    } else {
        res = ops->minExpire(set, 1);
    }

    if (vexpired) {
        for (size_t i = 0; i < vecSize(vexpired); i++)
            decrRefCount(vecGet(vexpired, i));
        vecRelease(vexpired);
    }
    return res;
}

/* Deletes all the expired members of the set (and the set itself if it is left
 * empty) unless this node must not delete expired data right now. This is a
 * sweep, used by the commands that sample members, to not pick among many
 * expired members. Returns 1 if the whole set was deleted. */
int setTypeExpireIfNeeded(redisDb *db, kvobj *set) {
    const setTypeOps *ops = setTypeGetOps(set->encoding);
    if (ops->minExpire == NULL) return 0;

    uint64_t minExpire = ops->minExpire(set, 1);
    /* Nothing to expire */
    if (minExpire == EB_EXPIRE_TIME_INVALID || (mstime_t)minExpire >= commandTimeSnapshot())
        return 0;

    /* Follow the conditions of when not to lazy-expire a key. */
    if (server.loading || server.allow_access_expired || server.masterhost ||
        isPausedActionsWithUpdate(PAUSE_ACTION_EXPIRE))
        return 0;

    uint32_t quota = UINT32_MAX;
    return setTypeExpire(db, set, &quota, 1, 0) == 0;
}

/* Registers the set in db->subexpires with its earliest member expiration, or
 * updates or removes the registration, to match the set as it is now. Call it
 * after a change that can make a member expire earlier than the registered time,
 * and when the set may not be registered yet (a new TTL, a new object).
 *
 * Changes that can only make the earliest expiration later (removing a member,
 * lazy expiry) do not need it: the active expiry cycle visits the set at the
 * registered time, finds nothing to expire or less than expected, and registers
 * the right time. */
void setTypeUpdateSubexpiry(redisDb *db, kvobj *set) {
    uint64_t current = setTypeGetMinExpire(set, 1);
    uint64_t registered = EB_EXPIRE_TIME_INVALID;
    int slot = getKeySlot(kvobjGetKey(set));

    if (setHasSubexpiry(set))
        registered = ebGetExpireTime(&subexpiresBucketsType, set);
    if (current == registered) return;

    if (registered == EB_EXPIRE_TIME_INVALID)
        estoreAdd(db->subexpires, slot, set, current);
    else if (current == EB_EXPIRE_TIME_INVALID)
        estoreRemove(db->subexpires, slot, set);
    else
        estoreUpdate(db->subexpires, slot, set, current);
}

/*-----------------------------------------------------------------------------
 * SEXPIRE, SPEXPIRE, SEXPIREAT, SPEXPIREAT
 *
 *   SEXPIRE key seconds [NX | XX | GT | LT] MEMBERS nummembers member [member ...]
 *
 * The keywords can come in any order, as for the HEXPIRE family.
 *----------------------------------------------------------------------------*/

typedef struct {
    int membersPos;         /* Position of the MEMBERS keyword (-1 if not found) */
    int numMembersPos;      /* Position of the nummembers argument */
    int firstMemberPos;     /* Position of the first member */
    int memberCount;        /* Number of members */
    int expireTimePos;      /* Position of the expire time argument */
    long long expireTime;   /* Parsed expire time, absolute in milliseconds */
    int expireCondition;    /* SME_NX, SME_XX, SME_GT, SME_LT */
} SetExpireArgs;

/* Parser for the SEXPIRE family with flexible keyword ordering.
 * Returns C_OK on success, C_ERR on error (with the reply sent). */
static int parseSetExpireArgs(client *c, SetExpireArgs *args, long long basetime, int unit) {
    memset(args, 0, sizeof(*args));
    args->membersPos = -1;
    args->expireTimePos = 2;

    if (parseSubkeyExpireTime(c, c->argv[2], unit, basetime, &args->expireTime) != C_OK)
        return C_ERR;

    /* Parse the remaining arguments starting from position 3 */
    for (int i = 3; i < c->argc; i++) {
        char *arg = c->argv[i]->ptr;

        if (!strcasecmp(arg, "MEMBERS")) {
            if (args->membersPos != -1) {
                addReplyError(c, "MEMBERS keyword specified multiple times");
                return C_ERR;
            }

            if (i >= c->argc - 2) {
                addReplyError(c, "MEMBERS requires at least nummembers and one member argument");
                return C_ERR;
            }

            args->membersPos = i;
            args->numMembersPos = i + 1;
            long numMembers;
            if (getRangeLongFromObjectOrReply(c, c->argv[args->numMembersPos], 1, INT_MAX,
                                              &numMembers,
                                              "Parameter `numMembers` should be greater than 0") != C_OK)
                return C_ERR;

            args->firstMemberPos = i + 2;

            /* We must have exactly the right number of members */
            if (numMembers > c->argc - args->firstMemberPos) {
                addReplyError(c, "wrong number of arguments");
                return C_ERR;
            }

            args->memberCount = (int)numMembers;

            /* Skip over the member arguments */
            i = args->firstMemberPos + args->memberCount - 1;
            continue;
        }

        if (!strcasecmp(arg, "NX")) {
            args->expireCondition |= SME_NX;
        } else if (!strcasecmp(arg, "XX")) {
            args->expireCondition |= SME_XX;
        } else if (!strcasecmp(arg, "GT")) {
            args->expireCondition |= SME_GT;
        } else if (!strcasecmp(arg, "LT")) {
            args->expireCondition |= SME_LT;
        } else {
            addReplyErrorFormat(c, "unknown argument: %s", (char*) c->argv[i]->ptr);
            return C_ERR;
        }
    }

    if (args->membersPos == -1) {
        addReplyError(c, "missing MEMBERS argument");
        return C_ERR;
    }

    if (__builtin_popcount(args->expireCondition & (SME_NX|SME_XX|SME_GT|SME_LT)) > 1) {
        addReplyError(c, "Multiple condition flags specified");
        return C_ERR;
    }

    return C_OK;
}

static void sexpireGenericCommand(client *c, long long basetime, int unit) {
    SetExpireArgs args;
    int64_t oldlen, newlen;
    size_t oldsize = 0;
    robj *keyArg = c->argv[1];

    kvobj *set = lookupKeyWrite(c->db, keyArg);
    if (checkType(c, set, OBJ_SET))
        return;

    if (parseSetExpireArgs(c, &args, basetime, unit) != C_OK)
        return;

    /* Non-existing keys and empty sets are the same thing. It also means the
     * members in the command don't exist in the set. */
    if (!set) {
        addReplyArrayLen(c, args.memberCount);
        for (int i = 0; i < args.memberCount; i++)
            addReplyLongLong(c, SME_NO_MEMBER);
        return;
    }

    oldlen = (int64_t)setTypeSize(set);
    if (server.memory_tracking_enabled)
        oldsize = kvobjAllocSize(set);

    /* Members are collected per outcome, for the subkey notifications. */
    memvec mvupdated, mvdeleted;
    vec *vupdated = memvecInit(&mvupdated, args.memberCount);
    vec *vdeleted = memvecInit(&mvdeleted, args.memberCount);
    /* Positions of the members that were not set, to remove them from the
     * propagated command. */
    int *notSet = NULL;
    int notSetCount = 0;

    addReplyArrayLen(c, args.memberCount);
    for (int i = 0; i < args.memberCount; i++) {
        int pos = args.firstMemberPos + i;
        sds member = c->argv[pos]->ptr;
        uint64_t cur;
        int res;

        int exists = setTypeGetExpire(set, member, &cur);
        if (setMemberIsGone(exists, cur)) {
            res = SME_NO_MEMBER;
        } else if (((args.expireCondition & SME_NX) && cur != EB_EXPIRE_TIME_INVALID) ||
                   ((args.expireCondition & SME_XX) && cur == EB_EXPIRE_TIME_INVALID) ||
                   /* A member without an expiration counts as an infinite one. */
                   ((args.expireCondition & SME_GT) &&
                    (cur == EB_EXPIRE_TIME_INVALID || (uint64_t)args.expireTime <= cur)) ||
                   ((args.expireCondition & SME_LT) &&
                    (cur != EB_EXPIRE_TIME_INVALID && (uint64_t)args.expireTime >= cur)))
        {
            res = SME_COND_NOT_MET;
        } else if (checkAlreadyExpired(args.expireTime)) {
            /* The new expiration is in the past: the member is deleted. */
            setTypeRemove(set, member);
            propagateSetMemberDeletion(c->db, keyArg->ptr, member, sdslen(member));
            vecPush(vdeleted, c->argv[pos]);
            res = SME_DELETED;
        } else {
            setTypeConvertToExpireEncoding(set);
            const setTypeOps *ops = setTypeGetOps(set->encoding);
            serverAssert(ops->setExpire(set, member, sdslen(member), 0, 1, (uint64_t)args.expireTime));
            vecPush(vupdated, c->argv[pos]);
            res = SME_UPDATED;
        }

        if (res != SME_UPDATED) {
            if (notSet == NULL)
                notSet = zmalloc(sizeof(int) * args.memberCount);
            notSet[notSetCount++] = pos;
        }
        addReplyLongLong(c, res);
    }

    if (server.memory_tracking_enabled)
        updateSlotAllocSize(c->db, getKeySlot(keyArg->ptr), set, oldsize, kvobjAllocSize(set));

    if (vecSize(vdeleted) + vecSize(vupdated) > 0) {
        server.dirty += vecSize(vdeleted) + vecSize(vupdated);
        keyModified(c, c->db, keyArg, set, 1);
        if (vecSize(vdeleted))
            notifyKeyspaceEventWithSubkeys(NOTIFY_SET, "srem", keyArg, c->db->id,
                                           (robj**)vecData(vdeleted), vecSize(vdeleted));
        if (vecSize(vupdated))
            notifyKeyspaceEventWithSubkeys(NOTIFY_SET, "sexpire", keyArg, c->db->id,
                                           (robj**)vecData(vupdated), vecSize(vupdated));
    }

    newlen = (int64_t)setTypeSize(set);
    if (newlen == 0) {
        newlen = -1;
        /* Delete the key without updating the keysizes, which is done below. */
        dbDeleteSkipKeysizesUpdate(c->db, keyArg);
        notifyKeyspaceEvent(NOTIFY_GENERIC, "del", keyArg, c->db->id);
    } else if (vecSize(vupdated)) {
        setTypeUpdateSubexpiry(c->db, set);
    }
    if (oldlen != newlen)
        updateKeysizesHist(c->db, OBJ_SET, oldlen, newlen);

    /* If no member was set (the time is in the past and the SREMs were
     * already propagated, or the conditions were not met), propagating the
     * command is useless, and invalid with no members. */
    if (vecSize(vupdated) == 0) {
        preventCommandPropagation(c);
    } else {
        /* Rewrite to the canonical SPEXPIREAT command. */
        if (c->cmd->proc != spexpireatCommand) {
            rewriteClientCommandArgument(c, 0, shared.spexpireat);
            robj *expireTimeObj = createStringObjectFromLongLong(args.expireTime);
            rewriteClientCommandArgument(c, args.expireTimePos, expireTimeObj);
            decrRefCount(expireTimeObj);
        }
        /* For a partial result, remove the members that were not set. */
        if (notSetCount) {
            for (int i = notSetCount - 1; i >= 0; i--)
                rewriteClientCommandArgument(c, notSet[i], NULL);
            robj *count = createStringObjectFromLongLong(vecSize(vupdated));
            rewriteClientCommandArgument(c, args.membersPos + 1, count);
            decrRefCount(count);
        }
    }

    zfree(notSet);
    vecRelease(vupdated);
    vecRelease(vdeleted);
}

/* SEXPIRE key seconds [NX | XX | GT | LT] MEMBERS nummembers member [member ...] */
void sexpireCommand(client *c) {
    sexpireGenericCommand(c, commandTimeSnapshot(), UNIT_SECONDS);
}

/* SPEXPIRE key milliseconds [NX | XX | GT | LT] MEMBERS nummembers member [member ...] */
void spexpireCommand(client *c) {
    sexpireGenericCommand(c, commandTimeSnapshot(), UNIT_MILLISECONDS);
}

/* SEXPIREAT key unix-time-seconds [NX | XX | GT | LT] MEMBERS nummembers member [member ...] */
void sexpireatCommand(client *c) {
    sexpireGenericCommand(c, 0, UNIT_SECONDS);
}

/* SPEXPIREAT key unix-time-milliseconds [NX | XX | GT | LT] MEMBERS nummembers member [member ...] */
void spexpireatCommand(client *c) {
    sexpireGenericCommand(c, 0, UNIT_MILLISECONDS);
}

/*-----------------------------------------------------------------------------
 * STTL, SPTTL, SEXPIRETIME, SPEXPIRETIME and SPERSIST
 *
 *   STTL key MEMBERS nummembers member [member ...]
 *
 * The MEMBERS keyword must be right after the key.
 *----------------------------------------------------------------------------*/

/* Parses and validates the MEMBERS block at its fixed position. Returns C_OK
 * and sets numMembers on success, C_ERR (with the reply sent) otherwise. */
static int parseSetMembersBlock(client *c, long *numMembers) {
    const int numMembersAt = 3;

    if (strcasecmp(c->argv[numMembersAt-1]->ptr, "MEMBERS")) {
        addReplyError(c, "Mandatory argument MEMBERS is missing or not at the right position");
        return C_ERR;
    }

    if (getRangeLongFromObjectOrReply(c, c->argv[numMembersAt], 1, LONG_MAX,
                                      numMembers, "Number of members must be a positive integer") != C_OK)
        return C_ERR;

    /* Verify `numMembers` is consistent with the number of arguments */
    if (*numMembers != (c->argc - numMembersAt - 1)) {
        addReplyError(c, "The `nummembers` parameter must match the number of arguments");
        return C_ERR;
    }
    return C_OK;
}

static void sttlGenericCommand(client *c, long long basetime, int unit) {
    long numMembers = 0;

    kvobj *set = lookupKeyRead(c->db, c->argv[1]);
    if (checkType(c, set, OBJ_SET))
        return;

    if (parseSetMembersBlock(c, &numMembers) != C_OK)
        return;

    addReplyArrayLen(c, numMembers);
    for (long i = 0; i < numMembers; i++) {
        sds member = c->argv[4 + i]->ptr;
        uint64_t expire = EB_EXPIRE_TIME_INVALID;
        /* Non-existing keys and empty sets are the same thing. This command only
         * reads: a member that is logically expired but not removed yet is
         * reported as missing, and nothing is deleted. */
        int exists = set ? setTypeGetExpire(set, member, &expire) : 0;
        if (!exists) {
            addReplyLongLong(c, SME_NO_MEMBER);
        } else if (expire == EB_EXPIRE_TIME_INVALID) {
            addReplyLongLong(c, SME_NO_TTL);
        } else if ((long long)expire < commandTimeSnapshot()) {
            addReplyLongLong(c, SME_NO_MEMBER);
        } else if (unit == UNIT_SECONDS) {
            addReplyLongLong(c, (expire + 999 - basetime) / 1000);
        } else {
            addReplyLongLong(c, expire - basetime);
        }
    }
}

/* STTL key MEMBERS nummembers member [member ...] */
void sttlCommand(client *c) {
    sttlGenericCommand(c, commandTimeSnapshot(), UNIT_SECONDS);
}

/* SPTTL key MEMBERS nummembers member [member ...] */
void spttlCommand(client *c) {
    sttlGenericCommand(c, commandTimeSnapshot(), UNIT_MILLISECONDS);
}

/* SEXPIRETIME key MEMBERS nummembers member [member ...] */
void sexpiretimeCommand(client *c) {
    sttlGenericCommand(c, 0, UNIT_SECONDS);
}

/* SPEXPIRETIME key MEMBERS nummembers member [member ...] */
void spexpiretimeCommand(client *c) {
    sttlGenericCommand(c, 0, UNIT_MILLISECONDS);
}

/* SPERSIST key MEMBERS nummembers member [member ...] */
void spersistCommand(client *c) {
    long numMembers = 0;
    size_t oldsize = 0;
    robj *keyArg = c->argv[1];

    kvobj *set = lookupKeyWrite(c->db, keyArg);
    if (checkType(c, set, OBJ_SET))
        return;

    if (parseSetMembersBlock(c, &numMembers) != C_OK)
        return;

    /* Non-existing keys and empty sets are the same thing. It also means the
     * members in the command don't exist in the set. */
    if (!set) {
        addReplyArrayLen(c, numMembers);
        for (long i = 0; i < numMembers; i++)
            addReplyLongLong(c, SME_NO_MEMBER);
        return;
    }

    /* Track which members were successfully persisted, for the notification. */
    memvec mvpersisted;
    vec *vpersisted = memvecInit(&mvpersisted, numMembers);
    if (server.memory_tracking_enabled)
        oldsize = kvobjAllocSize(set);

    addReplyArrayLen(c, numMembers);
    for (long i = 0; i < numMembers; i++) {
        robj *memberObj = c->argv[4 + i];
        sds member = memberObj->ptr;
        uint64_t expire = EB_EXPIRE_TIME_INVALID;

        int exists = setTypeGetExpire(set, member, &expire);
        if (!exists) {
            addReplyLongLong(c, SME_NO_MEMBER);
        } else if (expire == EB_EXPIRE_TIME_INVALID) {
            addReplyLongLong(c, SME_NO_TTL);
        } else if ((long long)expire < commandTimeSnapshot()) {
            /* Already expired. Pretend there is no such member. */
            addReplyLongLong(c, SME_NO_MEMBER);
        } else {
            serverAssert(setTypeGetOps(set->encoding)->setExpire(set, member, sdslen(member), 0, 1,
                                                                 EB_EXPIRE_TIME_INVALID));
            vecPush(vpersisted, memberObj);
            addReplyLongLong(c, SME_PERSISTED);
        }
    }

    if (server.memory_tracking_enabled)
        updateSlotAllocSize(c->db, getKeySlot(keyArg->ptr), set, oldsize, kvobjAllocSize(set));

    if (vecSize(vpersisted)) {
        setTypeUpdateSubexpiry(c->db, set);
        server.dirty += vecSize(vpersisted);
        keyModified(c, c->db, keyArg, set, 1);
        notifyKeyspaceEventWithSubkeys(NOTIFY_SET, "spersist", keyArg, c->db->id,
                                       (robj**)vecData(vpersisted), vecSize(vpersisted));
    } else {
        /* Nothing changed: nothing to propagate. */
        preventCommandPropagation(c);
    }
    vecRelease(vpersisted);
}

/*-----------------------------------------------------------------------------
 * SADDEX
 *
 *   SADDEX key [MNX | MXX]
 *       [EX seconds | PX milliseconds | EXAT unix-time-seconds | PXAT unix-time-milliseconds | KEEPTTL]
 *       MEMBERS nummembers member [member ...]
 *
 * Reply: Integer 0 if no member was added or updated (due to MNX or MXX),
 *        1 if all the members were added or updated.
 *----------------------------------------------------------------------------*/

/* Parses the SADDEX arguments, in any order of the keywords. Returns C_OK on
 * success, C_ERR (with the reply sent) otherwise. */
static int parseSaddexArgs(client *c, int *flags, long long *expireTime, int *expireTimePos,
                           int *firstMemberPos, int *memberCount) {
    *flags = 0;
    *firstMemberPos = -1;
    *memberCount = -1;
    *expireTimePos = -1;
    const int expireFlags = SME_EX | SME_EXAT | SME_PX | SME_PXAT | SME_KEEPTTL;

    for (int i = 2; i < c->argc; i++) {
        char *arg = c->argv[i]->ptr;

        if (!strcasecmp(arg, "MEMBERS")) {
            /* Ensure only one MEMBERS argument is provided */
            if (*firstMemberPos != -1) {
                addReplyError(c, "MEMBERS keyword specified multiple times");
                return C_ERR;
            }

            long val;
            /* Ensure we have at least the nummembers argument */
            if (i + 1 >= c->argc) {
                addReplyErrorArity(c);
                return C_ERR;
            }

            if (getRangeLongFromObjectOrReply(c, c->argv[i + 1], 1, INT_MAX, &val,
                                              "invalid number of members") != C_OK)
                return C_ERR;

            *firstMemberPos = i + 2;
            *memberCount = (int) val;

            if ((long long)*firstMemberPos + *memberCount > c->argc) {
                addReplyError(c, "wrong number of arguments");
                return C_ERR;
            }

            /* Skip over nummembers and the members. Set i to the last position
             * of the MEMBERS block, the loop will increment past it. */
            i = *firstMemberPos + *memberCount - 1;
            continue;
        }

        int unit = 0, flag = 0;
        long long basetime = 0;
        if (!strcasecmp(arg, "EX")) {
            flag = SME_EX; unit = UNIT_SECONDS; basetime = commandTimeSnapshot();
        } else if (!strcasecmp(arg, "PX")) {
            flag = SME_PX; unit = UNIT_MILLISECONDS; basetime = commandTimeSnapshot();
        } else if (!strcasecmp(arg, "EXAT")) {
            flag = SME_EXAT; unit = UNIT_SECONDS;
        } else if (!strcasecmp(arg, "PXAT")) {
            flag = SME_PXAT; unit = UNIT_MILLISECONDS;
        }

        if (flag) {
            if (*flags & expireFlags) {
                addReplyError(c, "Only one of EX, PX, EXAT, PXAT or KEEPTTL arguments can be specified");
                return C_ERR;
            }
            if (i >= c->argc - 1) {
                addReplyError(c, "missing expire time");
                return C_ERR;
            }
            *flags |= flag;
            i++;
            if (parseSubkeyExpireTime(c, c->argv[i], unit, basetime, expireTime) != C_OK)
                return C_ERR;
            *expireTimePos = i;
        } else if (!strcasecmp(arg, "KEEPTTL")) {
            if (*flags & expireFlags) {
                addReplyError(c, "Only one of EX, PX, EXAT, PXAT or KEEPTTL arguments can be specified");
                return C_ERR;
            }
            *flags |= SME_KEEPTTL;
        } else if (!strcasecmp(arg, "MXX") || !strcasecmp(arg, "MNX")) {
            if (*flags & (SME_MXX | SME_MNX)) {
                addReplyError(c, "Only one of MXX or MNX arguments can be specified");
                return C_ERR;
            }
            *flags |= !strcasecmp(arg, "MXX") ? SME_MXX : SME_MNX;
        } else {
            addReplyErrorFormat(c, "unknown argument: %s", (char*) c->argv[i]->ptr);
            return C_ERR;
        }
    }

    /* Ensure MEMBERS is specified */
    if (*firstMemberPos == -1) {
        addReplyError(c, "missing MEMBERS argument");
        return C_ERR;
    }

    return C_OK;
}

void saddexCommand(client *c) {
    int flags = 0, firstMemberPos = 0, memberCount = 0, expireTimePos = -1;
    long long expireTime = EB_EXPIRE_TIME_INVALID;
    int64_t oldlen, newlen;
    size_t oldsize = 0;
    dictEntryLink link;
    robj *keyArg = c->argv[1];

    if (parseSaddexArgs(c, &flags, &expireTime, &expireTimePos, &firstMemberPos, &memberCount) != C_OK)
        return;

    kvobj *set = lookupKeyWriteWithLink(c->db, keyArg, &link);
    if (checkType(c, set, OBJ_SET))
        return;

    /* With MXX all the members must exist, so a missing key means nothing is added. */
    if (!set && (flags & SME_MXX)) {
        addReplyLongLong(c, 0);
        return;
    }

    const int setsExpire = (flags & (SME_EX | SME_PX | SME_EXAT | SME_PXAT)) != 0;
    /* An expiration in the past deletes the members instead of adding them. */
    const int pastTime = setsExpire && checkAlreadyExpired(expireTime);

    /* The conditions are all or nothing: check them before changing anything. A
     * member that is logically expired but not removed yet does not exist. */
    if (set && (flags & (SME_MXX | SME_MNX))) {
        int found = 0;
        for (int i = 0; i < memberCount; i++) {
            uint64_t cur;
            int exists = setTypeGetExpire(set, c->argv[firstMemberPos + i]->ptr, &cur);
            found += !setMemberIsGone(exists, cur);
        }
        if (((flags & SME_MNX) && found != 0) || ((flags & SME_MXX) && found != memberCount)) {
            addReplyLongLong(c, 0);
            return;
        }
    }

    memvec mvset, mvupdated, mvdeleted;
    vec *vset = memvecInit(&mvset, memberCount);
    vec *vupdated = memvecInit(&mvupdated, memberCount);
    vec *vdeleted = memvecInit(&mvdeleted, memberCount);

    if (pastTime) {
        /* The members that exist are deleted, and the ones that do not exist
         * are not added, so a missing key is not even created. */
        if (set) {
            oldlen = (int64_t)setTypeSize(set);
            if (server.memory_tracking_enabled)
                oldsize = kvobjAllocSize(set);
            for (int i = 0; i < memberCount; i++) {
                robj *memberObj = c->argv[firstMemberPos + i];
                sds member = memberObj->ptr;
                if (setTypeRemove(set, member)) {
                    propagateSetMemberDeletion(c->db, keyArg->ptr, member, sdslen(member));
                    vecPush(vdeleted, memberObj);
                }
            }
        } else {
            oldlen = 0;
        }
    } else {
        if (!set) {
            set = setTypeCreate(c->argv[firstMemberPos]->ptr, memberCount);
            dbAddByLink(c->db, keyArg, &set, &link);
        }
        oldlen = (int64_t)setTypeSize(set);
        if (server.memory_tracking_enabled)
            oldsize = kvobjAllocSize(set);

        if (setsExpire)
            setTypeConvertToExpireEncoding(set);

        for (int i = 0; i < memberCount; i++) {
            robj *memberObj = c->argv[firstMemberPos + i];
            sds member = memberObj->ptr;
            if (setsExpire) {
                /* Add or update the member, and set its expiration. The set
                 * grows to a bigger encoding, keeping the expirations, if needed. */
                setTypeAddExAux(set, member, sdslen(member), 0, 1, (uint64_t)expireTime);
                vecPush(vupdated, memberObj);
            } else if (!setTypeAdd(set, member) && setTypeHasExpireSupport(set) &&
                       !(flags & SME_KEEPTTL))
            {
                /* The member exists: without KEEPTTL, its expiration is discarded. */
                setTypeGetOps(set->encoding)->setExpire(set, member, sdslen(member), 0, 1,
                                                        EB_EXPIRE_TIME_INVALID);
            }
            vecPush(vset, memberObj);
        }
    }

    if (server.memory_tracking_enabled && set)
        updateSlotAllocSize(c->db, getKeySlot(keyArg->ptr), set, oldsize, kvobjAllocSize(set));

    if (vecSize(vset) + vecSize(vdeleted) > 0) {
        server.dirty += vecSize(vset) + vecSize(vdeleted);
        keyModified(c, c->db, keyArg, set, 1);
        if (vecSize(vset))
            notifyKeyspaceEventWithSubkeys(NOTIFY_SET, "sadd", keyArg, c->db->id,
                                           (robj**)vecData(vset), vecSize(vset));
        if (vecSize(vdeleted))
            notifyKeyspaceEventWithSubkeys(NOTIFY_SET, "srem", keyArg, c->db->id,
                                           (robj**)vecData(vdeleted), vecSize(vdeleted));
        if (vecSize(vupdated))
            notifyKeyspaceEventWithSubkeys(NOTIFY_SET, "sexpire", keyArg, c->db->id,
                                           (robj**)vecData(vupdated), vecSize(vupdated));
    }

    if (set) {
        newlen = (int64_t)setTypeSize(set);
        if (newlen == 0) {
            newlen = -1;
            /* Delete the key without updating the keysizes, which is done below. */
            dbDeleteSkipKeysizesUpdate(c->db, keyArg);
            notifyKeyspaceEvent(NOTIFY_GENERIC, "del", keyArg, c->db->id);
        } else if (setTypeHasExpireSupport(set)) {
            setTypeUpdateSubexpiry(c->db, set);
        }
        if (oldlen != newlen)
            updateKeysizesHist(c->db, OBJ_SET, oldlen, newlen);
    }

    if (pastTime) {
        /* The SREMs of the deleted members were already propagated. */
        preventCommandPropagation(c);
    } else if (setsExpire && !(flags & SME_PXAT)) {
        /* Propagate with an absolute time in milliseconds: PXAT. */
        rewriteClientCommandArgument(c, expireTimePos - 1, shared.pxat);
        robj *expire = createStringObjectFromLongLong(expireTime);
        rewriteClientCommandArgument(c, expireTimePos, expire);
        decrRefCount(expire);
    }

    addReplyLongLong(c, 1);

    vecRelease(vset);
    vecRelease(vupdated);
    vecRelease(vdeleted);
}
