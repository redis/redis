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

/* The SetEntry pointer is the member sds pointer. */
static inline sds setEntryGetMember(const SetEntry *entry) {
    return (sds)entry;
}

/* Returns true if the entry carries an ExpireMeta (it has an expiration slot,
 * which is not necessarily registered in an ebuckets tree yet). */
int setEntryHasExpiry(const SetEntry *entry);

/* Returns the address of the entry allocation. */
void *setEntryGetAllocPtr(const SetEntry *entry);

/* Returns a reference to the ExpireMeta if present, NULL otherwise. This is the
 * accessor for the EbucketsType of the set's private expiration tree. */
ExpireMeta *setEntryRefExpiryMeta(SetEntry *entry);

/* Returns the expiration time (UNIX time in milliseconds) or
 * EB_EXPIRE_TIME_INVALID if the entry has none or is not registered in an
 * ebuckets tree. */
uint64_t setEntryGetExpiry(const SetEntry *entry);

/* Creates a new entry holding a copy of the given member. If withExpiry is set
 * the entry has an ExpireMeta that is not yet registered in an ebuckets tree
 * (its expiration reads as invalid until ebAdd() is called). Without expiry the
 * result is a plain sds. */
SetEntry *setEntryCreate(const char *member, size_t len, int withExpiry);

/* Reallocates a plain entry (no expiration) into one that carries an ExpireMeta,
 * and frees the old one. Returns the new entry. If usableDiff is not NULL it is
 * set to the difference in memory usage (new - old). */
SetEntry *setEntryAddExpiry(SetEntry *entry, ssize_t *usableDiff);

/* Reallocates an entry that carries an ExpireMeta into a plain entry and frees
 * the old one. The entry must not be registered in an ebuckets tree (remove it
 * with ebRemove() first). Returns the new entry. If usableDiff is not NULL it is
 * set to the difference in memory usage (new - old). */
SetEntry *setEntryRemoveExpiry(SetEntry *entry, ssize_t *usableDiff);

/* Memory used by the entry, based on the sds alloc fields. This is used for
 * size accounting and must be consistent with setEntryFree(). */
size_t setEntryMemUsage(const SetEntry *entry);

/* Frees the entry. If usable is not NULL it is set to the entry memory usage. */
void setEntryFree(SetEntry *entry, size_t *usable);

/* Defragments the entry using the given functions (as entryDefrag()). The
 * defrag functions return NULL if the allocation was not moved, otherwise the
 * new location. Returns the new entry pointer if it moved, NULL otherwise. A
 * separate sds defrag function is used for plain entries because of the unique
 * memory layout of sds strings. */
SetEntry *setEntryDefrag(SetEntry *entry, void *(*defragfn)(void *), sds (*sdsdefragfn)(sds));

/* Advises the allocator to dismiss the memory used by the entry. Only to be
 * used in a forked child, see dismissMemory(). */
void setEntryDismissMemory(SetEntry *entry);

#ifdef REDIS_TEST
int setEntryTest(int argc, char **argv, int flags);
#endif

#endif
