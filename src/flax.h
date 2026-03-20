#ifndef FLAX_H
#define FLAX_H

#include <stdint.h>
#include <stddef.h>

#define FLAX_MIN_CAPACITY 12

typedef struct flax {
    void *data;
    int64_t numele;
    int64_t capacity;
} flax;

typedef struct flaxIterator {
    flax *f;
    int64_t key;
    void *data;
    int64_t idx;
} flaxIterator;

/* Exported API. */
flax *flaxNew(void);
int flaxInsert(flax *f, int64_t key, void *data, void **old);
int flaxTryInsert(flax *f, int64_t key, void *data, void **old);
int flaxRemove(flax *f, int64_t key, void **old);
int flaxFind(flax *f, int64_t key, void **value);
void flaxFree(flax *f);
void flaxFreeWithCallback(flax *f, void (*free_callback)(void *));
void flaxFreeWithCbAndContext(flax *f,
                              void (*free_callback)(void *item, void *ctx),
                              void *ctx);
void flaxStart(flaxIterator *it, flax *f);
int flaxSeek(flaxIterator *it, const char *op, int64_t key);
int flaxNext(flaxIterator *it);
int flaxPrev(flaxIterator *it);
void flaxStop(flaxIterator *it);
int flaxEOF(flaxIterator *it);
uint64_t flaxSize(flax *f);

#ifdef REDIS_TEST
int flaxTest(int argc, char *argv[], int flags);
#endif

#endif
