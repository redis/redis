/* Hash table implementation.
 *
 * This file implements in memory hash tables with insert/del/replace/find/
 * get-random-element operations. Hash tables will auto resize if needed
 * tables of power of two in size are used, collisions are handled by
 * chaining. See the source code for more information... :)
 *
 * Copyright (c) 2006-2010, Salvatore Sanfilippo <antirez at gmail dot com>
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
#include "alloc.h"
#include <stdlib.h>
#include <assert.h>
#include <limits.h>
#include "dict.h"

/* -------------------------- private prototypes ---------------------------- */

static int _dictExpandIfNeeded(hi_dict *ht);
static unsigned long _dictNextPower(unsigned long size);
static int _dictKeyIndex(hi_dict *ht, const void *key);
static int _dictInit(hi_dict *ht, hi_dictType *type, void *privDataPtr);

/* -------------------------- hash functions -------------------------------- */

/* Generic hash function (a popular one from Bernstein).
 * I tested a few and this was the best. */
static unsigned int hi_dictGenHashFunction(const unsigned char *buf, int len) {
    unsigned int hash = 5381;

    while (len--)
        hash = ((hash << 5) + hash) + (*buf++); /* hash * 33 + c */
    return hash;
}

/* ----------------------------- API implementation ------------------------- */

/* Reset an hashtable already initialized with ht_init().
 * NOTE: This function should only called by ht_destroy(). */
static void _dictReset(hi_dict *ht) {
    ht->table = NULL;
    ht->size = 0;
    ht->sizemask = 0;
    ht->used = 0;
}

/* Create a new hash table */
static hi_dict *hi_dictCreate(hi_dictType *type, void *privDataPtr) {
    hi_dict *ht = hi_malloc(sizeof(*ht));
    if (ht == NULL)
        return NULL;

    _dictInit(ht,type,privDataPtr);
    return ht;
}

/* Initialize the hash table */
static int _dictInit(hi_dict *ht, hi_dictType *type, void *privDataPtr) {
    _dictReset(ht);
    ht->type = type;
    ht->privdata = privDataPtr;
    return DICT_OK;
}

/* Expand or create the hashtable */
static int hi_dictExpand(hi_dict *ht, unsigned long size) {
    hi_dict n; /* the new hashtable */
    unsigned long realsize = _dictNextPower(size), i;

    /* the size is invalid if it is smaller than the number of
     * elements already inside the hashtable */
    if (ht->used > size)
        return DICT_ERR;

    _dictInit(&n, ht->type, ht->privdata);
    n.size = realsize;
    n.sizemask = realsize-1;
    n.table = hi_calloc(realsize,sizeof(hi_dictEntry*));
    if (n.table == NULL)
        return DICT_ERR;

    /* Copy all the elements from the old to the new table:
     * note that if the old hash table is empty ht->size is zero,
     * so hi_dictExpand just creates an hash table. */
    n.used = ht->used;
    for (i = 0; i < ht->size && ht->used > 0; i++) {
        hi_dictEntry *he, *nextHe;

        if (ht->table[i] == NULL) continue;

        /* For each hash entry on this slot... */
        he = ht->table[i];
        while(he) {
            unsigned int h;

            nextHe = he->next;
            /* Get the new element index */
            h = hi_dictHashKey(ht, he->key) & n.sizemask;
            he->next = n.table[h];
            n.table[h] = he;
            ht->used--;
            /* Pass to the next element */
            he = nextHe;
        }
    }
    assert(ht->used == 0);
    hi_free(ht->table);

    /* Remap the new hashtable in the old */
    *ht = n;
    return DICT_OK;
}

/* Add an element to the target hash table */
static int hi_dictAdd(hi_dict *ht, void *key, void *val) {
    int index;
    hi_dictEntry *entry;

    /* Get the index of the new element, or -1 if
     * the element already exists. */
    if ((index = _dictKeyIndex(ht, key)) == -1)
        return DICT_ERR;

    /* Allocates the memory and stores key */
    entry = hi_malloc(sizeof(*entry));
    if (entry == NULL)
        return DICT_ERR;

    entry->next = ht->table[index];
    ht->table[index] = entry;

    /* Set the hash entry fields. */
    hi_dictSetHashKey(ht, entry, key);
    hi_dictSetHashVal(ht, entry, val);
    ht->used++;
    return DICT_OK;
}

/* Add an element, discarding the old if the key already exists.
 * Return 1 if the key was added from scratch, 0 if there was already an
 * element with such key and hi_dictReplace() just performed a value update
 * operation. */
static int hi_dictReplace(hi_dict *ht, void *key, void *val) {
    hi_dictEntry *entry, auxentry;

    /* Try to add the element. If the key
     * does not exists hi_dictAdd will succeed. */
    if (hi_dictAdd(ht, key, val) == DICT_OK)
        return 1;
    /* It already exists, get the entry */
    entry = hi_dictFind(ht, key);
    if (entry == NULL)
        return 0;

    /* Free the old value and set the new one */
    /* Set the new value and free the old one. Note that it is important
     * to do that in this order, as the value may just be exactly the same
     * as the previous one. In this context, think to reference counting,
     * you want to increment (set), and then decrement (free), and not the
     * reverse. */
    auxentry = *entry;
    hi_dictSetHashVal(ht, entry, val);
    hi_dictFreeEntryVal(ht, &auxentry);
    return 0;
}

/* Search and remove an element */
static int hi_dictDelete(hi_dict *ht, const void *key) {
    unsigned int h;
    hi_dictEntry *de, *prevde;

    if (ht->size == 0)
        return DICT_ERR;
    h = hi_dictHashKey(ht, key) & ht->sizemask;
    de = ht->table[h];

    prevde = NULL;
    while(de) {
        if (hi_dictCompareHashKeys(ht,key,de->key)) {
            /* Unlink the element from the list */
            if (prevde)
                prevde->next = de->next;
            else
                ht->table[h] = de->next;

            hi_dictFreeEntryKey(ht,de);
            hi_dictFreeEntryVal(ht,de);
            hi_free(de);
            ht->used--;
            return DICT_OK;
        }
        prevde = de;
        de = de->next;
    }
    return DICT_ERR; /* not found */
}

/* Destroy an entire hash table */
static int _dictClear(hi_dict *ht) {
    unsigned long i;

    /* Free all the elements */
    for (i = 0; i < ht->size && ht->used > 0; i++) {
        hi_dictEntry *he, *nextHe;

        if ((he = ht->table[i]) == NULL) continue;
        while(he) {
            nextHe = he->next;
            hi_dictFreeEntryKey(ht, he);
            hi_dictFreeEntryVal(ht, he);
            hi_free(he);
            ht->used--;
            he = nextHe;
        }
    }
    /* Free the table and the allocated cache structure */
    hi_free(ht->table);
    /* Re-initialize the table */
    _dictReset(ht);
    return DICT_OK; /* never fails */
}

/* Clear & Release the hash table */
static void hi_dictRelease(hi_dict *ht) {
    _dictClear(ht);
    hi_free(ht);
}

static hi_dictEntry *hi_dictFind(hi_dict *ht, const void *key) {
    hi_dictEntry *he;
    unsigned int h;

    if (ht->size == 0) return NULL;
    h = hi_dictHashKey(ht, key) & ht->sizemask;
    he = ht->table[h];
    while(he) {
        if (hi_dictCompareHashKeys(ht, key, he->key))
            return he;
        he = he->next;
    }
    return NULL;
}

static hi_dictIterator *hi_dictGetIterator(hi_dict *ht) {
    hi_dictIterator *iter = hi_malloc(sizeof(*iter));
    if (iter == NULL)
        return NULL;

    iter->ht = ht;
    iter->index = -1;
    iter->entry = NULL;
    iter->nextEntry = NULL;
    return iter;
}

static hi_dictEntry *hi_dictNext(hi_dictIterator *iter) {
    while (1) {
        if (iter->entry == NULL) {
            iter->index++;
            if (iter->index >=
                    (signed)iter->ht->size) break;
            iter->entry = iter->ht->table[iter->index];
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

static void hi_dictReleaseIterator(hi_dictIterator *iter) {
    hi_free(iter);
}

/* ------------------------- private functions ------------------------------ */

/* Expand the hash table if needed */
static int _dictExpandIfNeeded(hi_dict *ht) {
    /* If the hash table is empty expand it to the initial size,
     * if the table is "full" double its size. */
    if (ht->size == 0)
        return hi_dictExpand(ht, DICT_HT_INITIAL_SIZE);
    if (ht->used == ht->size)
        return hi_dictExpand(ht, ht->size*2);
    return DICT_OK;
}

/* Our hash table capability is a power of two */
static unsigned long _dictNextPower(unsigned long size) {
    unsigned long i = DICT_HT_INITIAL_SIZE;

    if (size >= LONG_MAX) return LONG_MAX;
    while(1) {
        if (i >= size)
            return i;
        i *= 2;
    }
}

/* Returns the index of a free slot that can be populated with
 * an hash entry for the given 'key'.
 * If the key already exists, -1 is returned. */
static int _dictKeyIndex(hi_dict *ht, const void *key) {
    unsigned int h;
    hi_dictEntry *he;

    /* Expand the hashtable if needed */
    if (_dictExpandIfNeeded(ht) == DICT_ERR)
        return -1;
    /* Compute the key hash value */
    h = hi_dictHashKey(ht, key) & ht->sizemask;
    /* Search if this slot does not already contain the given key */
    he = ht->table[h];
    while(he) {
        if (hi_dictCompareHashKeys(ht, key, he->key))
            return -1;
        he = he->next;
    }
    return h;
}

