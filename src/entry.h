/*
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Copyright (c) 2024-present, Valkey contributors.
 * All rights reserved.
 */

/* -----------------------------------------------------------------------------
 * Entry 
 * -----------------------------------------------------------------------------
 * An Entry represents a packed field-value pair with optional expiration metadata.
 * Currently it is used only by Hash.
 * 
 * There are 3 different formats for the "entry".  In all cases, the "entry" 
 * pointer points into the allocation and is identical to the "field" sds pointer.
 *
 * Type 1: Field sds type is an SDS_TYPE_5
 *     With this type, both the key and value are embedded in the entry. Expiration 
 *     is not allowed as the SDS_TYPE_5 (on field) doesn't contain any aux bits 
 *     to encode the existence of an expiration.  Extra padding is included in 
 *     the value to the size of the physical block.
 *
 *             entry
 *               |
 *     +---------V------------+----------------------------+
 *     |       Field          |      Value                 |
 *     | sdshdr5 | "foo" \0   | sdshdr8 "bar" \0 (padding) |
 *     +---------+------------+----------------------------+
 *
 *     Identified by: field sds type is SDS_TYPE_5
 *
 *
 * Type 2: Field sds type is an SDS_TYPE_8 type
 *     With this type, both the key and value are embedded.  Extra bits in the 
 *     sdshdr8 (on field) are used to encode aux flags which may indicate the 
 *     presence of an optional expiration. Extra padding is included in the value 
 *     to the size of the physical block.
 *
 *                            entry
 *                              |
 *     +--------------+---------V------------+----------------------------+
 *     | Expire (opt) |       Field          |      Value                 |
 *     |  ExpireMeta  | sdshdr8 | "foo" \0   | sdshdr8 "bar" \0 (padding) |
 *     +--------------+---------+------------+----------------------------+
 *
 *     Identified by: sds type is SDS_TYPE_8  AND  has embedded value
 *
 *
 * Type 3: Value is an sds, referenced by pointer
 *     With this type, the key is embedded, and the value is an sds, referenced 
 *     by pointer.  Extra bits in the sdshdr8/16/32 (on field) are used to encode 
 *     aux flags which indicate the presence of a value by pointer.  An aux bit 
 *     may indicate the presence of an optional expiration.  Note that the 
 *     "field" is not padded, so there's no direct way to identify the length of the allocation.
 *
 *                                             entry
 *                                               |
 *     +--------------+---------------+----------V----------+--------+
 *     | Expire (opt) |     Value     |        Field        | / / / /|
 *     |  ExpireMeta  | sds (pointer) | sdshdr8+ | "foo" \0 |/ / / / |
 *     +--------------+-------+-------+----------+----------+--------+
 *                            |
 *                            +-> sds value
 *
 *     Identified by: Aux bit FIELD_SDS_AUX_BIT_ENTRY_HAS_VALUE_PTR
 */
#ifndef _ENTRY_H_
#define _ENTRY_H_

#include "sds.h"
#include "ebuckets.h"
typedef struct _entry Entry;

/* The maximum allocation size we want to use for entries with embedded values. */
#define EMBED_VALUE_MAX_ALLOC_SIZE 128

/* Flags for entryCreate() and entryUpdate() */
#define ENTRY_TAKE_VALUE   (1<<1)  /* Take ownership of value if possible (not embedded) */
#define ENTRY_HAS_EXPIRY   (1<<2)  /* Entry has expiration */

/* Returns the value string (sds) from the entry. */
sds entryGetValue(const Entry *entry);

/* A pointer to the value pointer. If embedded or doesn't have a value, returns NULL. */
sds *entryGetValuePtrRef(const Entry *entry);

/* Gets the expiration timestamp (UNIX time in milliseconds). */
uint64_t entryGetExpiry(const Entry *entry);

int entryIsExpired(const Entry *entry);

/* Returns true if the entry has an expiration timestamp set. */
int entryHasExpiry(const Entry *entry);

/* Frees the memory used by the entry (including field/value). */
void entryFree(Entry *entry, size_t *usable);

/* Creates a new entry with the given field, value, and optional expiry.
 * Flags can be ENTRY_TAKE_VALUE (take ownership of value if not embedded) and
 * ENTRY_HAS_EXPIRY (entry has expiration metadata).
 * If usable is not NULL, it will be set to the actual allocated size. */
Entry *entryCreate(sds field, sds value, uint32_t flags, size_t *usable);

/* Updates the value and/or expiry of an existing entry.
 * In case value is NULL, will use the existing entry value.
 * Flags can be ENTRY_TAKE_VALUE (take ownership of value if not embedded) and
 * ENTRY_HAS_EXPIRY (reserve space for existing expiry).
 * If usableDiff not NULL, it will be set to diff in mem usage (newUsable - oldUsable) */
Entry *entryUpdate(Entry *entry, sds value, uint32_t flags, ssize_t *usableDiff);

/* Calculate entry allocation size based on SDS alloc fields.
 * This is used for size accounting. */
size_t entryMemUsage(Entry *entry);

/* Returns the address of the entry allocation. */
void *entryGetAllocPtr(const Entry *entry);

/* Defragments the entry and returns the new pointer (if moved). */
Entry *entryDefrag(Entry *entry, void *(*defragfn)(void *), sds (*sdsdefragfn)(sds));

/* Advises allocator to dismiss memory used by entry. */
void entryDismissMemory(Entry *entry);

/* Get a reference to the expiry metadata if present, NULL otherwise. */
static inline ExpireMeta *entryRefExpiryMeta(Entry *entry) {
    return entryHasExpiry(entry) ? (ExpireMeta *)entryGetAllocPtr(entry) : NULL;
}

/* The entry pointer is the field sds, but that's an implementation detail. */
static inline sds entryGetField(const Entry *entry) {
    /* Note: The Entry pointer is identical to the field sds pointer.
     * This is a fundamental design assumption verified by the implementation. */
    return (sds)entry;
}

static inline size_t entryFieldLen(const Entry *entry) {
    return sdslen(entryGetField(entry));
}

#endif
