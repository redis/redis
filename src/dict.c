/* Hash Tables Implementation.
 *
 * This file implements in memory hash tables with insert/del/replace/find/
 * get-random-element operations. Hash tables will auto resize if needed
 * tables of power of two in size are used, collisions are handled by
 * chaining.
 *
 * Hash resize/rehash algorithm is memory-optimized by avoiding from allocating
 * a new hash table and starting to move entries gradually to new HT.
 * Instead, it extends the hash table to the desired size by calling realloc()
 * and start iterating over the table and rehash each entry to its updated
 * bucket, in the same hash table. In case of big rehash, this approach relaxes
 * the risk to keys eviction since it avoids from holding two hash tables until
 * rehashing ends, which might take some time.
 *
 * Copyright (c) 2006-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "fmacros.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <sys/time.h>

#include "dict.h"
#include "zmalloc.h"
#include "redisassert.h"

/* Using dictEnableResize() / dictDisableResize() we make possible to
 * enable/disable resizing of the hash table as needed. This is very important
 * for Redis, as we use copy-on-write and don't want to move too much memory
 * around when there is a child performing saving operations.
 *
 * Note that even when dict_can_resize is set to 0, not all resizes are
 * prevented: a hash table is still allowed to grow if the ratio between
 * the number of elements and the buckets > dict_force_resize_ratio. */
static int dict_can_resize = 1;
static unsigned int dict_force_resize_ratio = 5;

/* -------------------------- private prototypes ---------------------------- */

static int _dictExpandIfNeeded(dict *d);
static signed char _dictNextExp(unsigned long size);
static long _dictKeyIndex(dict *d, const void *key, uint64_t hash, dictEntry **existing);
static int _dictInit(dict *d, dictType *type);

static int _dictResize(dict *d, unsigned long size, int *malloc_failed);
/* -------------------------- hash functions -------------------------------- */

static uint8_t dict_hash_function_seed[16];

void dictSetHashFunctionSeed(uint8_t *seed) {
    memcpy(dict_hash_function_seed,seed,sizeof(dict_hash_function_seed));
}

uint8_t *dictGetHashFunctionSeed(void) {
    return dict_hash_function_seed;
}

/* The default hashing function uses SipHash implementation
 * in siphash.c. */

uint64_t siphash(const uint8_t *in, const size_t inlen, const uint8_t *k);
uint64_t siphash_nocase(const uint8_t *in, const size_t inlen, const uint8_t *k);

uint64_t dictGenHashFunction(const void *key, size_t len) {
    return siphash(key,len,dict_hash_function_seed);
}

uint64_t dictGenCaseHashFunction(const unsigned char *buf, size_t len) {
    return siphash_nocase(buf,len,dict_hash_function_seed);
}

/* ----------------------------- API implementation ------------------------- */

/* Reset hash table parameters already initialized with _dictInit()*/
static void _dictReset(dict *d) {
    d->ht_table = NULL;
    d->ht_size_exp = -1;
    d->ht_size_exp_prev = -1;
    d->ht_used = 0;
}

/* Create a new hash table */
dict *dictCreate(dictType *type) {
    dict *d = zmalloc(sizeof(*d));

    _dictInit(d, type);
    return d;
}

/* Initialize the hash table */
int _dictInit(dict *d, dictType *type)
{
    _dictReset(d);
    d->type = type;
    d->rehashidx = -1;
    d->pauserehash = 0;
    return DICT_OK;
}

/* Resize the table to the minimal size that contains all the elements,
 * but with the invariant of a USED/BUCKETS ratio near to <= 1 */
int dictResize(dict *d) {
    unsigned long minimal;

    if (!dict_can_resize || dictIsRehashing(d)) return DICT_ERR;
    minimal = d->ht_used;
    if (minimal < DICT_HT_INITIAL_SIZE)
        minimal = DICT_HT_INITIAL_SIZE;
    return _dictResize(d, minimal, NULL);
}

/* Resize or create the hash table. When malloc_failed is non-NULL, it'll avoid
 * panic if malloc fails (in which case it'll be set to 1).
 * Returns DICT_OK if resize was performed, and DICT_ERR if skipped. */
int _dictResize(dict *d, unsigned long size, int *malloc_failed) {
    if (malloc_failed) *malloc_failed = 0;

    /* the size is invalid if it is smaller than the number of
     * elements already inside the hash table */
    if (dictIsRehashing(d) || d->ht_used > size)
        return DICT_ERR;

    /* the new reallocated hash table */
    dictEntry **new_ht_table;
    signed char new_ht_size_exp = _dictNextExp(size);

    /* Detect overflows */
    size_t newsize = 1ul << new_ht_size_exp;
    if (newsize < size || newsize * sizeof(dictEntry *) < newsize)
        return DICT_ERR;

    /* Rehashing to the same table size is not useful. */
    if (new_ht_size_exp == d->ht_size_exp) return DICT_ERR;

    /* If hash table grows - then realloc() bigger memory. */
    /* If first init - then this condition still TRUE. realloc() memory */
    if (d->ht_size_exp < new_ht_size_exp) {
        /* Realloc the hash table (without initializing new buckets to NULL) */
        if (malloc_failed) {
            new_ht_table = ztryrealloc(d->ht_table,
                                       newsize * sizeof(dictEntry *));
            *malloc_failed = new_ht_table == NULL;
            if (*malloc_failed)
                return DICT_ERR;
        } else
            new_ht_table = zrealloc(d->ht_table, newsize * sizeof(dictEntry *));
    } else {
        /* If table shrinks - Only after moving all entries to lower part of the
         * hash table it will be possible to realloc() to a smaller buffer */
        new_ht_table = d->ht_table;
    }

    /* Is this the first initialization? If so it's not really a rehashing
     * we just set the first hash table so that it can accept keys. */
    if (d->ht_table == NULL) {
        d->ht_size_exp = new_ht_size_exp;
        d->ht_used = 0;
        d->ht_table = new_ht_table;
        memset(d->ht_table, 0, DICTHT_SIZE(d->ht_size_exp)*sizeof(dictEntry *));
        return DICT_OK;
    }

    /* Prepare the table for incremental rehashing */
    d->ht_size_exp_prev = d->ht_size_exp;
    d->ht_size_exp = new_ht_size_exp;
    d->ht_table = new_ht_table;
    d->rehashidx = 0;
    return DICT_OK;
}

/* return DICT_ERR if expand was not performed */
int dictExpand(dict *d, unsigned long size) {
    return _dictResize(d, size, NULL);
}

/* return DICT_ERR if expand failed due to memory allocation failure */
int dictTryExpand(dict *d, unsigned long size) {
    int malloc_failed;
    _dictResize(d, size, &malloc_failed);
    return malloc_failed ? DICT_ERR : DICT_OK;
}

/* Performs limited steps of incremental rehashing. Returns 1 if there are still
 * keys to rehash (based on the new size of the HT). Otherwise, 0 is returned.
 *
 * Note that a rehashing step consists in updating a bucket (that may have more
 * than one key as we use chaining). However, since part of the hash table may be
 * composed of empty buckets, it is not guaranteed that this function will rehash
 * even a single bucket, since it will visit at max bucket_visits*10 empty
 * buckets in total, otherwise the amount of work it does would be unbound and
 * the function may block for a long time. */
int dictRehash(dict *d, int bucket_visits) {
    /* Max number of empty buckets to visit. */
    int empty_visits = bucket_visits * 10;

    if (!dictIsRehashing(d)) return 0;

    /* Iterate and rehash all buckets up to the previous size of the hash table,
     * and rehash all entries in buckets to their updated buckets */
    long ht_size_prev = DICTHT_SIZE(d->ht_size_exp_prev);

    while (bucket_visits > 0) {
        dictEntry *de, *nextde;
        dictEntry *this_bucket = NULL;

        /* If HT grows, need to init all new buckets gradually along with the
         * rehash iteration (We avoid from using memset on realloc()) */
        if (d->ht_size_exp > d->ht_size_exp_prev) {
            unsigned long mask = DICTHT_SIZE_MASK(d->ht_size_exp - d->ht_size_exp_prev);
            for (unsigned long i = 1; i <= mask; ++i)
                d->ht_table[(i << d->ht_size_exp_prev) + d->rehashidx] = NULL;
        }

        if (d->ht_table[d->rehashidx] == NULL) {
            if (--empty_visits == 0) return 1;
        } else {
            --bucket_visits;
            de = d->ht_table[d->rehashidx];
            /* rehash all the keys in this bucket */
            while (de) {
                long h;

                nextde = de->next;
                /* Get the new index based on the new resized hash table.
                 * Note that if HT shrinks, this operation will take care to
                 * rehash to corresponding bucket (by dropping MSB bit(s)) */
                h = dictHashKey(d, de->key) & DICTHT_SIZE_MASK(d->ht_size_exp);

                /* if key mapped to current bucket (at rehashidx), then aggregate
                 * these entries aside and update the bucket at the end */
                if (d->rehashidx == h) {
                    de->next = this_bucket;
                    this_bucket = de;
                } else {
                    de->next = d->ht_table[h];
                    d->ht_table[h] = de;
                }
                de = nextde;
            }
            d->ht_table[d->rehashidx] = this_bucket;
        }

        d->rehashidx += 1;

        if (d->rehashidx == ht_size_prev) {
            /* Mark rehash as done */
            d->rehashidx = -1;
            if (d->ht_size_exp < d->ht_size_exp_prev) {
                /* Unlike expanding HT that call realloc() before starting
                 * dictRehash(), when shrinking, only after done rehashing all
                 * entries to lower part of the table, it is safe to call
                 * realloc() to shrink the size of the hash table */
                d->ht_table = zrealloc(d->ht_table,
                                       DICTHT_SIZE(d->ht_size_exp) *
                                            sizeof(dictEntry *));
            }
            return 0;
        }
    }

    /* More to rehash... */
    return 1;
}

long long timeInMilliseconds(void) {
    struct timeval tv;

    gettimeofday(&tv,NULL);
    return (((long long)tv.tv_sec)*1000)+(tv.tv_usec/1000);
}

/* Rehash in ms+"delta" milliseconds. The value of "delta" is larger 
 * than 0, and is smaller than 1 in most cases. The exact upper bound 
 * depends on the running time of dictRehash(d,100).*/
int dictRehashMilliseconds(dict *d, int ms) {
    if (d->pauserehash > 0) return 0;

    long long start = timeInMilliseconds();
    int rehashes = 0;

    while(dictRehash(d,100)) {
        rehashes += 100;
        if (timeInMilliseconds()-start > ms) break;
    }
    return rehashes;
}

/* This function performs just a step of rehashing, and only if hashing has
 * not been paused for our hash table. When we have iterators in the
 * middle of a rehashing we can't mess with the two hash tables otherwise
 * some element can be missed or duplicated.
 *
 * This function is called by common lookup or update operations in the
 * dictionary so that the hash table automatically migrates from H1 to H2
 * while it is actively used. */
static void _dictRehashStep(dict *d, int bucket_visits) {
    if (d->pauserehash == 0) dictRehash(d,bucket_visits);
}

/* Add an element to the target hash table */
int dictAdd(dict *d, void *key, void *val)
{
    dictEntry *entry = dictAddRaw(d,key,NULL);

    if (!entry) return DICT_ERR;
    dictSetVal(d, entry, val);
    return DICT_OK;
}

/* Low level add or find:
 * This function adds the entry but instead of setting a value returns the
 * dictEntry structure to the user, that will make sure to fill the value
 * field as they wish.
 *
 * This function is also directly exposed to the user API to be called
 * mainly in order to store non-pointers inside the hash value, example:
 *
 * entry = dictAddRaw(dict,mykey,NULL);
 * if (entry != NULL) dictSetSignedIntegerVal(entry,1000);
 *
 * Return values:
 *
 * If key already exists NULL is returned, and "*existing" is populated
 * with the existing entry if existing is not NULL.
 *
 * If key was added, the hash entry is returned to be manipulated by the caller.
 */
dictEntry *dictAddRaw(dict *d, void *key, dictEntry **existing) {
    long bucketidx;
    dictEntry *entry;

    if (dictIsRehashing(d)) _dictRehashStep(d,1);

    /* Get the bucketidx of the new element, or -1 if
     * the element already exists. */
    if ((bucketidx = _dictKeyIndex(d, key, dictHashKey(d, key), existing)) == -1)
        return NULL;

    /* Allocate the memory and store the new entry.
     * Insert the element in top, with the assumption that in a database
     * system it is more likely that recently added entries are accessed
     * more frequently. */
    size_t metasize = dictMetadataSize(d);
    entry = zmalloc(sizeof(*entry) + metasize);
    if (metasize > 0) {
        memset(dictMetadata(entry), 0, metasize);
    }
    entry->next = d->ht_table[bucketidx];
    d->ht_table[bucketidx] = entry;
    d->ht_used++;

    /* Set the hash entry fields. */
    dictSetKey(d, entry, key);
    return entry;
}

/* Add or Overwrite:
 * Add an element, discarding the old value if the key already exists.
 * Return 1 if the key was added from scratch, 0 if there was already an
 * element with such key and dictReplace() just performed a value update
 * operation. */
int dictReplace(dict *d, void *key, void *val)
{
    dictEntry *entry, *existing, auxentry;

    /* Try to add the element. If the key
     * does not exists dictAdd will succeed. */
    entry = dictAddRaw(d,key,&existing);
    if (entry) {
        dictSetVal(d, entry, val);
        return 1;
    }

    /* Set the new value and free the old one. Note that it is important
     * to do that in this order, as the value may just be exactly the same
     * as the previous one. In this context, think to reference counting,
     * you want to increment (set), and then decrement (free), and not the
     * reverse. */
    auxentry = *existing;
    dictSetVal(d, existing, val);
    dictFreeVal(d, &auxentry);
    return 0;
}

/* Add or Find:
 * dictAddOrFind() is simply a version of dictAddRaw() that always
 * returns the hash entry of the specified key, even if the key already
 * exists and can't be added (in that case the entry of the already
 * existing key is returned.)
 *
 * See dictAddRaw() for more information. */
dictEntry *dictAddOrFind(dict *d, void *key) {
    dictEntry *entry, *existing;
    entry = dictAddRaw(d,key,&existing);
    return entry ? entry : existing;
}

long dictGetIndex(dict *d, uint64_t keyHash) {
    long bucketidx;

    if (!dictIsRehashing(d)) {
        bucketidx = keyHash & DICTHT_SIZE_MASK(d->ht_size_exp);
    } else {
        /* Calculate index based on old HT */
        bucketidx = keyHash & DICTHT_SIZE_MASK(d->ht_size_exp_prev);
        /* If rehashidx is bigger than bucketidx, then corresponding entries
         * were already rehashed based on the new size of the HT */
        if (bucketidx < d->rehashidx)
        {
            bucketidx = keyHash & DICTHT_SIZE_MASK(d->ht_size_exp);
        }
    }

    return bucketidx;
}

/* Search and remove an element. This is a helper function for
 * dictDelete() and dictUnlink(), please check the top comment
 * of those functions. */
static dictEntry *dictGenericDelete(dict *d, const void *key, int nofree) {
    dictEntry *he, *prevHe;

    /* dict is empty */
    if (dictSize(d) == 0) return NULL;

    if (dictIsRehashing(d)) _dictRehashStep(d,1);

    uint64_t idx = dictGetIndex(d, dictHashKey(d, key));

    he = d->ht_table[idx];
    prevHe = NULL;
    while (he) {
        if (key == he->key || dictCompareKeys(d, key, he->key)) {
            /* Unlink the element from the list */
            if (prevHe)
                prevHe->next = he->next;
            else
                d->ht_table[idx] = he->next;
            if (!nofree) {
                dictFreeUnlinkedEntry(d, he);
            }
            d->ht_used--;
            return he;
        }
        prevHe = he;
        he = he->next;
    }
    return NULL; /* not found */
}

/* Remove an element, returning DICT_OK on success or DICT_ERR if the
 * element was not found. */
int dictDelete(dict *ht, const void *key) {
    return dictGenericDelete(ht,key,0) ? DICT_OK : DICT_ERR;
}

/* Remove an element from the table, but without actually releasing
 * the key, value and dictionary entry. The dictionary entry is returned
 * if the element was found (and unlinked from the table), and the user
 * should later call `dictFreeUnlinkedEntry()` with it in order to release it.
 * Otherwise if the key is not found, NULL is returned.
 *
 * This function is useful when we want to remove something from the hash
 * table but want to use its value before actually deleting the entry.
 * Without this function the pattern would require two lookups:
 *
 *  entry = dictFind(...);
 *  // Do something with entry
 *  dictDelete(dictionary,entry);
 *
 * Thanks to this function it is possible to avoid this, and use
 * instead:
 *
 * entry = dictUnlink(dictionary,entry);
 * // Do something with entry
 * dictFreeUnlinkedEntry(entry); // <- This does not need to lookup again.
 */
dictEntry *dictUnlink(dict *d, const void *key) {
    return dictGenericDelete(d,key,1);
}

/* You need to call this function to really free the entry after a call
 * to dictUnlink(). It's safe to call this function with 'he' = NULL. */
void dictFreeUnlinkedEntry(dict *d, dictEntry *he) {
    if (he == NULL) return;
    dictFreeKey(d, he);
    dictFreeVal(d, he);
    zfree(he);
}

/* Destroy an entire dictionary */
int _dictClear(dict *d, void(callback)(dict *)) {
    unsigned long released = 0;

    /* Free all the dictionary entries first */
    dictEntry *de, *deNext;
    dictIterator *di = dictGetSafeIterator(d);
    while ((de = dictNext(di)) != NULL) {
        deNext = de->next;
        dictFreeKey(d, de);
        dictFreeVal(d, de);
        zfree(de);
        d->ht_used--;
        de = deNext;
        if (callback && ((++released) & 65535) == 0) callback(d);
    }
    dictReleaseIterator(di);

    /* Free the table and the allocated cache structure */
    zfree(d->ht_table);
    /* Re-initialize the table */
    _dictReset(d);
    return DICT_OK; /* never fails */
}

/* Clear & Release the hash table */
void dictRelease(dict *d) {
    _dictClear(d, NULL);
    zfree(d);
}

dictEntry *dictFind(dict *d, const void *key) {
    dictEntry *he;

    if (dictSize(d) == 0) return NULL; /* dict is empty */
    if (dictIsRehashing(d)) _dictRehashStep(d,1);

    uint64_t idx = dictGetIndex(d, dictHashKey(d, key));

    he = d->ht_table[idx];
    while (he) {
        if (key == he->key || dictCompareKeys(d, key, he->key))
            return he;
        he = he->next;
    }
    return NULL;
}

void *dictFetchValue(dict *d, const void *key) {
    dictEntry *he;

    he = dictFind(d,key);
    return he ? dictGetVal(he) : NULL;
}

/* A fingerprint is a 64 bit number that represents the state of the dictionary
 * at a given time, it's just a few dict properties xored together.
 * When an unsafe iterator is initialized, we get the dict fingerprint, and check
 * the fingerprint again when the iterator is released.
 * If the two fingerprints are different it means that the user of the iterator
 * performed forbidden operations against the dictionary while iterating. */
unsigned long long dictFingerprint(dict *d) {
    unsigned long long integers[3], hash = 0;
    int j;

    integers[0] = (long) d->ht_table;
    integers[1] = d->ht_size_exp;
    integers[2] = d->ht_used;

    /* We hash N integers by summing every successive integer with the integer
     * hashing of the previous sum. Basically:
     *
     * Result = hash(hash(hash(int1)+int2)+int3) ...
     *
     * This way the same set of integers in a different order will (likely) hash
     * to a different number. */
    for (j = 0; j < 3; j++) {
        hash += integers[j];
        /* For the hashing step we use Tomas Wang's 64 bit integer hash. */
        hash = (~hash) + (hash << 21); // hash = (hash << 21) - hash - 1;
        hash = hash ^ (hash >> 24);
        hash = (hash + (hash << 3)) + (hash << 8); // hash * 265
        hash = hash ^ (hash >> 14);
        hash = (hash + (hash << 2)) + (hash << 4); // hash * 21
        hash = hash ^ (hash >> 28);
        hash = hash + (hash << 31);
    }
    return hash;
}

dictIterator *dictGetIterator(dict *d) {
    dictIterator *iter = zmalloc(sizeof(*iter));

    iter->d = d;
    iter->index = -1;
    iter->safe = 0;
    iter->entry = NULL;
    iter->nextEntry = NULL;
    return iter;
}

dictIterator *dictGetSafeIterator(dict *d) {
    dictIterator *i = dictGetIterator(d);

    i->safe = 1;
    return i;
}

dictEntry *dictNext(dictIterator *iter) {
    dict *d = iter->d;
    while (1) {
        if (iter->entry == NULL) {
            if (iter->index == -1) {
                if (iter->safe)
                    dictPauseRehashing(d);
                else
                    iter->fingerprint = dictFingerprint(d);
            }

            if (!dictIsRehashing(d)) {
                /* If no rehashing is taking place, then simply iterate all over
                 * the hash table */
                iter->index++;
                if (iter->index >= (long) DICTHT_SIZE(d->ht_size_exp)) break;

            } else {
                /* Check if idx was rehashed already: (idx<d->rehashidx) */
                long idx_ht_prev = iter->index & DICTHT_SIZE_MASK(d->ht_size_exp_prev);
                if ((idx_ht_prev < d->rehashidx) && (d->ht_size_exp > d->ht_size_exp_prev)) {
                    /* If a given bucket index, say 101 (=5), was already rehashed
                     * and hash table expanded from size of 8 to 32 then entries
                     * from bucket index 101 were already spread to one or more
                     * buckets indexes of type [XX]101 which is {00101, 01101,
                     * 10101 and 11101}. In that case requires iterating extended
                     * group of buckets of type [XX]101 before advancing the
                     * index to [00]110 (=6).
                     *
                     * The following 3 lines of code do exactly that:
                     * 1. Adds 1 to extended (higher) bits. Following the example
                     *    above, adding 1<<3 to [00]101 brings to [01]101.
                     * 2. If the new higher bits got overflow, behind the new
                     *    HT size, then the overflow bit will be added to index
                     *    value as +1. In the example above, after visiting
                     *    [11]101, adding 1<<3 will cause index overflow,
                     *    behind new index size: 1[00]101. Then adding the
                     *    overflow-bit as +1 to the index will bring the index
                     *    to be equal 1[00]110.
                     * 3. If overflow bit raised, then take care to remove it
                     *    from the index. In the example above the index will
                     *    change from 1[00]110 to [00]110, as expected. */
                    iter->index += (1 << d->ht_size_exp_prev); /*1*/
                    iter->index += iter->index >> d->ht_size_exp; /*2*/
                    iter->index &= DICTHT_SIZE_MASK(d->ht_size_exp); /*3*/
                } else {
                    /* Left to iterate all the items that didn't rehashed yet
                     * Until the old size of the HT. */
                    iter->index++;
                    /* if reached end of old size of the hash table, then break */
                    if ((unsigned long)iter->index >= DICTHT_SIZE(d->ht_size_exp_prev)) break;
                }
            }

            iter->entry = d->ht_table[iter->index];
        } else {
            iter->entry = iter->nextEntry;
        }
        if (iter->entry) {
            /* We need to save the 'next' here, the iterator user
             * may delete the entry we are returning. */
            iter->nextEntry = iter->entry->next;
            return iter->entry;
        }
    }
    return NULL;
}

void dictReleaseIterator(dictIterator *iter) {
    if (!(iter->index == -1)) {
        if (iter->safe)
            dictResumeRehashing(iter->d);
        else
            assert(iter->fingerprint == dictFingerprint(iter->d));
    }
    zfree(iter);
}

/* Return a random entry from the hash table. Useful to
 * implement randomized algorithms */
dictEntry *dictGetRandomKey(dict *d) {
    unsigned long bucketidx;
    int listlen, listele;

    if (dictSize(d) == 0) return NULL;
    if (dictIsRehashing(d))  _dictRehashStep(d,1);

    do {
        bucketidx = dictGetIndex(d, randomULong() );
    } while (d->ht_table[bucketidx] == NULL);

    /* Now we found a non empty bucket, but it is a linked
     * list and we need to get a random element from the list.
     * The only sane way to do so is counting the elements and
     * select a random index. */
    listlen = 0;
    dictEntry *entry = d->ht_table[bucketidx];
    dictEntry *first_entry = entry;
    while (entry) {
        entry = entry->next;
        listlen++;
    }
    listele = random() % listlen;
    entry = first_entry;
    while (listele--) entry = entry->next;
    return entry;
}

/* This function samples the dictionary to return a few keys from random
 * locations.
 *
 * It does not guarantee to return all the keys specified in 'count', nor
 * it does guarantee to return non-duplicated elements, however it will make
 * some effort to do both things.
 *
 * Returned pointers to hash table entries are stored into 'des' that
 * points to an array of dictEntry pointers. The array must have room for
 * at least 'count' elements, that is the argument we pass to the function
 * to tell how many random elements we need.
 *
 * The function returns the number of items stored into 'des', that may
 * be less than 'count' if the hash table has less than 'count' elements
 * inside, or if not enough elements were found in a reasonable amount of
 * steps.
 *
 * Note that this function is not suitable when you need a good distribution
 * of the returned items, but only when you need to "sample" a given number
 * of continuous elements to run some kind of algorithm or to produce
 * statistics. However the function is much faster than dictGetRandomKey()
 * at producing N elements. */
unsigned int dictGetSomeKeys(dict *d, dictEntry **des, unsigned int count) {
    unsigned long randomval, maxsteps, stored = 0;
    unsigned long emptylen = 0; /* Continuous empty entries so far. */

    if (dictSize(d) < count) count = dictSize(d);
    maxsteps = count*10;

    /* Try to do a rehashing work proportional to 'count'. */
    if (dictIsRehashing(d)) _dictRehashStep(d, count);

    randomval = randomULong();
    while (stored < count && maxsteps--) {
        dictEntry *entry = d->ht_table[ dictGetIndex(d, randomval) ];

        /* Count contiguous empty buckets, and jump to other
         * locations if they reach 'count' (with a minimum of 5). */
        if (entry == NULL) {
            emptylen++;
            if (emptylen >= 5 && emptylen > count) {
                randomval = randomULong();
                emptylen = 0;
            }
        } else {
            emptylen = 0;
            while (entry) {
                /* Collect all the elements of the buckets found non
                 * empty while iterating. */
                *des = entry;
                des++;
                entry = entry->next;
                stored++;
                if (stored == count) return stored;
            }
        }
        randomval += 1;
    }
    return stored;
}

/* This is like dictGetRandomKey() from the POV of the API, but will do more
 * work to ensure a better distribution of the returned element.
 *
 * This function improves the distribution because the dictGetRandomKey()
 * problem is that it selects a random bucket, then it selects a random
 * element from the chain in the bucket. However elements being in different
 * chain lengths will have different probabilities of being reported. With
 * this function instead what we do is to consider a "linear" range of the table
 * that may be constituted of N buckets with chains of different lengths
 * appearing one after the other. Then we report a random element in the range.
 * In this way we smooth away the problem of different chain lengths. */
#define GETFAIR_NUM_ENTRIES 15
dictEntry *dictGetFairRandomKey(dict *d) {
    dictEntry *entries[GETFAIR_NUM_ENTRIES];
    unsigned int count = dictGetSomeKeys(d,entries,GETFAIR_NUM_ENTRIES);
    /* Note that dictGetSomeKeys() may return zero elements in an unlucky
     * run() even if there are actually elements inside the hash table. So
     * when we get zero, we call the true dictGetRandomKey() that will always
     * yield the element if the hash table has at least one. */
    if (count == 0) return dictGetRandomKey(d);
    unsigned int idx = rand() % count;
    return entries[idx];
}

/* Function to reverse bits. Algorithm from:
 * http://graphics.stanford.edu/~seander/bithacks.html#ReverseParallel */
static unsigned long rev(unsigned long v) {
    unsigned long s = CHAR_BIT * sizeof(v); // bit size; must be power of 2
    unsigned long mask = ~0UL;
    while ((s >>= 1) > 0) {
        mask ^= (mask << s);
        v = ((v >> s) & mask) | ((v << s) & ~mask);
    }
    return v;
}

/**/
static void dictScanReportEntries(dict *d,
                                  unsigned long bucketidx,
                                  dictScanFunction *fn,
                                  dictScanBucketFunction *bucketfn,
                                  void *privdata)
{
    /* Emit entries at cursor */
    if (bucketfn) bucketfn(d, &d->ht_table[bucketidx]);
    const dictEntry *next, *de = d->ht_table[bucketidx];

    while (de) {
        next = de->next;
        fn(privdata, de);
        de = next;
    }
}

/* dictScan() is used to iterate over the elements of a dictionary.
 *
 * Iterating works the following way:
 *
 * 1) Initially you call the function using a cursor (v) value of 0.
 * 2) The function performs one step of the iteration, and returns the
 *    new cursor value you must use in the next call.
 * 3) When the returned cursor is 0, the iteration is complete.
 *
 * The function guarantees all elements present in the
 * dictionary get returned between the start and end of the iteration.
 * However it is possible some elements get returned multiple times.
 *
 * For every element returned, the callback argument 'fn' is
 * called with 'privdata' as first argument and the dictionary entry
 * 'de' as second argument.
 *
 * HOW IT WORKS.
 *
 * The iteration algorithm was designed by Pieter Noordhuis.
 * The main idea is to increment a cursor starting from the higher order
 * bits. That is, instead of incrementing the cursor normally, the bits
 * of the cursor are reversed, then the cursor is incremented, and finally
 * the bits are reversed again.
 *
 * This strategy is needed because the hash table may be resized between
 * iteration calls.
 *
 * dict.c hash tables are always power of two in size, and they
 * use chaining, so the position of an element in a given table is given
 * by computing the bitwise AND between Hash(key) and SIZE-1
 * (where SIZE-1 is always the mask that is equivalent to taking the rest
 *  of the division between the Hash of the key and SIZE).
 *
 * For example if the current hash table size is 16, the mask is
 * (in binary) 1111. The position of a key in the hash table will always be
 * the last four bits of the hash output, and so forth.
 *
 * WHAT HAPPENS IF THE TABLE CHANGES IN SIZE?
 *
 * If the hash table grows, elements can go anywhere in one multiple of
 * the old bucket: for example let's say we already iterated with
 * a 4 bit cursor 1100 (the mask is 1111 because hash table size = 16).
 *
 * If the hash table will be resized to 64 elements, then the new mask will
 * be 111111. The new buckets you obtain by substituting in ??1100
 * with either 0 or 1 can be targeted only by keys we already visited
 * when scanning the bucket 1100 in the smaller hash table.
 *
 * By iterating the higher bits first, because of the inverted counter, the
 * cursor does not need to restart if the table size gets bigger. It will
 * continue iterating using cursors without '1100' at the end, and also
 * without any other combination of the final 4 bits already explored.
 *
 * Similarly when the table size shrinks over time, for example going from
 * 16 to 8, if a combination of the lower three bits (the mask for size 8
 * is 111) were already completely explored, it would not be visited again
 * because we are sure we tried, for example, both 0111 and 1111 (all the
 * variations of the higher bit) so we don't need to test it again.
 *
 * WAIT... YOU HAVE *TWO* TABLES DURING REHASHING!
 *
 * Yes, this is true, but we always iterate the smaller table first, then
 * we test all the expansions of the current cursor into the larger
 * table. For example if the current cursor is 101 and we also have a
 * larger table of size 16, we also test (0)101 and (1)101 inside the larger
 * table. This reduces the problem back to having only one table, where
 * the larger one, if it exists, is just an expansion of the smaller one.
 *
 * LIMITATIONS
 *
 * This iterator is completely stateless, and this is a huge advantage,
 * including no additional memory used.
 *
 * The disadvantages resulting from this design are:
 *
 * 1) It is possible we return elements more than once. However this is usually
 *    easy to deal with in the application level.
 * 2) The iterator must return multiple elements per call, as it needs to always
 *    return all the keys chained in a given bucket, and all the expansions, so
 *    we are sure we don't miss keys moving during rehashing.
 * 3) The reverse cursor is somewhat hard to understand at first, but this
 *    comment is supposed to help.
 */
unsigned long dictScan(dict *d,
                       unsigned long cursor,
                       dictScanFunction *fn,
                       dictScanBucketFunction *bucketfn,
                       void *privdata) {
    signed char size_exp;
    long bucketidx, newidx;

    if (dictSize(d) == 0) return 0;

    /* This is needed in case the scan callback tries to do dictFind or alike. */
    dictPauseRehashing(d);

    /* If rehashing is in progress, then iterate based on the size of
     * the old HT. */
    size_exp = (dictIsRehashing(d)) ? d->ht_size_exp_prev : d->ht_size_exp ;

    bucketidx = cursor & DICTHT_SIZE_MASK(size_exp);

    /* If during rehash and visiting a bucket that its entries were rehashed
     * already to one or more buckets, then we need to follow corresponding
     * bucket(s) */
    if ((dictIsRehashing(d)) && (bucketidx < d->rehashidx))
    {
        if (d->ht_size_exp_prev > d->ht_size_exp) {
            /* HT shrinks. Resolve corresponding bucket index */
            newidx = bucketidx & DICTHT_SIZE_MASK(d->ht_size_exp);
            dictScanReportEntries(d, newidx, fn, bucketfn, privdata);
        } else {

            /* Iterate over indices in extended table that are the expansion
             * of bucketidx. For example, expanded table from size 8 to 32 and
             * bucketidx equals 101 (=5), then entries from bucket index 101 were
             * already rehashed to one or more buckets indexes of type [XX]101
             * which are 00101, 01101, 10101 and 11101. In that case requires
             * iterating extended group of buckets of type [XX]101 before
             * advancing the cursor */
            long mask = DICTHT_SIZE_MASK(d->ht_size_exp - d->ht_size_exp_prev);
            for (long i = 0 ; i <= mask ; ++i )
            {
                newidx = (i << d->ht_size_exp_prev) + bucketidx;
                dictScanReportEntries(d, newidx, fn, bucketfn, privdata);
            }
        }
    } else {
        /* No rehashing is taking place. Or rehashing but visiting bucketidx
         * that not rehashed yet. Report bucketidx entry without translation. */
        dictScanReportEntries(d, bucketidx, fn, bucketfn, privdata);
    }

    /* Set unmasked bits so incrementing the reversed cursor
     * operates on the masked bits */
    cursor |= ~DICTHT_SIZE_MASK(size_exp);

    /* Increment the reverse cursor */
    cursor = rev(cursor);
    cursor++;
    cursor = rev(cursor);

    dictResumeRehashing(d);

    return cursor;
}

/* ------------------------- private functions ------------------------------ */

/* Because we may need to allocate huge memory chunk at once when dict
 * expands, we will check this allocation is allowed or not if the dict
 * type has expandAllowed member function. */
static int dictTypeExpandAllowed(dict *d) {
    if (d->type->expandAllowed == NULL) return 1;
    return d->type->expandAllowed(
            DICTHT_SIZE(_dictNextExp(d->ht_used + 1)) * sizeof(dictEntry *),
            (double) d->ht_used / DICTHT_SIZE(d->ht_size_exp));
}

/* Expand the hash table if needed */
static int _dictExpandIfNeeded(dict *d) {
    /* Incremental rehashing already in progress. Return. */
    if (dictIsRehashing(d)) return DICT_OK;

    /* If the hash table is empty expand it to the initial size. */
    if (DICTHT_SIZE(d->ht_size_exp) == 0) return dictExpand(d, DICT_HT_INITIAL_SIZE);

    /* If we reached the 1:1 ratio, and we are allowed to resize the hash
     * table (global setting) or we should avoid it but the ratio between
     * elements/buckets is over the "safe" threshold, we resize doubling
     * the number of buckets. */
    if (d->ht_used >= DICTHT_SIZE(d->ht_size_exp) &&
        (dict_can_resize ||
         d->ht_used / DICTHT_SIZE(d->ht_size_exp) > dict_force_resize_ratio) &&
        dictTypeExpandAllowed(d)) {
        return dictExpand(d, d->ht_used + 1);
    }
    return DICT_OK;
}

/* TODO: clz optimization */
/* Our hash table capability is a power of two */
static signed char _dictNextExp(unsigned long size)
{
    unsigned char e = DICT_HT_INITIAL_EXP;

    if (size >= LONG_MAX) return (8*sizeof(long)-1);
    while(1) {
        if (((unsigned long)1<<e) >= size)
            return e;
        e++;
    }
}

/* Returns the bucketidx of a free slot that can be populated with a hash entry
 * for the given 'key'. If the key already exists, -1 is returned and the
 * optional output parameter may be filled. */
static long _dictKeyIndex(dict *d, const void *key, uint64_t hash, dictEntry **existing) {
    unsigned long bucketidx;

    if (existing) *existing = NULL;

    /* Expand the hash table if needed */
    if (_dictExpandIfNeeded(d) == DICT_ERR)
        return -1;

    bucketidx = dictGetIndex (d, hash);

    /* Search if this slot does not already contain the given key */
    dictEntry *entry = d->ht_table[bucketidx];
    while (entry) {
        if (key == entry->key || dictCompareKeys(d, key, entry->key)) {
            if (existing) *existing = entry;
            return -1;
        }
        entry = entry->next;
    }
    return bucketidx;
}

void dictEmpty(dict *d, void(callback)(dict *)) {
    _dictClear(d, callback);
    d->rehashidx = -1;
    d->pauserehash = 0;
}

void dictEnableResize(void) {
    dict_can_resize = 1;
}

void dictDisableResize(void) {
    dict_can_resize = 0;
}

uint64_t dictGetHash(dict *d, const void *key) {
    return dictHashKey(d, key);
}

/* Finds the dictEntry reference by using pointer and pre-calculated hash.
 * oldkey is a dead pointer and should not be accessed.
 * the hash value should be provided using dictGetHash.
 * no string / key comparison is performed.
 * return value is the reference to the dictEntry if found, or NULL if not found. */
dictEntry **dictFindEntryRefByPtrAndHash(dict *d, const void *oldptr, uint64_t hash) {
    dictEntry *he, **heref;
    unsigned long bucketidx;

    if (dictSize(d) == 0) return NULL; /* dict is empty */

    bucketidx = dictGetIndex(d, hash);

    heref = &d->ht_table[bucketidx];
    he = *heref;
    while (he) {
        if (oldptr == he->key)
            return heref;
        heref = &he->next;
        he = *heref;
    }

    return NULL;
}

/* ------------------------------- Debugging ---------------------------------*/

#define DICT_STATS_VECTLEN 50
size_t _dictGetStatsHt(char *buf, size_t bufsize, dict *d) {
    unsigned long i, slots = 0, chainlen, maxchainlen = 0;
    unsigned long totchainlen = 0;
    unsigned long clvector[DICT_STATS_VECTLEN];
    unsigned long  ht_size = DICTHT_SIZE(d->ht_size_exp);
    unsigned long expanded_bits_exp = 0, expanded_bits_mask = 0;
    dictEntry *he;
    size_t len = 0;

    if (d->ht_used == 0) {
        return snprintf(buf, bufsize,
                        "No stats available for empty dictionaries\n");
    }

    /* Init stats array. */
    for (i = 0; i < DICT_STATS_VECTLEN; i++) clvector[i] = 0;

    if (dictIsRehashing(d)) {
        /* In case of rehash, iteration will be made up to old size of the HT and
         * if visited bucket was already rehashed, then follow expanded buckets */
        ht_size = DICTHT_SIZE(d->ht_size_exp_prev);

        /* the values expanded_bits_exp and expanded_bits_mask will assist to
         * have common loop below, either for rehash or not, expanding or
         * shrinking */
        if (d->ht_size_exp > d->ht_size_exp_prev) {
            expanded_bits_mask = DICTHT_SIZE_MASK(
                    d->ht_size_exp - d->ht_size_exp_prev);
            expanded_bits_exp = d->ht_size_exp_prev;

            /* count in statistics uninitialized buckets (new buckets in expanded
             * table that not initialized\visited yet by rehashing algorithm) */
            long items_left_to_rehash = d->ht_size_exp_prev - d->rehashidx;
            clvector[0] += items_left_to_rehash *
                           DICTHT_SIZE_MASK(d->ht_size_exp - d->ht_size_exp_prev);
        }
    }

    for (unsigned long idx = 0; idx < ht_size ; idx++) {
        unsigned long mask = 0;

        /* Given visited bucket index (idx), if state of the hash table is:
         * - No rehash
         * - Or, in rehash but the new size of the HT is smaller
         * - Or, in rehash but visiting index that not rehashed yet
         *
         * Then will make a trivial single visit to bucket index (idx). The
         * value of mask will be 1 and in-turn the for-loop will do single
         * iteration.
         *
         * But, If we are in state that:
         * - Rehash is in progress
         * - And, the new size of the table is bigger
         * - And, visiting index that was already rehashed
         * Then mask will reflect the additional higher bitmask that need to
         * append to the index, in order to reach expanded corresponding
         * buckets. */
        if ((long) idx < d->rehashidx) mask = expanded_bits_mask;

        for (unsigned long j = 0; j <= mask; ++j) {
            unsigned long newidx = (j << expanded_bits_exp) + idx;

            if (d->ht_table[newidx] == NULL) {
                clvector[0]++;
                continue;
            }
            slots++;
            /* For each hash entry on this slot... */
            chainlen = 0;
            he = d->ht_table[newidx];
            while (he) {
                chainlen++;
                he = he->next;
            }
            clvector[(chainlen < DICT_STATS_VECTLEN) ? chainlen : (
                    DICT_STATS_VECTLEN - 1)]++;
            if (chainlen > maxchainlen) maxchainlen = chainlen;
            totchainlen += chainlen;
        }
    }

    float rehash_percent = dictIsRehashing(d) ?
                           (float)(d->rehashidx*100)/DICTHT_SIZE(d->ht_size_exp_prev) :
                           100;

    /* Generate human readable stats. */
    len += snprintf(buf + len, bufsize - len,
                  "Hash table stats:\n"
                  " table size: %lu\n"
                  " number of elements: %lu\n"
                  " different slots: %lu\n"
                  " Rehashing progress (N/A=100%%): %.02f%%\n"
                  " max chain length: %lu\n"
                  " avg chain length (counted): %.02f\n"
                  " avg chain length (computed): %.02f\n"
                  " Chain length distribution:\n",
                    DICTHT_SIZE(d->ht_size_exp), d->ht_used, slots,
                    rehash_percent, maxchainlen, (float) totchainlen / slots,
                    (float) d->ht_used / slots);

    for (long i = 0; i < DICT_STATS_VECTLEN - 1; i++) {
        if (clvector[i] == 0) continue;
        if (len >= bufsize) break;
        len += snprintf(buf + len, bufsize - len,
                        "   %ld: %ld (%.02f%%)\n",
                        i, clvector[i], ((float) clvector[i] / DICTHT_SIZE(d->ht_size_exp)) * 100);
    }

    /* Unlike snprintf(), return the number of characters actually written. */
    if (bufsize) buf[bufsize - 1] = '\0';
    return strlen(buf);
}

void dictGetStats(char *buf, size_t bufsize, dict *d) {
    size_t l;
    char *orig_buf = buf;
    size_t orig_bufsize = bufsize;

    l = _dictGetStatsHt(buf, bufsize, d);
    buf += l;
    bufsize -= l;

    /* Make sure there is a NULL term at the end. */
    if (orig_bufsize) orig_buf[orig_bufsize - 1] = '\0';
}

/* ------------------------------- Benchmark ---------------------------------*/

#ifdef REDIS_TEST
#include "testhelp.h"

#define UNUSED(V) ((void) V)

uint64_t hashCallback(const void *key) {
    return dictGenHashFunction((unsigned char*)key, strlen((char*)key));
}

int compareCallback(dict *d, const void *key1, const void *key2) {
    int l1,l2;
    UNUSED(d);

    l1 = strlen((char*)key1);
    l2 = strlen((char*)key2);
    if (l1 != l2) return 0;
    return memcmp(key1, key2, l1) == 0;
}

void freeCallback(dict *d, void *val) {
    UNUSED(d);

    zfree(val);
}

char *stringFromLongLong(long long value) {
    char buf[32];
    int len;
    char *s;

    len = sprintf(buf,"%lld",value);
    s = zmalloc(len+1);
    memcpy(s, buf, len);
    s[len] = '\0';
    return s;
}

dictType TestDictType = {
    hashCallback,
    NULL,
    NULL,
    compareCallback,
    freeCallback,
    NULL,
    NULL
};

#define start_benchmark() start = timeInMilliseconds()
#define end_benchmark(msg) do { \
    elapsed = timeInMilliseconds()-start; \
    printf(msg ": %ld items in %lld ms\n", count, elapsed); \
} while(0)

int dictTestBenchmark(long count) {
    long j;
    long long start, elapsed;
    dict *dict = dictCreate(&TestDictType);

    start_benchmark();
    for (j = 0; j < count; j++) {
        int retval = dictAdd(dict,stringFromLongLong(j),(void*)j);
        assert(retval == DICT_OK);
    }
    end_benchmark("Inserting");
    assert((long)dictSize(dict) == count);

    /* Wait for rehashing. */
    while (dictIsRehashing(dict)) {
        dictRehashMilliseconds(dict,100);
    }

    start_benchmark();
    for (j = 0; j < count; j++) {
        char *key = stringFromLongLong(j);
        dictEntry *de = dictFind(dict,key);
        assert(de != NULL);
        zfree(key);
    }
    end_benchmark("Linear access of existing elements");

    start_benchmark();
    for (j = 0; j < count; j++) {
        char *key = stringFromLongLong(j);
        dictEntry *de = dictFind(dict,key);
        assert(de != NULL);
        zfree(key);
    }
    end_benchmark("Linear access of existing elements (2nd round)");

    start_benchmark();
    for (j = 0; j < count; j++) {
        char *key = stringFromLongLong(rand() % count);
        dictEntry *de = dictFind(dict,key);
        assert(de != NULL);
        zfree(key);
    }
    end_benchmark("Random access of existing elements");

    start_benchmark();
    for (j = 0; j < count; j++) {
        dictEntry *de = dictGetRandomKey(dict);
        assert(de != NULL);
    }
    end_benchmark("Accessing random keys");

    start_benchmark();
    for (j = 0; j < count; j++) {
        char *key = stringFromLongLong(rand() % count);
        key[0] = 'X';
        dictEntry *de = dictFind(dict,key);
        assert(de == NULL);
        zfree(key);
    }
    end_benchmark("Accessing missing");

    start_benchmark();
    for (j = 0; j < count; j++) {
        char *key = stringFromLongLong(j);
        int retval = dictDelete(dict,key);
        assert(retval == DICT_OK);
        key[0] += 17; /* Change first number to letter. */
        retval = dictAdd(dict,key,(void*)j);
        assert(retval == DICT_OK);
    }
    end_benchmark("Removing and adding");
    dictRelease(dict);
    return 0;
}

int dictIterateTest(long count) {
    UNUSED(count);
    printf("Iterating dict in the middle of rehash\n");
    {
        long j;

            long items[] = {1, 2, 3, 4, 5, 6, 7, 8, 9};

        for (long i = 0; i < (long)(sizeof(items) / sizeof(items[0])); ++i) {
            long n = items[i];

            dict *dict = dictCreate(&TestDictType);
            long counters[n];
            memset(counters, 0, sizeof(counters));

            /* Fillup elements */
            for (j = 0; j < n; j++) {
                dictAdd(dict, stringFromLongLong(j), (void *) j);
            }

            /* Iterate entire HT in the middle of rehashing */
            dictIterator *di;
            dictEntry *de;

            di = dictGetIterator(dict);
            while ((de = dictNext(di)) != NULL) {

                long value = (long) dictGetVal(de);
                ++counters[value];
            }
            dictReleaseIterator(di);
            for (j = 0; j < n; j++) {
                assert((long) counters[j] == ((j < n) ? 1 : 0));
            }
            dictRelease(dict);
        }
        return 0;
    }
}

/* ./redis-server test dict [<count> | --accurate] */
int dictTest(int argc, char **argv, int flags) {
    long count = 0;
    int res, accurate = (flags & REDIS_TEST_ACCURATE);

    if (argc == 4) {
        if (accurate) {
            count = 5000000;
        } else {
            count = strtol(argv[3], NULL, 10);
        }
    } else {
        count = 5000;
    }
    if ((res = dictTestBenchmark(count))) {return res;}
    if ((res = dictIterateTest(count))) {return res;}
    return 0;
}
#endif
