/*
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Copyright (c) 2024-present, Valkey contributors.
 * All rights reserved.
 */

#include "server.h"
#include "redisassert.h"
#include "entry.h"

/* Aggregates parameters for entry layout and serialization.
 * Populated by setEntryWriteInfo() and consumed by needsNewAlloc() and entryWrite*(). */
typedef struct EntryWriteInfo {
    int isEmbdNewVal;          /* Whether value should be embedded */
    int embdFieldType;         /* SDS type for the field */
    size_t embdFieldSize;      /* Allocated size for SDS field */
    size_t embdValueSize;      /* Required size for new embd value (SDS_TYPE_8) */
    size_t expirySize;         /* Size of expiry metadata (0 if none) */
    size_t newEntryAllocSize;  /* Total allocation size needed for new entry */
    uint32_t flags;            /* Entry creation/update flags */
} EntryWriteInfo;

enum {
    /* SDS aux flag. If set, it indicates that the entry has TTL metadata set. */
    FIELD_SDS_AUX_BIT_ENTRY_HAS_EXPIRY = 0,
    /* SDS aux flag. If set, it indicates that the entry has an embedded value
     * pointer located in memory before the embedded field. If unset, the entry
     * instead has an embedded value located after the embedded field. */
    FIELD_SDS_AUX_BIT_ENTRY_HAS_VALUE_PTR = 1,
    FIELD_SDS_AUX_BIT_ENTRY_MAX
};
static_assert(FIELD_SDS_AUX_BIT_ENTRY_MAX + SDS_TYPE_BITS < sizeof(char) * CHAR_BIT, 
              "too many sds bits are used for entry metadata");

/* Returns true if the entry's value is not embedded (stored by pointer). */
static inline int entryHasValuePtr(const Entry *entry) {
    return sdsGetAuxBit(entryGetField(entry), FIELD_SDS_AUX_BIT_ENTRY_HAS_VALUE_PTR);
}

/* Returns true if the entry's value is embedded in the entry. */
static inline int entryHasEmbeddedValue(Entry *entry) {
    return !entryHasValuePtr(entry);
}

/* Returns true if the entry has an expiration timestamp. */
int entryHasExpiry(const Entry *entry) {
    return sdsGetAuxBit(entryGetField(entry), FIELD_SDS_AUX_BIT_ENTRY_HAS_EXPIRY);
}

/* Returns the location of a pointer to a separately allocated value. Only for
 * an entry without an embedded value. */
static sds *entryGetValueRef(const Entry *entry) {
    serverAssert(entryHasValuePtr(entry));
    char *fieldHdr = sdsAllocPtr(entryGetField(entry));
    char *valuePtr = fieldHdr - sizeof(sds);
    return (sds *) valuePtr;
}

/* A pointer to the value pointer. If embedded or doesn't have a value, returns NULL. */
sds *entryGetValuePtrRef(const Entry *entry) {
    return entryHasValuePtr(entry) ? entryGetValueRef(entry) : NULL;
}

/* Returns the sds of the entry's value. */
sds entryGetValue(const Entry *entry) {
    if (entryHasValuePtr(entry)) {
        return *entryGetValueRef(entry);
    } else {
        /* Skip field content, field null terminator and value sds8 hdr. */
        size_t offset = sdslen(entryGetField(entry)) + 1 + sdsHdrSize(SDS_TYPE_8);
        return (sds)((char *)entry + offset);
    }
}

/* Returns the address of the entry allocation. */
void *entryGetAllocPtr(const Entry *entry) {
    char *buf = sdsAllocPtr(entryGetField(entry));
    if (entryHasValuePtr(entry)) buf -= sizeof(sds);
    if (entryHasExpiry(entry)) buf -= sizeof(ExpireMeta);
    return buf;
}

/**************************************** Entry Expiry API *****************************************/

/* Returns the entry expiration timestamp, or EB_EXPIRE_TIME_INVALID */
uint64_t entryGetExpiry(const Entry *entry) {
    if (!entryHasExpiry(entry))
        return EB_EXPIRE_TIME_INVALID;

    ExpireMeta *expireMeta = (ExpireMeta *)entryGetAllocPtr(entry);
    if (expireMeta->trash)
        return EB_EXPIRE_TIME_INVALID;

    return ebGetMetaExpTime(expireMeta);
}

int entryIsExpired(const Entry *entry) {
    if (server.allow_access_expired)
        return 0;

    /* Condition remains valid even if entryGetExpiry() returns EB_EXPIRE_TIME_INVALID,
     * as the constant is equivalent to (EB_EXPIRE_TIME_MAX + 1). */
    uint64_t expireTime = entryGetExpiry(entry);
    return (mstime_t)expireTime < commandTimeSnapshot();
}

/**************************************** Entry Expiry API - End *****************************************/

/* Calculate entry allocation size based on SDS alloc fields.
 * This is used for size accounting. */
size_t entryMemUsage(Entry *entry) {
    size_t size = sdsAllocSize(entryGetField(entry));
    size += sdsAllocSize(entryGetValue(entry));
    if (entryHasValuePtr(entry)) size += sizeof(sds);
    if (entryHasExpiry(entry)) size += sizeof(ExpireMeta);
    return size;
}

void entryFree(Entry *entry, size_t *usable) {
    if (usable) *usable = entryMemUsage(entry);

    if (entryHasValuePtr(entry))
        sdsfree(entryGetValue(entry));

    zfree(entryGetAllocPtr(entry));
}

/* Determine the appropriate SDS type for the field based on length and expiry.
 * SDS_TYPE_5 cannot be used if expiry is present (no aux bits available). */
static inline int entryCalcFieldSdsType(size_t fieldLen, int hasExpiry) {
    int sdsType = sdsReqType(fieldLen);
    if (sdsType == SDS_TYPE_5 && hasExpiry) {
        sdsType = SDS_TYPE_8;
    }
    return sdsType;
}

/* Calculate required size and layout for an entry */
static inline void setEntryWriteInfo(EntryWriteInfo *info, sds field, sds value, uint32_t flags) {
    info->flags = flags;

    /* Calculate expiry allocation size */
    info->expirySize = (flags & ENTRY_HAS_EXPIRY) ? sizeof(ExpireMeta) : 0;

    /* Calculate field allocation size */
    size_t fieldLen = sdslen(field);
    info->embdFieldType = entryCalcFieldSdsType(fieldLen, info->expirySize > 0);
    info->embdFieldSize = sdsReqSize(fieldLen, info->embdFieldType);
    info->isEmbdNewVal = 0;

    /* Calculate value allocation size (Always use SDS_TYPE_8 for embedded values) */
    size_t valueLen = value ? sdslen(value) : 0;
    info->embdValueSize = value ? sdsReqSize(valueLen, SDS_TYPE_8) : 0;

    /* Start with field and expiry */
    size_t allocSize = info->embdFieldSize + info->expirySize;
    
    if (unlikely(!value)) {
        info->newEntryAllocSize = allocSize;
        return;
    }
    
    /* Decide whether to embed the value or use a pointer */
    if (allocSize + info->embdValueSize <= EMBED_VALUE_MAX_ALLOC_SIZE) {
        /* Embed field and value (SDS_TYPE_8). Unused space in value's SDS header.
         *   [ExpireMeta | Field hdr "foo"\0 | Value hdr8 "bar"\0] 
         */
        info->isEmbdNewVal = 1;
        allocSize += info->embdValueSize;
    } else {
        /* Embed field only (>= SDS_TYPE_8 to encode value ptr flag).
         *   [ExpireMeta | Value Ptr | Field hdr8 "foo"\0] 
         */
        info->isEmbdNewVal = 0;
        allocSize += sizeof(sds);

        /* Upgrade field to SDS_TYPE_8 if needed for aux bits */
        if (info->embdFieldType == SDS_TYPE_5) {
            info->embdFieldType = SDS_TYPE_8;
            info->embdFieldSize = sdsReqSize(fieldLen, info->embdFieldType);
            allocSize = info->embdFieldSize + info->expirySize + sizeof(sds);
        }
    }
    info->newEntryAllocSize = allocSize;
}

/* Serialize the content of the entry into the provided buffer */
static Entry *entryWriteNew(EntryWriteInfo *info, sds field, sds value) {
    char *fieldBuf, *buf = zmalloc(info->newEntryAllocSize);

    /* Take into account expiry metadata if present */
    if (info->expirySize) {
        ExpireMeta *expMeta = (ExpireMeta *)buf;
        expMeta->trash = 1;  /* Mark as trash until added to ebuckets */
        fieldBuf = buf + info->expirySize;
    } else {
        fieldBuf = buf;
    }

    /* Write value (either as pointer or embedded) */
    if (value) {
        if (info->isEmbdNewVal) {
            /* Embed the value after the field - always copy content */
            sdsnewplacement(fieldBuf + info->embdFieldSize, info->embdValueSize,
                          SDS_TYPE_8, value, sdslen(value));
            /* Free value only if ownership was transferred */
            if (info->flags & ENTRY_TAKE_VALUE)
                sdsfree(value);
            info->flags &= ~ENTRY_TAKE_VALUE;
        } else {
            /* Store value as a pointer. dup if ownership wasn't transferred */
            *(sds *)fieldBuf = ((info->flags & ENTRY_TAKE_VALUE)) ? value : sdsdup(value);
            info->flags &= ~ENTRY_TAKE_VALUE;
            fieldBuf += sizeof(sds);
        }
    }

    /* Write the field */
    Entry *newEntry = (Entry *)sdsnewplacement(fieldBuf, info->embdFieldSize,
                                               info->embdFieldType, field, sdslen(field));

    /* Set aux bits to encode entry type */
    sdsSetAuxBit(entryGetField(newEntry), FIELD_SDS_AUX_BIT_ENTRY_HAS_VALUE_PTR, !info->isEmbdNewVal);
    sdsSetAuxBit(entryGetField(newEntry), FIELD_SDS_AUX_BIT_ENTRY_HAS_EXPIRY, info->expirySize > 0);

    /* Verify the entry was built correctly */
    debugServerAssert(entryHasValuePtr(newEntry) == !info->isEmbdNewVal);
    debugServerAssert(entryHasExpiry(newEntry) == (info->expirySize > 0));

    return newEntry;
}

Entry *entryCreate(sds field, sds value, uint32_t flags, size_t *usable) {
    /* Calculate required size and layout */
    EntryWriteInfo info;
    setEntryWriteInfo(&info, field, value, flags);

    /* Allocate and write the entry */
    Entry *entry = entryWriteNew(&info, field, value);

    /* Calculate usable size if requested */
    if (usable)
        *usable = entryMemUsage(entry);

    return entry;
}

/* Helper: Check if we need to create a new entry allocation during update.
 * Returns true if a new allocation is needed, false if we can update in-place. */
static inline int needsNewAlloc(Entry *e,
                                EntryWriteInfo *info,
                                int isUpdateValue,
                                int expiryAddRemove)
{
    /* if we need to add/remove expiration metadata */
    if (expiryAddRemove)
        return 1;
    
    /* If not updating value, no need to allocate new entry */
    if (!isUpdateValue)
        return 0;
    
    /* If value embedding <> pointer changed */
    if (info->isEmbdNewVal != entryHasEmbeddedValue(e))
        return 1;
    
    /* If old & new are both pointers, no need to allocate new entry */
    if (!info->isEmbdNewVal)
        return 0;
    
    /* Check if new embedded value fits in old allocation */
    size_t oldAllocSize = sdsAllocSize(entryGetValue(e));
    size_t newReqSize = info->embdValueSize;
    return !((newReqSize <= oldAllocSize) && (newReqSize >= oldAllocSize * 3 / 4));
}

/* Helper: Update entry in-place */
static Entry *entryWriteOver(Entry *e, EntryWriteInfo *info, sds value)
{
    /* No need to touch expiration metadata. It's done by caller */

    if (entryHasValuePtr(e)) {
        /* Replace pointer value */
        sds *valueRef = entryGetValueRef(e);
        sdsfree(*valueRef);
        *valueRef = (info->flags & ENTRY_TAKE_VALUE) ? value : sdsdup(value);
    } else {
        /* Update embedded value in-place - always copy content */
        sds oldValue = entryGetValue(e);
        /* Use the old value's allocation size */
        size_t valueSize = sdsAllocSize(oldValue);
        sdsnewplacement(sdsAllocPtr(oldValue), valueSize, SDS_TYPE_8, value, sdslen(value));
        /* Free value only if we took ownership */
        if (info->flags & ENTRY_TAKE_VALUE)
            sdsfree(value);
    }
    info->flags &= ~ENTRY_TAKE_VALUE;
    return e;
}

/* Modify the entry's value and/or reserve space for expiration metadata */
Entry *entryUpdate(Entry *entry, sds value, uint32_t flags, ssize_t *usableDiff) {
    /* Check if we need to add/remove expiration metadata */
    int oldHasExpiry = entryHasExpiry(entry) != 0;
    int newHasExpiry = (flags & ENTRY_HAS_EXPIRY) != 0;    
    int expiryAddRemove = (oldHasExpiry != newHasExpiry);
    
    int isUpdateVal = (value != NULL);

    /* Early return if nothing changes */
    if (!isUpdateVal && !expiryAddRemove) {
        if (usableDiff)
            *usableDiff = 0;
        if (flags & ENTRY_TAKE_VALUE)
            sdsfree(value);
        return entry;
    }

    /* Calculate old usable size before any modifications */
    size_t oldUsable = entryMemUsage(entry);

    /* Get the value to use (either provided or existing) */
    if (!value)
        value = entryGetValue(entry);

    /* Calculate required size and layout for the updated entry */
    EntryWriteInfo info;
    setEntryWriteInfo(&info, entryGetField(entry), value, flags);

    /* Decide whether to update in-place or create a new entry */
    Entry *newEntry;
    size_t newUsable;

    if (needsNewAlloc(entry, &info, isUpdateVal, expiryAddRemove)) {
        Entry *oldEntry = entry;
        /* If not updating value */
        if (value == NULL) {
            /* Should not flag ownership of value if not updating value */
            debugServerAssert((info.flags & ENTRY_TAKE_VALUE) == 0);
            
            /* Try reuse the existing value */
            value = entryGetValue(oldEntry);
            
            /* If value is a pointer, we can transfer it from old to new entry  */
            if (entryHasValuePtr(oldEntry)) {
                sds *oldValuePtr = entryGetValueRef(oldEntry);
                *oldValuePtr = NULL;
                info.flags |= ENTRY_TAKE_VALUE;
            }
        }
        
        newEntry = entryWriteNew(&info, entryGetField(oldEntry), value);
        entryFree(oldEntry, NULL);

        newUsable = entryMemUsage(newEntry);
    } else {
        /* Update in-place */
        newEntry = entryWriteOver(entry, &info, value);
        newUsable = entryMemUsage(newEntry);
    }

    /* Calculate the difference in memory usage */
    if (usableDiff)
        *usableDiff = (ssize_t)newUsable - (ssize_t)oldUsable;

    /* Verify the entry was built correctly */
    debugServerAssert(entryHasValuePtr(newEntry) == !info.isEmbdNewVal);
    debugServerAssert(entryHasExpiry(newEntry) == (info.expirySize != 0));
    debugServerAssert((info.flags & ENTRY_TAKE_VALUE) == 0); /* verify the flag is cleared */
    serverAssert(newEntry);

    return newEntry;
}

/* Defragments a hashtable entry (field-value pair) if needed, using the
 * provided defrag functions. The defrag functions return NULL if the allocation
 * was not moved, otherwise they return a pointer to the new memory location.
 * A separate sds defrag function is needed because of the unique memory layout
 * of sds strings.
 * If the location of the entry changed we return the new location,
 * otherwise we return NULL. */
Entry *entryDefrag(Entry *e, void *(*defragfn)(void *), sds (*sdsdefragfn)(sds)) {
    if (entryHasValuePtr(e)) {
        sds *valueRef = entryGetValueRef(e);
        sds newValue = sdsdefragfn(*valueRef);
        if (newValue) *valueRef = newValue;
    }
    char *allocation = entryGetAllocPtr(e);
    char *newAllocation = defragfn(allocation);
    if (newAllocation != NULL) {
        /* Return the same offset into the new allocation as the entry's offset
         * in the old allocation. */
        int entryPointerOffset = (char *)e - allocation;
        return (Entry *)(newAllocation + entryPointerOffset);
    }
    return NULL;
}

/* Used for releasing memory to OS to avoid unnecessary CoW. Called when we've
 * forked and memory won't be used again. See zmadvise_dontneed() */
void entryDismissMemory(Entry *entry) {
    /* Only dismiss values memory since the field size usually is small. */
    if (entryHasValuePtr(entry)) {
        dismissSds(*entryGetValueRef(entry));
    }
}
