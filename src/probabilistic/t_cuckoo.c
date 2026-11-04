/* Native Redis Cuckoo objects.
 *
 * Copyright (c) 2026-present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */
#include "server.h"
#include "t_cuckoo.h"
#include "endianconv.h"

robj *createCuckooObject(CuckooFilter *filter) {
    robj *o = createObject(OBJ_CUCKOO, filter);
    o->encoding = OBJ_ENCODING_CUCKOO;
    return o;
}

void freeCuckooObject(robj *o) {
    CuckooFilter_Free(o->ptr);
    zfree(o->ptr);
}

robj *cuckooDup(const robj *o) {
    const CuckooFilter *src = o->ptr;
    CuckooFilter *dst = zmalloc(sizeof(*dst));
    *dst = *src;
    dst->filters = zmalloc(src->numFilters * sizeof(*dst->filters));
    memcpy(dst->filters, src->filters, src->numFilters * sizeof(*dst->filters));
    for (size_t i = 0; i < src->numFilters; i++) {
        SubCF *filter = &dst->filters[i];
        size_t bytes = (size_t)filter->numBuckets * filter->bucketSize;
        filter->data = zmalloc(bytes);
        memcpy(filter->data, src->filters[i].data, bytes);
    }
    return createCuckooObject(dst);
}

size_t cuckooAllocSize(const robj *o) {
    const CuckooFilter *cf = o->ptr;
    size_t size = zmalloc_size((void *)cf) + zmalloc_size(cf->filters);
    for (size_t i = 0; i < cf->numFilters; i++) size += zmalloc_size(cf->filters[i].data);
    return size;
}

void cuckooDismiss(robj *o) {
    CuckooFilter *cf = o->ptr;
    for (size_t i = 0; i < cf->numFilters; i++) {
        SubCF *filter = &cf->filters[i];
        dismissMemory(filter->data, (size_t)filter->numBuckets * filter->bucketSize);
    }
    dismissMemory(cf->filters, cf->numFilters * sizeof(*cf->filters));
    dismissMemory(cf, sizeof(*cf));
}

/* Revisit metadata on every call: insertions and compaction can change the
 * filter array between incremental defrag steps. */
unsigned long cuckooDefrag(robj *o, unsigned long cursor, size_t steps,
                          void *(*defrag)(void *)) {
    CuckooFilter *cf = o->ptr;
    void *moved = defrag(cf);
    if (moved) o->ptr = cf = moved;
    if ((moved = defrag(cf->filters))) cf->filters = moved;
    while (cursor < cf->numFilters && steps--) {
        SubCF *filter = &cf->filters[cursor++];
        if ((moved = defrag(filter->data))) filter->data = moved;
    }
    return cursor < cf->numFilters ? cursor : 0;
}

void cuckooMixDigest(unsigned char *digest, const robj *o) {
    const CuckooFilter *cf = o->ptr;
    uint64_t meta[] = {cf->numBuckets, cf->numItems, cf->numDeletes, cf->numFilters,
                       cf->bucketSize, cf->maxIterations, cf->expansion};
    for (size_t i = 0; i < sizeof(meta) / sizeof(*meta); i++) memrev64ifbe(&meta[i]);
    mixDigest(digest, meta, sizeof(meta));
    for (size_t i = 0; i < cf->numFilters; i++) {
        const SubCF *filter = &cf->filters[i];
        uint64_t fields[] = {filter->numBuckets, filter->bucketSize};
        for (size_t j = 0; j < sizeof(fields) / sizeof(*fields); j++) memrev64ifbe(&fields[j]);
        mixDigest(digest, fields, sizeof(fields));
        mixDigest(digest, filter->data, (size_t)filter->numBuckets * filter->bucketSize);
    }
}
