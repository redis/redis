/*
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

/* -----------------------------------------------------------------------------
 * SetEntry
 * -----------------------------------------------------------------------------
 * A SetEntry is a member of a hashtable-encoded set that may carry an
 * expiration (member TTL). The SetEntry pointer is identical to the member sds
 * pointer, so it can be used directly as the dict key, exactly like the plain
 * sds member of a set without expirations.
 *
 * There are two forms:
 *
 * Without expiration: a plain sds of any type (a member that never had a TTL
 *     is exactly what it was before member expiration existed):
 *
 *             entry
 *               |
 *     +---------V------------+
 *     |        Member        |
 *     | sdshdr  | "foo" \0   |
 *     +---------+------------+
 *
 *     Identified by: the has-expiry aux bit is clear. SDS_TYPE_5 has no aux
 *     bits and always reads as "no expiration".
 *
 * With expiration: an ExpireMeta prefix, then the member sds. The ExpireMeta is
 *     what makes the entry an item of the set's private ebuckets tree. The sds
 *     type must be SDS_TYPE_8 or larger, since only those have aux bits to
 *     record the presence of the prefix.
 *
 *                            entry
 *                              |
 *     +--------------+---------V------------+
 *     | ExpireMeta   |        Member        |
 *     |              | sdshdr8+ | "foo" \0  |
 *     +--------------+----------+-----------+
 *
 *     Identified by: the has-expiry aux bit is set.
 *
 * Both forms are odd pointers (an sds header is odd-sized and the prefix is a
 * multiple of 8), which is what the dict (keys_are_odd) and ebuckets
 * (itemsAddrAreOdd) require.
 *
 * Because the aux bits are lost when an sds is auto-resized, a member must
 * never be resized in place. Adding or removing an expiration reallocates the
 * entry, and the caller must replace the pointer it stores (for example with
 * dictSetKeyAtLink).
 */
#ifndef _SET_ENTRY_H_
#define _SET_ENTRY_H_

#include <sys/types.h>
#include "sds.h"
#include "ebuckets.h"

typedef struct _setEntry SetEntry;

/* SDS aux bit. If set, the entry has an ExpireMeta located right before the
 * member sds. */
#define SET_ENTRY_AUX_BIT_HAS_EXPIRY 0

/* The accessors below are inline since they run per member on the hot paths
 * of every hashtable-encoded set, including the sets without expirations. */

/* The SetEntry pointer is the member sds pointer. */
static inline sds sEntryGetMember(const SetEntry *sEntry) {
    return (sds)sEntry;
}

/* Returns true if the entry carries an ExpireMeta (it has an expiration slot,
 * which is not necessarily registered in an ebuckets tree yet). */
static inline int sEntryHasExpiry(const SetEntry *sEntry) {
    return sdsGetAuxBit(sEntryGetMember(sEntry), SET_ENTRY_AUX_BIT_HAS_EXPIRY);
}

/* Returns the address of the entry allocation. */
void *sEntryGetAllocPtr(const SetEntry *sEntry);

/* Returns a reference to the ExpireMeta if present, NULL otherwise. This is the
 * accessor for the EbucketsType of the set's private expiration tree. */
ExpireMeta *sEntryRefExpiryMeta(SetEntry *sEntry);

/* Returns the expiration time (UNIX time in milliseconds) or
 * EB_EXPIRE_TIME_INVALID if the entry has none or is not registered in an
 * ebuckets tree. */
static inline uint64_t sEntryGetExpiry(const SetEntry *sEntry) {
    if (!sEntryHasExpiry(sEntry))
        return EB_EXPIRE_TIME_INVALID;
    ExpireMeta *expireMeta = (ExpireMeta *)sEntryGetAllocPtr(sEntry);
    if (expireMeta->trash)
        return EB_EXPIRE_TIME_INVALID;
    return ebGetMetaExpTime(expireMeta);
}

/* Returns 1 if the entry has an expiration time that has passed, so the member
 * must be treated as if it did not exist. Counterpart of hEntryIsExpired() of
 * hash fields. */
int sEntryIsExpired(const SetEntry *sEntry);

/* Creates a new entry holding a copy of the given member. If withExpiry is set
 * the entry has an ExpireMeta that is not yet registered in an ebuckets tree
 * (its expiration reads as invalid until ebAdd() is called). Without expiry the
 * result is a plain sds. */
SetEntry *sEntryCreate(const char *member, size_t len, int withExpiry);

/* Reallocates a plain entry (no expiration) into one that carries an ExpireMeta,
 * and frees the old one. Returns the new entry. If usableDiff is not NULL it is
 * set to the difference in memory usage (new - old). */
SetEntry *sEntryAddExpiry(SetEntry *sEntry, ssize_t *usableDiff);

/* Reallocates an entry that carries an ExpireMeta into a plain entry and frees
 * the old one. The entry must not be registered in an ebuckets tree (remove it
 * with ebRemove() first). Returns the new entry. If usableDiff is not NULL it is
 * set to the difference in memory usage (new - old). */
SetEntry *sEntryRemoveExpiry(SetEntry *sEntry, ssize_t *usableDiff);

/* Memory used by the entry, based on the sds alloc fields. This is used for
 * size accounting and must be consistent with sEntryFree(). */
static inline size_t sEntryMemUsage(const SetEntry *sEntry) {
    size_t size = sdsAllocSize(sEntryGetMember(sEntry));
    if (sEntryHasExpiry(sEntry)) size += sizeof(ExpireMeta);
    return size;
}

/* Frees the entry. If usable is not NULL it is set to the entry memory usage. */
void sEntryFree(SetEntry *sEntry, size_t *usable);

/* Advises the allocator to dismiss the memory used by the entry. Only to be
 * used in a forked child, see dismissMemory(). */
void sEntryDismissMemory(SetEntry *sEntry);

#ifdef REDIS_TEST
int sEntryTest(int argc, char **argv, int flags);
#endif

#endif
