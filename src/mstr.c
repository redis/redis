/*
 * Copyright Redis Ltd. 2024 - present
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include <string.h>
#include <assert.h>
#include "sdsalloc.h"
#include "mstr.h"
#include "stdio.h"

#define NULL_SIZE 1

static inline char mstrReqType(size_t string_size);
static inline int mstrHdrSize(char type);
static inline int mstrSumMetaLen(mstrKind *k, mstrFlags flags);
static inline size_t mstrAllocLen(const mstr s, struct mstrKind *kind);

/*** mstr API ***/

/* Create mstr without any metadata attached, based on string 'initStr'.
 * - If initStr equals NULL, then only allocation will be made.
 * - string of mstr is always null-terminated.
 */
mstr mstrNew(const char *initStr, size_t lenStr, int trymalloc) {
    unsigned char *pInfo; /* pointer to mstr info field */
    void *sh;
    mstr s;
    char type = mstrReqType(lenStr);
    int mstrHdr = mstrHdrSize(type);

    assert(lenStr + mstrHdr + 1 > lenStr); /* Catch size_t overflow */

    size_t len = mstrHdr + lenStr + NULL_SIZE;
    sh = trymalloc? s_trymalloc(len) : s_malloc(len);

    if (sh == NULL) return NULL;

    s = (char*)sh + mstrHdr;
    pInfo = ((unsigned char*)s) - 1;

    switch(type) {
        case MSTR_TYPE_5: {
            *pInfo = CREATE_MSTR_INFO(lenStr, 0 /*ismeta*/, type);
            break;
        }
        case MSTR_TYPE_8: {
            MSTR_HDR_VAR(8,s);
            *pInfo = CREATE_MSTR_INFO(0 /*unused*/, 0 /*ismeta*/, type);
            sh->len = lenStr;
            break;
        }
        case MSTR_TYPE_16: {
            MSTR_HDR_VAR(16,s);
            *pInfo = CREATE_MSTR_INFO(0 /*unused*/, 0 /*ismeta*/, type);
            sh->len = lenStr;
            break;
        }
        case MSTR_TYPE_64: {
            MSTR_HDR_VAR(64,s);
            *pInfo = CREATE_MSTR_INFO(0 /*unused*/, 0 /*ismeta*/, type);
            sh->len = lenStr;
            break;
        }
    }

    if (initStr && lenStr)
        memcpy(s, initStr, lenStr);

    s[lenStr] = '\0';
    return s;
}

/* Creates mstr with given string. Reserve space for metadata.
 *
 * Note: mstrNew(s,l) and mstrNewWithMeta(s,l,0) are not the same. The first allocates
 * just string. The second allocates a string with flags (yet without any metadata
 * structures allocated).
 */
mstr mstrNewWithMeta(struct mstrKind *kind, const char *initStr, size_t lenStr, mstrFlags metaFlags, int trymalloc) {
    unsigned char *pInfo; /* pointer to mstr info field */
    char *allocMstr;
    mstr mstrPtr;
    char type = mstrReqType(lenStr);
    int mstrHdr = mstrHdrSize(type);
    int sumMetaLen = mstrSumMetaLen(kind, metaFlags);


    /* mstrSumMetaLen() + sizeof(mstrFlags) + sizeof(mstrhdrX) + lenStr  */

    size_t allocLen = sumMetaLen + sizeof(mstrFlags) + mstrHdr + lenStr + NULL_SIZE;
    allocMstr = trymalloc? s_trymalloc(allocLen) : s_malloc(allocLen);

    if (allocMstr == NULL) return NULL;

    /* metadata is located at the beginning of the allocation, then meta-flags and lastly the string */
    mstrFlags *pMetaFlags = (mstrFlags *) (allocMstr + sumMetaLen) ;
    mstrPtr = ((char*) pMetaFlags) + sizeof(mstrFlags) + mstrHdr;
    pInfo = ((unsigned char*)mstrPtr) - 1;

    switch(type) {
        case MSTR_TYPE_5: {
            *pInfo = CREATE_MSTR_INFO(lenStr, 1 /*ismeta*/, type);
            break;
        }
        case MSTR_TYPE_8: {
            MSTR_HDR_VAR(8, mstrPtr);
            sh->len = lenStr;
            *pInfo = CREATE_MSTR_INFO(0 /*unused*/, 1 /*ismeta*/, type);
            break;
        }
        case MSTR_TYPE_16: {
            MSTR_HDR_VAR(16, mstrPtr);
            sh->len = lenStr;
            *pInfo = CREATE_MSTR_INFO(0 /*unused*/, 1 /*ismeta*/, type);
            break;
        }
        case MSTR_TYPE_64: {
            MSTR_HDR_VAR(64, mstrPtr);
            sh->len = lenStr;
            *pInfo = CREATE_MSTR_INFO(0 /*unused*/, 1 /*ismeta*/, type);
            break;
        }
    }
    *pMetaFlags = metaFlags;
    if (initStr != NULL) memcpy(mstrPtr, initStr, lenStr);
    mstrPtr[lenStr] = '\0';

    return mstrPtr;
}

/* Create copy of mstr. Flags can be modified. For each metadata flag, if
 * same flag is set on both, then copy its metadata. */
mstr mstrNewCopy(struct mstrKind *kind, mstr src, mstrFlags newFlags) {
    mstr dst;

    /* if no flags are set, then just copy the string */
    if (newFlags == 0) return mstrNew(src, mstrlen(src), 0);

    dst = mstrNewWithMeta(kind, src, mstrlen(src), newFlags, 0);
    memcpy(dst, src, mstrlen(src) + 1);

    /* if metadata is attached to src, then selectively copy metadata */
    if (mstrIsMetaAttached(src)) {
        mstrFlags *pFlags1 = mstrFlagsRef(src),
                *pFlags2 = mstrFlagsRef(dst);

        mstrFlags flags1Shift = *pFlags1,
                flags2Shift = *pFlags2;

        unsigned char *at1 = ((unsigned char *) pFlags1),
                *at2 = ((unsigned char *) pFlags2);

        /* if the flag is set on both, then copy the metadata */
        for (int i = 0; flags1Shift != 0; ++i) {
            int isFlag1Set = flags1Shift & 0x1;
            int isFlag2Set = flags2Shift & 0x1;

            if (isFlag1Set) at1 -= kind->metaSize[i];
            if (isFlag2Set) at2 -= kind->metaSize[i];

            if (isFlag1Set && isFlag2Set)
                memcpy(at2, at1, kind->metaSize[i]);
            flags1Shift >>= 1;
            flags2Shift >>= 1;
        }
    }
    return dst;
}

/* Free mstring. Note, mstrKind is required to eval sizeof metadata and find start
 * of allocation but if mstrIsMetaAttached(s) is false, you can pass NULL as well.
 */
void mstrFree(struct mstrKind *kind, mstr s) {
    if (s != NULL)
        s_free(mstrGetAllocPtr(kind, s));
}

/* return ref to metadata flags. Useful to modify directly flags which doesn't
 * include metadata payload */
mstrFlags *mstrFlagsRef(mstr s) {
    switch(s[-1]&MSTR_TYPE_MASK) {
        case MSTR_TYPE_5:
            return ((mstrFlags *) (s - sizeof(struct mstrhdr5))) - 1;
        case MSTR_TYPE_8:
            return ((mstrFlags *) (s - sizeof(struct mstrhdr8))) - 1;
        case MSTR_TYPE_16:
            return ((mstrFlags *) (s - sizeof(struct mstrhdr16))) - 1;
        default: /* MSTR_TYPE_64: */
            return ((mstrFlags *) (s - sizeof(struct mstrhdr64))) - 1;
    }
}

/* Return a reference to corresponding metadata of the specified metadata flag
 * index (flagIdx). If the metadata doesn't exist, it still returns a reference
 * to the starting location where it would have been written among other metadatas.
 * To verify if `flagIdx` of some metadata is attached, use `mstrGetFlag(s, flagIdx)`.
 */
void *mstrMetaRef(mstr s, struct mstrKind *kind, int flagIdx) {
    int metaOffset = 0;
    /* start iterating from flags backward */
    mstrFlags *pFlags = mstrFlagsRef(s);
    mstrFlags tmp = *pFlags;

    for (int i = 0 ; i <= flagIdx ; ++i) {
        if (tmp & 0x1) metaOffset += kind->metaSize[i];
        tmp >>= 1;
    }
    return ((char *)pFlags) - metaOffset;
}

/* mstr layout: [meta-data#N]...[meta-data#0][mstrFlags][mstrhdr][string][null] */
void *mstrGetAllocPtr(struct mstrKind *kind, mstr str) {
    if (!mstrIsMetaAttached(str))
        return (char*)str - mstrHdrSize(str[-1]);

    int totalMetaLen = mstrSumMetaLen(kind, *mstrFlagsRef(str));
    return (char*)str - mstrHdrSize(str[-1]) - sizeof(mstrFlags) - totalMetaLen;
}

/* Prints in the following fashion:
 *   [0x7f8bd8816017] my_mstr: foo (strLen=3, mstrLen=11, isMeta=1, metaFlags=0x1)
 *   [0x7f8bd8816010] >> meta[0]: 0x78 0x56 0x34 0x12 (metaLen=4)
 */
void mstrPrint(mstr s, struct mstrKind *kind, int verbose) {
    mstrFlags mflags, tmp;
    int isMeta = mstrIsMetaAttached(s);

    tmp = mflags = (isMeta) ? *mstrFlagsRef(s) : 0;

    if (!isMeta) {
        printf("[%p] %s: %s (strLen=%zu, mstrLen=%zu, isMeta=0)\n",
               (void *)s, kind->name, s, mstrlen(s), mstrAllocLen(s, kind));
        return;
    }

    printf("[%p] %s: %s (strLen=%zu, mstrLen=%zu, isMeta=1, metaFlags=0x%x)\n",
           (void *)s, kind->name, s, mstrlen(s), mstrAllocLen(s, kind),  mflags);

    if (verbose) {
        for (unsigned int i = 0 ; i < NUM_MSTR_FLAGS ; ++i) {
            if (tmp & 0x1) {
                int mSize = kind->metaSize[i];
                void *mRef = mstrMetaRef(s, kind, i);
                printf("[%p] >> meta[%d]:", mRef, i);
                for (int j = 0 ; j < mSize ; ++j) {
                    printf(" 0x%02x", ((unsigned char *) mRef)[j]);
                }
                printf(" (metaLen=%d)\n", mSize);
            }
            tmp >>= 1;
        }
    }
}

/* return length of the string (ignoring metadata attached) */
size_t mstrlen(const mstr s) {
    unsigned char info = s[-1];
    switch(info & MSTR_TYPE_MASK) {
        case MSTR_TYPE_5:
            return MSTR_TYPE_5_LEN(info);
        case MSTR_TYPE_8:
            return MSTR_HDR(8,s)->len;
        case MSTR_TYPE_16:
            return MSTR_HDR(16,s)->len;
        default: /* MSTR_TYPE_64: */
            return MSTR_HDR(64,s)->len;
    }
}

/*** mstr internals ***/

static inline int mstrSumMetaLen(mstrKind *k, mstrFlags flags) {
    int total = 0;
    int i = 0 ;
    while (flags) {
        total += (flags & 0x1) ? k->metaSize[i] : 0;
        flags >>= 1;
        ++i;
    }
    return total;
}

/* mstrSumMetaLen() + sizeof(mstrFlags) + sizeof(mstrhdrX) + strlen + '\0' */
static inline size_t mstrAllocLen(const mstr s, struct mstrKind *kind) {
    int hdrlen;
    mstrFlags *pMetaFlags;
    size_t strlen = 0;

    int isMeta = mstrIsMetaAttached(s);
    unsigned char info = s[-1];

    switch(info & MSTR_TYPE_MASK) {
        case MSTR_TYPE_5:
            strlen = MSTR_TYPE_5_LEN(info);
            hdrlen = sizeof(struct mstrhdr5);
            pMetaFlags = ((mstrFlags *) MSTR_HDR(5, s)) - 1;
            break;
        case MSTR_TYPE_8:
            strlen = MSTR_HDR(8,s)->len;
            hdrlen = sizeof(struct mstrhdr8);
            pMetaFlags = ((mstrFlags *) MSTR_HDR(8, s)) - 1;
            break;
        case MSTR_TYPE_16:
            strlen = MSTR_HDR(16,s)->len;
            hdrlen = sizeof(struct mstrhdr16);
            pMetaFlags = ((mstrFlags *) MSTR_HDR(16, s)) - 1;
            break;
        default: /* MSTR_TYPE_64: */
            strlen = MSTR_HDR(64,s)->len;
            hdrlen = sizeof(struct mstrhdr64);
            pMetaFlags = ((mstrFlags *) MSTR_HDR(64, s)) - 1;
            break;
    }
    return hdrlen + strlen + NULL_SIZE + ((isMeta) ? (mstrSumMetaLen(kind, *pMetaFlags) + sizeof(mstrFlags)) : 0);
}

/* returns pointer to the beginning of malloc() of mstr */
void *mstrGetStartAlloc(mstr s, struct mstrKind *kind) {
    int hdrlen;
    mstrFlags *pMetaFlags;

    int isMeta = mstrIsMetaAttached(s);

    switch(s[-1]&MSTR_TYPE_MASK) {
        case MSTR_TYPE_5:
            hdrlen = sizeof(struct mstrhdr5);
            pMetaFlags = ((mstrFlags *) MSTR_HDR(5, s)) - 1;
            break;
        case MSTR_TYPE_8:
            hdrlen = sizeof(struct mstrhdr8);
            pMetaFlags = ((mstrFlags *) MSTR_HDR(8, s)) - 1;
            break;
        case MSTR_TYPE_16:
            hdrlen = sizeof(struct mstrhdr16);
            pMetaFlags = ((mstrFlags *) MSTR_HDR(16, s)) - 1;
            break;
        default: /* MSTR_TYPE_64: */
            hdrlen = sizeof(struct mstrhdr64);
            pMetaFlags = ((mstrFlags *) MSTR_HDR(64, s)) - 1;
            break;
    }
    return (char *) s - hdrlen -  ((isMeta) ? (mstrSumMetaLen(kind, *pMetaFlags) + sizeof(mstrFlags)) : 0);
}

static inline int mstrHdrSize(char type) {
    switch(type&MSTR_TYPE_MASK) {
        case MSTR_TYPE_5:
            return sizeof(struct mstrhdr5);
        case MSTR_TYPE_8:
            return sizeof(struct mstrhdr8);
        case MSTR_TYPE_16:
            return sizeof(struct mstrhdr16);
        case MSTR_TYPE_64:
            return sizeof(struct mstrhdr64);
    }
    return 0;
}

static inline char mstrReqType(size_t string_size) {
    if (string_size < 1<<5)
        return MSTR_TYPE_5;
    if (string_size < 1<<8)
        return MSTR_TYPE_8;
    if (string_size < 1<<16)
        return MSTR_TYPE_16;
    return MSTR_TYPE_64;
}

#ifdef REDIS_TEST
#include <stdlib.h>
#include <assert.h>
#include "testhelp.h"
#include "limits.h"

#ifndef UNUSED
#define UNUSED(x) (void)(x)
#endif

/* Challenge mstr with metadata interesting enough that can include the case of hfield and hkey and more */
#define B(idx)  (1<<(idx))

#define META_IDX_MYMSTR_TTL4             0
#define META_IDX_MYMSTR_TTL8             1
#define META_IDX_MYMSTR_TYPE_ENC_LRU     2       // 4Bbit type, 4bit encoding, 24bits lru
#define META_IDX_MYMSTR_VALUE_PTR        3
#define META_IDX_MYMSTR_FLAG_NO_META     4

#define TEST_CONTEXT(context) printf("\nContext: %s \n", context);

int mstrTest(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    struct mstrKind kind_mymstr = {
            .name = "my_mstr",
            .metaSize[META_IDX_MYMSTR_TTL4]           = 4,
            .metaSize[META_IDX_MYMSTR_TTL8]           = 8,
            .metaSize[META_IDX_MYMSTR_TYPE_ENC_LRU]   = 4,
            .metaSize[META_IDX_MYMSTR_VALUE_PTR]      = 8,
            .metaSize[META_IDX_MYMSTR_FLAG_NO_META]   = 0,
    };

    TEST_CONTEXT("Create simple short mstr")
    {
        char *str = "foo";
        mstr s = mstrNew(str, strlen(str), 0);
        size_t expStrLen = strlen(str);

        test_cond("Verify str length and alloc length",
                  mstrAllocLen(s, NULL) == (1 + expStrLen + 1) &&   /* mstrhdr5 + str + null */
                  mstrlen(s) == expStrLen &&                             /* expected strlen(str) */
                  memcmp(s, str, expStrLen + 1) == 0);
        mstrFree(&kind_mymstr, s);
    }

    TEST_CONTEXT("Create simple 40 bytes mstr")
    {
        char *str = "0123456789012345678901234567890123456789"; // 40 bytes
        mstr s = mstrNew(str, strlen(str), 0);

        test_cond("Verify str length and alloc length",
                  mstrAllocLen(s, NULL) == (3 + 40 + 1) &&   /* mstrhdr8 + str + null */
                  mstrlen(s) == 40 &&
                  memcmp(s,str,40) == 0);
        mstrFree(&kind_mymstr, s);
    }

    TEST_CONTEXT("Create mstr with random characters")
    {
        long unsigned int i;
        char str[66000];
        for (i = 0 ; i < sizeof(str) ; ++i) str[i] = rand() % 256;

        size_t len[] = { 31, 32, 33, 255, 256, 257, 65535, 65536, 65537, 66000};
        for (i = 0 ; i < sizeof(len) / sizeof(len[0]) ; ++i) {
            char title[100];
            mstr s = mstrNew(str, len[i], 0);
            size_t mstrhdrSize = (len[i] < 1<<5) ? sizeof(struct mstrhdr5) :
                            (len[i] < 1<<8) ? sizeof(struct mstrhdr8) :
                            (len[i] < 1<<16) ? sizeof(struct mstrhdr16) :
                            sizeof(struct mstrhdr64);

            snprintf(title, sizeof(title), "Verify string of length %zu", len[i]);
            test_cond(title,
                      mstrAllocLen(s, NULL) == (mstrhdrSize + len[i] + 1) &&   /* mstrhdrX + str + null */
                      mstrlen(s) == len[i] &&
                      memcmp(s,str,len[i]) == 0);
            mstrFree(&kind_mymstr, s);
        }
    }

    TEST_CONTEXT("Create short mstr with TTL4")
    {
        uint32_t *ttl;
        mstr s = mstrNewWithMeta(&kind_mymstr,
                                 "foo",
                                 strlen("foo"),
                                 B(META_IDX_MYMSTR_TTL4), /* allocate with TTL4 metadata */
                                 0);

        ttl = mstrMetaRef(s, &kind_mymstr, META_IDX_MYMSTR_TTL4);
        *ttl = 0x12345678;

        test_cond("Verify memory-allocation and string lengths",
                  mstrAllocLen(s, &kind_mymstr) == (1 + 3 + 2 + 1 + 4) && /* mstrhdr5 + str + null + mstrFlags + TLL */
                  mstrlen(s) == 3);

        unsigned char expMem[] = {0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x1c, 'f', 'o', 'o', '\0' };
        uint32_t value = 0x12345678;
        memcpy(expMem, &value, sizeof(uint32_t));
        test_cond("Verify string and TTL4 payload", memcmp(
                mstrMetaRef(s, &kind_mymstr, 0) , expMem, sizeof(expMem)) == 0);

        test_cond("Verify mstrIsMetaAttached() function works", mstrIsMetaAttached(s) != 0);

        mstrFree(&kind_mymstr, s);
    }

    TEST_CONTEXT("Create short mstr with TTL4 and value ptr ")
    {
        mstr s = mstrNewWithMeta(&kind_mymstr, "foo", strlen("foo"),
                                 B(META_IDX_MYMSTR_TTL4) | B(META_IDX_MYMSTR_VALUE_PTR), 0);
        *((uint32_t *) (mstrMetaRef(s, &kind_mymstr,
                                    META_IDX_MYMSTR_TTL4))) = 0x12345678;

        test_cond("Verify length and alloc length",
                  mstrAllocLen(s, &kind_mymstr) == (1 + 3 + 1 + 2 + 4 + 8) && /* mstrhdr5 + str + null + mstrFlags + TLL + PTR */
                  mstrlen(s) == 3);
        mstrFree(&kind_mymstr, s);
    }

    TEST_CONTEXT("Copy mstr and add it TTL4")
    {
        mstr s1 = mstrNew("foo", strlen("foo"), 0);
        mstr s2 = mstrNewCopy(&kind_mymstr, s1, B(META_IDX_MYMSTR_TTL4));
        *((uint32_t *) (mstrMetaRef(s2, &kind_mymstr, META_IDX_MYMSTR_TTL4))) = 0x12345678;

        test_cond("Verify new mstr includes TTL4",
                  mstrAllocLen(s2, &kind_mymstr) == (1 + 3 + 1 + 2 + 4) &&   /* mstrhdr5 + str + null + mstrFlags + TTL4 */
                  mstrlen(s2) == 3 &&                   /* 'foo' = 3bytes */
                  memcmp(s2, "foo\0", 4) == 0);

        mstr s3 = mstrNewCopy(&kind_mymstr, s2, B(META_IDX_MYMSTR_TTL4));
        unsigned char expMem[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0x1, 0x0, 0x1c, 'f', 'o', 'o', '\0' };
        uint32_t value = 0x12345678;
        memcpy(expMem, &value, sizeof(uint32_t));

        char *ppp = mstrGetStartAlloc(s3, &kind_mymstr);
        test_cond("Verify string and TTL4 payload",
                  memcmp(ppp, expMem, sizeof(expMem)) == 0);

        mstrPrint(s3, &kind_mymstr, 1);
        mstrFree(&kind_mymstr, s1);
        mstrFree(&kind_mymstr, s2);
        mstrFree(&kind_mymstr, s3);
    }

    return 0;
}
#endif
