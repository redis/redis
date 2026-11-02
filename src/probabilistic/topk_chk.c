/* Native CuckooHeavyKeeper lifecycle and portable persistence.
 * Copyright (c) 2026-Present, Redis Ltd.; RSALv2/SSPLv1/AGPLv3. */
#include "native_topk.h"
#include "xxhash.h"

/* Version 1: engine tag, k, buckets/table, decay, total, packed tables,
 * packed heap metadata, one binary string per occupied heap slot, EOF.
 * Packed integers are little endian; pointers, padding, allocation sizes and
 * derived lookup tables are never serialized. Version 0 remains HeavyKeeper. */
#define CHK_WIRE_BUCKET_SIZE 23 /* 2 * (u16 fingerprint + u64 count) + u16 + u8 */
#define CHK_WIRE_HEAP_SIZE 17   /* u64 fingerprint + u64 count + u8 presence */

static void putInteger(unsigned char **p, uint64_t value, unsigned bytes) {
    while (bytes--) { *(*p)++ = value & 255; value >>= 8; }
}
static uint64_t getInteger(const unsigned char **p, unsigned bytes) {
    uint64_t value = 0;
    for (unsigned i = 0; i < bytes; i++) value |= (uint64_t)*(*p)++ << (i * 8);
    return value;
}

chkHeapBucket *topkChkFind(chkTopK *t, const sds item) {
    uint64_t fp = XXH3_64bits_withSeed(item, sdslen(item), CHK_HEAP_SEED);
    for (int i = 0; i < t->k; i++) {
        chkHeapBucket *entry = &t->heap[i];
        if (entry->fp == fp && entry->item && sdslen(entry->item) == sdslen(item) &&
            !memcmp(entry->item, item, sdslen(item))) return entry;
    }
    return NULL;
}

chkTopK *topkChkDup(chkTopK *t) {
    chkTopK *copy = chkTopKCreate(t->k, t->numbuckets, t->decay);
    copy->total = t->total;
    for (int i = 0; i < CHK_NUM_TABLES; i++)
        memcpy(copy->tables[i], t->tables[i], (size_t)t->numbuckets * sizeof(chkBucket));
    memcpy(copy->heap, t->heap, (size_t)t->k * sizeof(chkHeapBucket));
    for (int i = 0; i < t->k; i++) {
        if (!t->heap[i].item) continue;
        copy->heap[i].item = sdsdup(t->heap[i].item);
        copy->alloc_size += sdsAllocSize(copy->heap[i].item);
    }
    return copy;
}

void topkChkDefrag(chkTopK *t, void *(*defrag)(void *)) {
    void *p;
    for (int i = 0; i < CHK_NUM_TABLES; i++) {
        p = defrag(t->tables[i]); if (p) t->tables[i] = p;
    }
    p = defrag(t->heap); if (p) t->heap = p;
    for (int i = 0; i < t->k; i++) {
        sds item = t->heap[i].item;
        if (!item) continue;
        void *base = sdsAllocPtr(item);
        size_t offset = item - (char *)base;
        p = defrag(base);
        if (p) t->heap[i].item = (char *)p + offset;
    }
}

void topkChkDigest(unsigned char *digest, chkTopK *t) {
    const char *algorithm = "cuckoo-heavykeeper";
    mixDigest(digest, algorithm, strlen(algorithm));
    mixDigest(digest, &t->k, sizeof(t->k));
    mixDigest(digest, &t->numbuckets, sizeof(t->numbuckets));
    mixDigest(digest, &t->decay, sizeof(t->decay));
    mixDigest(digest, &t->total, sizeof(t->total));
    for (int i = 0; i < CHK_NUM_TABLES; i++) {
        for (int j = 0; j < t->numbuckets; j++) {
            chkBucket *bucket = &t->tables[i][j];
            for (int h = 0; h < CHK_HEAVY_ENTRIES_PER_BUCKET; h++) {
                mixDigest(digest, &bucket->heavy_entries[h].fp, sizeof(fingerprint_t));
                mixDigest(digest, &bucket->heavy_entries[h].count, sizeof(counter_t));
            }
            mixDigest(digest, &bucket->lobby_entry.fp, sizeof(fingerprint_t));
            mixDigest(digest, &bucket->lobby_entry.count, sizeof(lobby_counter_t));
        }
    }
    for (int i = 0; i < t->k; i++) {
        mixDigest(digest, &t->heap[i].fp, sizeof(t->heap[i].fp));
        mixDigest(digest, &t->heap[i].count, sizeof(t->heap[i].count));
        if (t->heap[i].item) mixDigest(digest, t->heap[i].item, sdslen(t->heap[i].item));
    }
}

ssize_t topkChkRdbSave(rio *rdb, chkTopK *t) {
    probIO io = {.rdb = rdb};
    addSaved(&io, rdbSaveLen(rdb, probRdbId("TopK-TYPE", 1)));
    saveUnsigned(&io, TOPK_CUCKOO_HEAVYKEEPER);
    saveUnsigned(&io, t->k);
    saveUnsigned(&io, t->numbuckets);
    saveDouble(&io, t->decay);
    saveUnsigned(&io, t->total);

    size_t len = (size_t)t->numbuckets * CHK_NUM_TABLES * CHK_WIRE_BUCKET_SIZE;
    unsigned char *blob = zmalloc(len), *p = blob;
    for (int i = 0; i < CHK_NUM_TABLES; i++) {
        for (int j = 0; j < t->numbuckets; j++) {
            chkBucket *bucket = &t->tables[i][j];
            for (int h = 0; h < CHK_HEAVY_ENTRIES_PER_BUCKET; h++) {
                putInteger(&p, bucket->heavy_entries[h].fp, 2);
                putInteger(&p, bucket->heavy_entries[h].count, 8);
            }
            putInteger(&p, bucket->lobby_entry.fp, 2);
            putInteger(&p, bucket->lobby_entry.count, 1);
        }
    }
    saveString(&io, (char *)blob, len);
    zfree(blob);
    len = (size_t)t->k * CHK_WIRE_HEAP_SIZE;
    blob = zmalloc(len); p = blob;
    for (int i = 0; i < t->k; i++) {
        putInteger(&p, t->heap[i].fp, 8);
        putInteger(&p, t->heap[i].count, 8);
        putInteger(&p, t->heap[i].item != NULL, 1);
    }
    saveString(&io, (char *)blob, len);
    zfree(blob);
    for (int i = 0; i < t->k; i++) {
        if (t->heap[i].item) saveString(&io, t->heap[i].item, sdslen(t->heap[i].item));
    }
    addSaved(&io, rdbSaveLen(rdb, RDB_MODULE_OPCODE_EOF));
    return io.error ? -1 : io.bytes;
}

robj *topkChkRdbLoad(rio *rdb) {
    probIO io = {.rdb = rdb};
    uint64_t algorithm = loadUnsigned(&io), k = loadUnsigned(&io), buckets = loadUnsigned(&io);
    double decay = loadDouble(&io);
    uint64_t total = loadUnsigned(&io);
    /* Only the fixed, supported decay is accepted. Arbitrary values can
     * overflow the engine's floating-point-to-counter conversions. */
    if (io.error || algorithm != TOPK_CUCKOO_HEAVYKEEPER ||
        !k || k > INT_MAX || !buckets || buckets > (1ULL << 30) ||
        (buckets & (buckets - 1)) || decay != TOPK_CHK_DECAY || total > LLONG_MAX ||
        buckets > SIZE_MAX / (CHK_NUM_TABLES * CHK_WIRE_BUCKET_SIZE) ||
        k > SIZE_MAX / CHK_WIRE_HEAP_SIZE) return NULL;

    size_t len = 0;
    unsigned char *tables = loadString(&io, &len), *heap = NULL;
    chkTopK *t = NULL;
    rax *names = NULL;
    if (io.error || len != buckets * CHK_NUM_TABLES * CHK_WIRE_BUCKET_SIZE) goto error;
    heap = loadString(&io, &len);
    if (io.error || len != k * CHK_WIRE_HEAP_SIZE) goto error;
    /* Verify serialized lengths before allocating according to untrusted dimensions. */
    t = chkTopKTryCreate(k, buckets, decay);
    if (!t) goto error;
    t->total = total;
    const unsigned char *p = tables;
    for (int i = 0; i < CHK_NUM_TABLES; i++) {
        for (int j = 0; j < t->numbuckets; j++) {
            chkBucket *bucket = &t->tables[i][j];
            for (int h = 0; h < CHK_HEAVY_ENTRIES_PER_BUCKET; h++) {
                bucket->heavy_entries[h].fp = getInteger(&p, 2);
                bucket->heavy_entries[h].count = getInteger(&p, 8);
                if (bucket->heavy_entries[h].count > total) goto error;
            }
            bucket->lobby_entry.fp = getInteger(&p, 2);
            bucket->lobby_entry.count = getInteger(&p, 1);
            if (bucket->lobby_entry.count > total) goto error;
        }
    }
    p = heap;
    names = raxNew();
    for (int i = 0; i < t->k; i++) {
        chkHeapBucket *entry = &t->heap[i];
        entry->fp = getInteger(&p, 8);
        entry->count = getInteger(&p, 8);
        unsigned present = getInteger(&p, 1);
        if (present > 1 || entry->count > total ||
            (present && !entry->count) || (!present && (entry->count || entry->fp)) ||
            (i && entry->count < t->heap[(i - 1) / 2].count)) goto error;
        if (!present) continue;
        char *item = loadString(&io, &len);
        if (io.error || len > INT_MAX ||
            XXH3_64bits_withSeed(item, len, CHK_HEAP_SEED) != entry->fp ||
            !raxTryInsert(names, (unsigned char *)item, len, NULL, NULL)) {
            zfree(item);
            goto error;
        }
        entry->item = sdsnewlen(item, len);
        t->alloc_size += sdsAllocSize(entry->item);
        zfree(item);
    }
    if (!loadOpcode(&io, RDB_MODULE_OPCODE_EOF)) goto error;
    raxFree(names); zfree(tables); zfree(heap);
    return createTopkChkObject(t);
error:
    if (names) raxFree(names);
    if (t) chkTopKRelease(t);
    zfree(tables); zfree(heap);
    return NULL;
}
