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
 * STAGE 2 STUBS: the commands parse and validate their arguments for real, and
 * reply for missing keys and wrong types for real. For an existing set they
 * only log what they would do ("SME-STUB ..." at notice level) and reply with
 * a fixed placeholder, until the per-member primitives exist. Everything marked
 * "STUB" below is removed when the real implementation lands. */

#include "server.h"
#include "t_set_encoding.h"

/* Retrieve the ExpireMeta associated with the set, used by db->subexpires.
 * The caller is responsible for ensuring that it is indeed attached. */
ExpireMeta *setGetExpireMeta(const eItem set) {
    const robj *o = (const robj *)set;
    if (o->encoding == OBJ_ENCODING_LISTPACK_EX)
        return setListpackExGetExpireMeta(o);
    serverPanic("Unexpected set encoding in subexpires: %d", o->encoding);
}

/* Returns the earliest member expiration time of the set, or
 * EB_EXPIRE_TIME_INVALID if no member has one. 'accurate' requests an exact
 * answer instead of the cached one. */
uint64_t setTypeGetMinExpire(robj *o, int accurate) {
    serverAssert(o->type == OBJ_SET);
    const setTypeOps *ops = setTypeGetOps(o->encoding);
    if (ops->minExpire == NULL) return EB_EXPIRE_TIME_INVALID;
    return ops->minExpire(o, accurate);
}

/* Returns 1 if the set currently has an ExpireMeta attached and can be
 * registered in db->subexpires. */
int setHasSubexpiry(const kvobj *o) {
    serverAssert(o->type == OBJ_SET);
    return o->encoding == OBJ_ENCODING_LISTPACK_EX;
}

/* Returns 1 if the encoding of the set can hold member expirations. */
int setTypeHasExpireSupport(const robj *set) {
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
    const setTypeOps *ops = setTypeGetOps(set->encoding);
    if (ops->getExpire == NULL) {
        *expire = EB_EXPIRE_TIME_INVALID;
        return setTypeIsMember(set, member);
    }
    char tmpbuf[LONG_STR_SIZE];
    (void)tmpbuf;
    return ops->getExpire(set, member, sdslen(member), 0, 1, expire);
}

/* Converts a set that cannot hold member expirations (an intset or a listpack)
 * to the listpack with expirations encoding, so that members can get an
 * expiration. Returns 1 if the set can hold member expirations afterwards, and 0
 * if it cannot, because it needs an encoding that is not implemented yet (a
 * hashtable with expirations). */
int setTypeConvertToExpireEncoding(robj *set) {
    if (setTypeHasExpireSupport(set)) return 1;
    if (set->encoding == OBJ_ENCODING_HT) return 0;

    /* The listpack limits apply: members count, not listpack elements. */
    if (setTypeSize(set) > server.set_max_listpack_entries) return 0;
    if (set->encoding == OBJ_ENCODING_INTSET) {
        /* An intset member is at most 20 characters long. */
        if (server.set_max_listpack_value < 20) return 0;
    }
    setTypeConvertAndExpand(set, OBJ_ENCODING_LISTPACK_EX, setTypeSize(set), 1);
    return 1;
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

/*-----------------------------------------------------------------------------
 * STUB: per-member work, logged instead of performed
 *----------------------------------------------------------------------------*/

static void smeStubLog(const char *what, client *c, robj *member, long long expire, int flags) {
    serverLog(LL_NOTICE, "SME-STUB %s key=%s member=%s expire_ms=%lld flags=%d",
              what, (char *)c->argv[1]->ptr, (char *)member->ptr, expire, flags);
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

    kvobj *set = lookupKeyWrite(c->db, c->argv[1]);
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

    /* STUB: log every member and reply "condition not met". */
    addReplyArrayLen(c, args.memberCount);
    for (int i = 0; i < args.memberCount; i++) {
        smeStubLog("setExpire", c, c->argv[args.firstMemberPos + i],
                   args.expireTime, args.expireCondition);
        addReplyLongLong(c, 0);
    }
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

static void sttlGenericCommand(client *c, const char *name, long long basetime, int unit) {
    UNUSED(basetime);
    UNUSED(unit);
    long numMembers = 0;

    kvobj *set = lookupKeyRead(c->db, c->argv[1]);
    if (checkType(c, set, OBJ_SET))
        return;

    if (parseSetMembersBlock(c, &numMembers) != C_OK)
        return;

    addReplyArrayLen(c, numMembers);
    if (!set) {
        for (long i = 0; i < numMembers; i++)
            addReplyLongLong(c, SME_NO_MEMBER);
        return;
    }

    /* STUB: log every member and reply "no expiration". */
    for (long i = 0; i < numMembers; i++) {
        smeStubLog(name, c, c->argv[4 + i], 0, 0);
        addReplyLongLong(c, SME_NO_TTL);
    }
}

/* STTL key MEMBERS nummembers member [member ...] */
void sttlCommand(client *c) {
    sttlGenericCommand(c, "getTtlSeconds", commandTimeSnapshot(), UNIT_SECONDS);
}

/* SPTTL key MEMBERS nummembers member [member ...] */
void spttlCommand(client *c) {
    sttlGenericCommand(c, "getTtlMilliseconds", commandTimeSnapshot(), UNIT_MILLISECONDS);
}

/* SEXPIRETIME key MEMBERS nummembers member [member ...] */
void sexpiretimeCommand(client *c) {
    sttlGenericCommand(c, "getExpireTimeSeconds", 0, UNIT_SECONDS);
}

/* SPEXPIRETIME key MEMBERS nummembers member [member ...] */
void spexpiretimeCommand(client *c) {
    sttlGenericCommand(c, "getExpireTimeMilliseconds", 0, UNIT_MILLISECONDS);
}

/* SPERSIST key MEMBERS nummembers member [member ...] */
void spersistCommand(client *c) {
    long numMembers = 0;

    kvobj *set = lookupKeyWrite(c->db, c->argv[1]);
    if (checkType(c, set, OBJ_SET))
        return;

    if (parseSetMembersBlock(c, &numMembers) != C_OK)
        return;

    addReplyArrayLen(c, numMembers);
    if (!set) {
        for (long i = 0; i < numMembers; i++)
            addReplyLongLong(c, SME_NO_MEMBER);
        return;
    }

    /* STUB: log every member and reply "no expiration". */
    for (long i = 0; i < numMembers; i++) {
        smeStubLog("persist", c, c->argv[4 + i], 0, 0);
        addReplyLongLong(c, SME_NO_TTL);
    }
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

    if (parseSaddexArgs(c, &flags, &expireTime, &expireTimePos, &firstMemberPos, &memberCount) != C_OK)
        return;

    kvobj *set = lookupKeyWrite(c->db, c->argv[1]);
    if (checkType(c, set, OBJ_SET))
        return;

    /* With MXX all the members must exist, so a missing key means nothing is added. */
    if (!set && (flags & SME_MXX)) {
        addReplyLongLong(c, 0);
        return;
    }

    /* STUB: log every member, change nothing, and reply "nothing was added". */
    for (int i = 0; i < memberCount; i++)
        smeStubLog("add", c, c->argv[firstMemberPos + i], expireTime, flags);
    addReplyLongLong(c, 0);
}
