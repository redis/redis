/* Hash Tables Implementation.
 *
 * This file implements in memory dictionaries with insert/del/replace/find/
 * get-random-element operations. The dictionary is implemented as a Hash Array
 * Mapped Trie (HAMT). See the source code for more information... :)
 *
 * Copyright (c) 2006-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2021, Viktor Söderqvist <viktor.soderqvist@est.tech>
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
#include <math.h>

#include "dict.h"
#include "zmalloc.h"
#include "redisassert.h"

/* Our dict is implemented as a Hash Array Mapped Trie (HAMT). The hash (result
 * of a hash function) of each key is stored in a a tree structure where each
 * node has up to 32 children. With this large branching factor, the tree never
 * becomes very deep (6-7 levels). The time complexity is O(log32 n), which is
 * very close to constant.
 *
 * This is our HAMT structure:
 *
 * +--------------------+
 * | dict               |
 * |--------------------|   +--------------------+   +--------------------+
 * | bitmaps | children---->| bitmaps | children---->| key     | value    |
 * | size               |   | key     | value    |   | ...     | ...      |
 * | dictType           |   | key     | value    |   +--------------------+
 * | privdata           |   | key     | value    |
 * +--------------------+   | key     | value    |   +--------------------+
 *                          | bitmaps | children---->| key     | value    |
 *                          | key     | value    |   | bitmaps | children--->..
 *                          | ...     | ...      |   | value   | key      |
 *                          +--------------------+   | ...     | ...      |
 *                                                   +--------------------+
 *
 * In each level, 5 bits of the hash is used as an index into the children. A
 * bitmap indicates which of the children exist (1) and which don't (0). Whether
 * the child is a leaf or a subnode is indicated by a second bitmap, "leafmap".
 *
 * hash(key) = 0010 10010 01011 00101 01001 10110 11101
 * (64 bits)   10101 11001 01001 01101 10111 11010
 *                                           ^^^^^
 *                                             26
 *
 * bitmap    = 00100101 00000010 00010100 01100001
 * (level 0)        ^
 *                  bit 26 is 1, so there is a child node or leaf
 *
 * leafmap   = 00000100 00000010 00010000 01100000
 * (level 0)        ^
 *                  bit 26 is 1, so the child is a key-value entry
 *
 * The children array is compact and its length is equal to the number of 1s in
 * the bitmap. In the bitmap above, our bit is the 2nd 1-bit from the left, so
 * we look at the 2nd child.
 *
 * children  = +--------------------+
 *             | bitmaps | children |
 *             | key     | value    | <-- The 2nd child is a key-value entry. If
 *             | bitmaps | children |     the key matches our key, then it's our
 *             | ...     | ...      |     key. Otherwise, the dict doesn't
 *             +--------------------+     contain our key.
 *
 * If, instead, our child is a subnode, we use the next 5 bits from the hash to
 * index into the next level of children.
 *
 * The HAMT is described in Phil Bagwell (2000). Ideal Hash Trees.
 * https://infoscience.epfl.ch/record/64398/files/idealhashtrees.pdf
 *
 * Until 2021, the dictionary was implemented as a hash table with incremental
 * rehashing using two hash tables while rehashing and resized in powers of two.
 * Despite the incremental rehashing, allocating and freeing very large
 * continous memory is expensive. The memory usage was also higher than for the
 * HAMT (TO BE VERIFIED). */

/*##########################################################################\
|#/                                                                       \#|
|#|    DEBUGGING                                                          |#|
|#|                                                                       |#|

cd src
make OPTIMIZATION="-O0" REDIS_CFLAGS=-DREDIS_TEST -j5 noopt
make BUILD_TLS=1 OPTIMIZATION="-O0" REDIS_CFLAGS=-DREDIS_TEST \
     CFLAGS=-fsanitize=address \
     "LDFLAGS=-static-libasan -fsanitize=address" -j5 noopt
gdb --args ./redis-server test dict

|#|                                                                       |#|
|#\_______________________________________________________________________/#|
\##########################################################################*/

/* -------------------------- globals --------------------------------------- */

/* Using dictEnableResize() / dictDisableResize() we make possible to
 * enable/disable resizing of the hash table as needed. This is very important
 * for Redis, as we use copy-on-write and don't want to move too much memory
 * around when there is a child performing saving operations.
 *
 * Note that even when dict_can_resize is set to 0, not all resizes are
 * prevented: a hash table is still allowed to grow if the ratio between
 * the number of elements and the buckets > dict_force_resize_ratio. */
static int dict_can_resize = 1;
/* static unsigned int dict_force_resize_ratio = 5; */

/* -------------------------- private prototypes ---------------------------- */

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

uint64_t dictGenHashFunction(const void *key, int len) {
    return siphash(key,len,dict_hash_function_seed);
}

uint64_t dictGenCaseHashFunction(const unsigned char *buf, int len) {
    return siphash_nocase(buf,len,dict_hash_function_seed);
}

/* ---------------- Macros and functions for bit manipulation --------------- */

/* Using builtin popcount CPU instruction is ~4 times faster than the
 * bit-twiddling emulation below. The builtin requires GCC >= 3.4.0 or a recent
 * Clang. To utilize the CPU popcount instruction, compile with -msse4.2 or use
 * #pragma GCC target ("sse4.2"). */
#ifdef USE_BUILTIN_POPCOUNT
# define dict_popcount(x)  __builtin_popcount((unsigned int)(x))
#else
/* Population count (the number of 1s in a binary number), parallel summing. */
int dict_popcount(uint32_t x) {
    x -= ((x >> 1) & 0x55555555);
    x  = ((x >> 2) & 0x33333333) + (x & 0x33333333);
    x  = ((x >> 4) & 0x0f0f0f0f) + (x & 0x0f0f0f0f);
    x += (x >> 8);
    return (x + (x >> 16)) & 0x3f;
}
#endif

/* The 5 bits used at level N to index into the children. */
#define bitindex_at_level(h, lvl) ((int)((h) >> (5 * (lvl))) & 0x1f)

/* Check if bit 'bit' is set in 'bitmap' */
#define bit_is_set(bitmap, bit) (((bitmap) >> (bit)) & 1)

#define child_exists(node, bit) bit_is_set((node)->bitmap, bit)
#define child_is_entry(node, bit) bit_is_set((node)->leafmap, bit)

/* The index in the children array is the number of children before this one */
#define child_index(node, bit) dict_popcount((node)->bitmap >> (bit) >> 1)

/* ----------------------------- API implementation ------------------------- */

/* Create a new hash table */
dict *dictCreate(dictType *type,
        void *privDataPtr)
{
    dict *d = zmalloc(sizeof(*d));
    d->type = type;
    d->privdata = privDataPtr;
    d->size = 0;
    return d;
}

/* Performs N steps of incremental rehashing. Returns 1 if there are still
 * keys to move from the old to the new hash table, otherwise 0 is returned.
 */
int dictRehash(dict *d, int n) {
    DICT_NOTUSED(d);
    DICT_NOTUSED(n);
    return 0;
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
    DICT_NOTUSED(d);
    DICT_NOTUSED(ms);
    return 0;
}

/* Add an element to the target hash table */
int dictAdd(dict *d, void *key, void *val)
{
    dictEntry *entry = dictAddRaw(d,key,NULL);

    if (!entry) return DICT_ERR;
    dictSetVal(d, entry, val);
    return DICT_OK;
}

/* In-place converts an entry into a subnode. The old entry is copied and
 * becomes the only child of the subnode. When using this function, don't forget
 * to flag the node as a subnode in the parent's leafmap. */
static void dictWrapEntryInSubnode(dict *d, union dictNode *node, int level) {
    union dictNode *children = zmalloc(sizeof(union dictNode) * 1);
    children[0].entry = node->entry; /* copy entry */
    uint64_t hash = dictHashKey(d, node->entry.key);
    int childbit = bitindex_at_level(hash, level);
    /* Reinterpret the node as a subnode and add children. */
    node->sub.children = children;
    node->sub.bitmap = (1U << childbit);
    node->sub.leafmap = (1U << childbit);
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
    if (existing) *existing = NULL;
    if (d->size == 0) {
        dictSetKey(d, &d->root.entry, key);
        d->size = 1;
        return &d->root.entry;
    }
    if (d->size == 1) {
        /* Root is an entry. */
        if (dictCompareKeys(d, key, d->root.entry.key)) {
            if (existing) *existing = &d->root.entry;
            return NULL;
        }
        /* Convert the root to a subnode. */
        dictWrapEntryInSubnode(d, &d->root, 0);
    }

    uint64_t hash = dictHashKey(d, key);
    dictSubNode *node = &d->root.sub;
    int level = 0;
    for (;;) {
        /* Use 5 bits of the hash as an index into one of 32 possible
         * children. The child exists if the bit at childbit is set. */
        int childbit = bitindex_at_level(hash, level);
        int childindex = child_index(node, childbit);
        if (!child_exists(node, childbit)) {
            /* Let's make space for our entry here. */
            int numchildren = dict_popcount(node->bitmap);
            int numafter = numchildren - childindex;
            node->children = zrealloc(node->children,
                                      sizeof(union dictNode) * ++numchildren);
            memmove(&node->children[childindex + 1],
                    &node->children[childindex],
                    sizeof(union dictNode) * numafter);
            dictSetKey(d, &node->children[childindex].entry, key);
            node->bitmap |= (1U << childbit);
            node->leafmap |= (1U << childbit);
            d->size++;
            return &node->children[childindex].entry;
        }

        if (child_is_entry(node, childbit)) {
            dictEntry *entry = &node->children[childindex].entry;
            if (dictCompareKeys(d, key, entry->key)) {
                if (existing) *existing = entry;
                return NULL;
            }

            /* There's another entry at this position. We need to wrap it in
             * a subnode and then add our entry in there. */
            if ((level + 1) * 5 >= 64) {
                /* Max depth reached; no more hash bits available. TODO: Use
                 * secondary hash function or a duplicate list (a flat array on
                 * the deepest level). */
                assert(0);
            }

            /* Convert entry to subnode and clear its leaf flag. */
            dictWrapEntryInSubnode(d, &node->children[childindex], level+1);
            node->leafmap &= ~(1U << childbit);
        }

        /* The child is a subnode. Loop to add our new key inside it. */
        node = &node->children[childindex].sub;
        level++;
    }
}

/* Returns a pointer to an entry or NULL if the key isn't found in the dict. */
dictEntry *dictFind(dict *d, const void *key) {
    if (d->size == 0) return NULL; /* dict is empty */
    if (d->size == 1) {
        /* Root is an entry. */
        dictEntry *entry = &d->root.entry;
        if (dictCompareKeys(d, key, entry->key))
            return entry;
        else
            return NULL;
    }
    uint64_t hash = dictHashKey(d, key);
    dictSubNode *node = &d->root.sub;
    int level = 0;
    for (;;) {
        /* Use 5 bits of the hash as an index into one of 32 possible
           children. The child exists if the bit at childbit is set. */
        int childbit = bitindex_at_level(hash, level);
        if (!child_exists(node, childbit))
            return NULL;
        int childindex = child_index(node, childbit);
        if (child_is_entry(node, childbit)) {
            dictEntry *entry = &node->children[childindex].entry;
            if (dictCompareKeys(d, key, entry->key))
                return entry;
            else
                return NULL;
        } else {
            /* It's a subnode */
            node = &node->children[childindex].sub;
            level++;
        }
    }
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

/* Removes an entry from a subnode. Returns 1 if the key was found and 0
 * otherwise. The 'deleted' entry is filled with the key and value from the
 * deleted entry. This is a recursive helper for dictGenericDelete(). */
static int dictDeleteFromNode(dict *d, dictSubNode *node, int level,
                              uint64_t hash, const void *key,
                              dictEntry *deleted) {
    /* Use 5 bits of the hash as an index into one of 32 possible
       children. The child exists if the bit at childbit is set. */
    int childbit = bitindex_at_level(hash, level);
    if (!child_exists(node, childbit))
        return 0;

    int childindex = child_index(node, childbit);
    if (child_is_entry(node, childbit)) {
        dictEntry *entry = &node->children[childindex].entry;
        if (!dictCompareKeys(d, key, entry->key))
            return 0;

        /* It's a match. Copy to 'deleted' and remove child from node. */
        *deleted = *entry;

        int numchildren = dict_popcount(node->bitmap);
        numchildren--;
        int numafter = numchildren - childindex;
        memmove(&node->children[childindex],
                &node->children[childindex + 1],
                sizeof(union dictNode) * numafter);
        node->children = zrealloc(node->children,
                                  sizeof(union dictNode) * numchildren);
        node->bitmap  &= ~(1U << childbit);
        node->leafmap &= ~(1U << childbit);
        return 1;
    }

    /* It's a subnode. Recursively delete from the subnode. */
    if ((level + 1) * 5 >= 64) {
        /* All hash bits used up. TODO: Use secondary hash function. */
        assert(0);
    }
    dictSubNode *subnode = &node->children[childindex].sub;
    if (!dictDeleteFromNode(d, subnode, level + 1, hash, key, deleted))
        return 0; /* Not found in subtree. */

    /* If we're here, it means we have removed an entry from the subnode. If the
     * subnode has now only one child and it's an entry, we need to collapse the
     * subnode.
     *                                  ,--entry
     * Before delete:  node---subnode--<
     *                                  `--entry
     *
     * After removal:  node---subnode---entry
     *
     * After collapse: node---entry
     */
    if (dict_popcount(subnode->bitmap) == 1 &&
        subnode->leafmap == subnode->bitmap) {
        union dictNode *grandchildren = subnode->children;
        node->children[childindex] = grandchildren[0]; /* copy */
        node->leafmap |= (1U << childbit);
        zfree(grandchildren);
    }
    return 1;
}

/* Search and remove an element. Returns 1 if the key was found and 0 otherwise.
 * The 'deleted' entry is filled with the key and value from the deleted entry.
 *
 * This is an helper function for dictDelete() and dictUnlink(). Please check
 * the top comment of those functions. */
static int dictGenericDelete(dict *d, const void *key, dictEntry *deleted) {
    if (d->size == 0) return 0;
    if (d->size == 1) {
        /* Root is an entry. */
        if (dictCompareKeys(d, key, d->root.entry.key)) {
            *deleted = d->root.entry;
            d->size = 0;
            return 1;
        }
        return 0;
    }
    uint64_t hash = dictHashKey(d, key);
    int found = dictDeleteFromNode(d, &d->root.sub, 0, hash, key, deleted);
    if (found) {
        d->size--;
        if (d->size == 1) {
            /* Collapse into root. */
            assert(d->root.sub.bitmap == d->root.sub.leafmap);
            assert(dict_popcount(d->root.sub.bitmap) == 1);
            union dictNode *grandchildren = d->root.sub.children;
            d->root.entry = grandchildren[0].entry; /* copy */
            zfree(grandchildren);
        }
    }
    return found;
}

/* Remove an element, returning DICT_OK on success or DICT_ERR if the
 * element was not found. */
int dictDelete(dict *d, const void *key) {
    dictEntry entry;
    if (dictGenericDelete(d, key, &entry)) {
        dictFreeKey(d, &entry);
        dictFreeVal(d, &entry);
        return DICT_OK;
    }
    return DICT_ERR;
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
    dictEntry entry;
    if (dictGenericDelete(d, key, &entry)) {
        /* TODO: Rename dictGenericDelete() to dictUnlink() and refactor calls
           to it to get rid of the silly dup_entry. */
        dictEntry *dup_entry = zmalloc(sizeof(dictEntry));
        *dup_entry = entry;
        return dup_entry;
    }
    return NULL;
}

/* You need to call this function to really free the entry after a call
 * to dictUnlink(). It's safe to call this function with 'he' = NULL. */
void dictFreeUnlinkedEntry(dict *d, dictEntry *he) {
    if (he == NULL) return;
    dictFreeKey(d, he);
    dictFreeVal(d, he);
    zfree(he);
}

/* Deletes the contents of a subnode. Returns counter after incrementing it with
 * the number of deleted elements. Calls progress indicator callback sometimes
 * if provided. */
int dictClearNode(dict *d, dictSubNode *node, long counter,
                  void(callback)(void *)) {
    for (int childbit = 0; childbit < 32; childbit++) {
        if (child_exists(node, childbit)) {
            int childindex = child_index(node, childbit);
            if (child_is_entry(node, childbit)) {
                dictEntry *entry = &node->children[childindex].entry;
                dictFreeKey(d, entry);
                dictFreeVal(d, entry);
                counter++;
                if (callback && (counter & 65535) == 0)
                    callback(d->privdata);
            } else {
                dictSubNode *subnode = &node->children[childindex].sub;
                counter = dictClearNode(d, subnode, counter, callback);
            }
        }
    }
    zfree(node->children);
    return counter;
}

/* Clear & Release the hash table */
void dictRelease(dict *d)
{
    dictEmpty(d, NULL);
    zfree(d);
}

void *dictFetchValue(dict *d, const void *key) {
    dictEntry *he;

    he = dictFind(d,key);
    return he ? dictGetVal(he) : NULL;
}

dictIterator *dictGetIterator(dict *d)
{
    dictIterator *iter = zmalloc(sizeof(*iter));
    dictInitIterator(iter, d, 0);
    return iter;
}

dictIterator *dictGetSafeIterator(dict *d) {
    return dictGetIterator(d);
}

/* Forwards a cursor to the next index on the given level. The 5-bit group
 * corresponding to the given level is incremented and all sublevels indexes are
 * cleared. If the incremented 5-bit number overflows (i.e. if it reaches 32),
 * it wraps to 0 and the outer level's 5-bit group is incremented and overflow
 * on this level is handle the same way. If level 0 overflows, 0 is returned
 * (meaning that the root level has been fully scanned and there is no more to
 * scan).
 *
 * If the 5-bit groups were be stored in order of significance, this would be a
 * simple increment, but they are stored in reverse order, i.e. the most
 * significant 5-bit group is the least significant 5 bits in the cursor. */
static inline uint64_t incrCursorAtLevel(uint64_t cursor, int level) {
    do {
        int bits = (cursor >> (level * 5)) & 0x1f;
        /* Clear this level and all more significant bits. */
        cursor &= (1ULL << ((level) * 5)) - 1;
        if (++bits < 32) {
            assert((bits & ~0x1f) == 0);
            cursor |= ((uint64_t)bits << (level * 5)); /* set bits this level */
            break;
        }
        /* loop to increment the level above */
    } while(level-- > 0);
    return cursor;
}

/* Recursive helper for dictNext(). */
dictEntry *dictNextInNode(dictIterator *iter, dictSubNode *node, int level) {
    union dictNode *children = node->children;
    int childbit = bitindex_at_level(iter->cursor, level);
    while (1) {
        if (child_exists(node, childbit)) {
            int childindex = child_index(node, childbit);
            if (child_is_entry(node, childbit)) {
                /* Set start position for next time. */
                iter->cursor = incrCursorAtLevel(iter->cursor, level);
                return &children[childindex].entry;
            } else {
                /* Find next recurively. */
                assert(level < 13); /* FIXME */
                dictSubNode *subnode = &children[childindex].sub;
                dictEntry *found = dictNextInNode(iter, subnode, level + 1);
                if (found) return found;
            }
        }
        /* No more entries within child. Clear this and all sublevel indices.
         * (At level 0 the mask is 0, at level 1 it is 0x1f, and so on.) */
        iter->cursor &= (1ULL << (5 * level)) - 1;

        /* Skip to beginning of next child. */
        if (++childbit == 32) break;
        iter->cursor |= ((uint64_t)childbit << (5*level)); /* set bits */
    }
    return NULL;
}

/* Returns a pointer to the next entry. It's safe to add, delete and replace
 * elements in the dict while iterating. However, the entry pointer returned by
 * this function becomes invalid when adding or deleting any entries. */
dictEntry *dictNext(dictIterator *iter) {
    if (iter->d == NULL) return NULL;
    dictEntry *entry = NULL;
    switch (iter->d->size) {
    case 0:
        break;
    case 1:
        if (iter->cursor++ == 0)
            entry = &iter->d->root.entry;
        break;
    default:
        entry = dictNextInNode(iter, &iter->d->root.sub, 0);
    }
    if (entry == NULL) {
        iter->d = NULL; /* Prevents it from restarting from 0. */
        iter->cursor = 0;
    }
    return entry;
}

void dictInitIterator(dictIterator *iter, dict *d, uint64_t cursor) {
    iter->d = d;
    iter->cursor = cursor;
}

void dictReleaseIterator(dictIterator *iter)
{
    zfree(iter);
}

/* Return a random entry from the hash table. Useful to
 * implement randomized algorithms */
dictEntry *dictGetRandomKey(dict *d)
{
    if (dictSize(d) == 0) return NULL;
    dictIterator iter;
    uint64_t start = randomULong();
    dictInitIterator(&iter, d, start);
    dictEntry *entry = dictNext(&iter);
    if (entry == NULL) {
        /* wrap and start from the beginning */
        dictInitIterator(&iter, d, 0);
        entry = dictNext(&iter);
        assert(entry != NULL);
    }
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
    if (dictSize(d) < count) count = dictSize(d);
    dictIterator iter;
    uint64_t start = randomULong();
    dictInitIterator(&iter, d, start);
    for (unsigned int i = 0; i < count; i++) {
        dictEntry *entry = dictNext(&iter);
        if (entry == NULL) {
            /* wrap and start from the beginning */
            dictInitIterator(&iter, d, 0);
            entry = dictNext(&iter);
            assert(entry != NULL);
        }
        des[i] = entry;
    }
    return count;
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
 * The cursor is basically a path into the hash tree. The keys are iterated in
 * the order of how they are stored in the hash tree, in depth first order. The
 * 5 least significant bits of the cursor are used as an index into the first
 * level of nodes. The next 5 bits are used an index into the next level and so
 * forth.
 *
 * LIMITATIONS
 *
 * This iterator is completely stateless, and this is a huge advantage,
 * including no additional memory used.
 *
 * It also has the following nice properties:
 *
 * 1) No duplicates. Each element is returned only once.
 *
 * 2) The iterator can usually return exactly the requested number of entries.
 *    The exception is when there are multiple keys with exactly the same 64-bit
 *    hash value. These are always returned together, since they correspond to
 *    the same cursor value.
 *
 * The disadvantages resulting from this design is:
 *
 * 1) The cursor is somewhat hard to understand at first, but this comment is
 *    supposed to help.
 */
unsigned long dictScan(dict *d,
                       unsigned long v,
                       dictScanFunction *fn,
                       dictScanBucketFunction* bucketfn,
                       void *privdata)
{
    DICT_NOTUSED(bucketfn);
    dictIterator iter;
    dictInitIterator(&iter, d, v);
    unsigned long u = v;
    do {
        const dictEntry *de = dictNext(&iter);
        if (de == NULL)
            return 0;
        fn(privdata, de);
        u = iter.cursor;
    } while (u == v);
    return u;
}

void dictEmpty(dict *d, void(callback)(void*)) {
    if (d->size == 1) {
        /* The root is an entry. */
        dictFreeKey(d, &d->root.entry);
        dictFreeVal(d, &d->root.entry);
    } else if (d->size > 1) {
        dictClearNode(d, &d->root.sub, 0, callback);
    }
    d->size = 0;
}

void dictEnableResize(void) {
    dict_can_resize = 1;
}

void dictDisableResize(void) {
    dict_can_resize = 0;
}

int dictExpand(dict *d, unsigned long size) {
    DICT_NOTUSED(d);
    DICT_NOTUSED(size);
    return DICT_OK;
}
int dictTryExpand(dict *d, unsigned long size) {
    DICT_NOTUSED(d);
    DICT_NOTUSED(size);
    return DICT_OK;
}
int dictResize(dict *d) {
    DICT_NOTUSED(d);
    return DICT_OK;
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
    DICT_NOTUSED(oldptr);
    DICT_NOTUSED(hash);
    if (dictSize(d) == 0) return NULL; /* dict is empty */
    printf("FIXME dictFindEntryRefByPtrAndHash\n");
    assert(0);
    return NULL;
}

/* Returns the estimated memory usage of the dict structure in bytes. This does
 * NOT include additional memory allocated for keys and values.
 *
 * With large sizes, the root node will be close to full. So will the nodes
 * close to the root. As a rough estimate we assume that half of the nodes, the
 * ones far away from the root, are only half-full, thus roughly 1.5 * log32 N.
 *
 * (The estimate 1.28 * N given in Phil Bagwell (2000). Ideal Hash Trees,
 * Section 3.5 "Space Used" refers to a HAMT with resizable root table.)
 */
size_t dictEstimateMem(dict *d) {
    /* Rough estimate: N + 1.5 * log32(N) */
    /* Log base conversion rule: log32(x) = log2(x) / log2(32) */
    unsigned long numsubnodes = 1.5 * log2(d->size) / log2(32);
    unsigned long numentries = d->size;
    unsigned long numnodes = numentries + numsubnodes;
    return sizeof(dict) + sizeof(union dictNode) * numnodes;
}

/* ------------------------------- Debugging ---------------------------------*/

/* Ideas for stats: Store the total number of subnodes in the dict. Then the
 * total allocation size can be computed using dict size. */
void dictGetStats(char *buf, size_t bufsize, dict *d) {
    DICT_NOTUSED(d);
    snprintf(buf, bufsize, "Stats for HAMT not implemented");
    /* Make sure there is a NULL term at the end. */
    if (bufsize) buf[bufsize-1] = '\0';
}

/* Helper for dictDump(). */
void dictDumpSub(dictSubNode *node, int level) {
    for (int childbit = 0; childbit < 32; childbit++) {
        if (!child_exists(node, childbit)) continue;
        int childindex = child_index(node, childbit);
        if (child_is_entry(node, childbit)) {
            dictEntry *entry = &node->children[childindex].entry;
            printf("%*s%02d \"%s\"\n", level*3, "", childbit, (char*)entry->key);
        } else {
            printf("%*s%02d\n", level*3, "", childbit);
            dictDumpSub(&node->children[childindex].sub, level + 1);
        }
    }
}

/* Prints a dict as a tree with node indices. Keys must be strings. */
void dictDump(dict *d) {
    if (d->size == 0)
        printf("00 (empty)\n");
    else if (d->size == 1)
        printf("00 \"%s\"\n", (char*)d->root.entry.key);
    else
        dictDumpSub(&d->root.sub, 0);
}

/* Prints a cursor as a path of 5-bit indices into the tree. */
void dictDumpCursor(uint64_t c) {
    printf("cursor:");
    while (c) {
        printf("/%d", (int)(c & 0x1f));
        c = c >> 5;
    }
}

/* Prints all keys on a single line using an iterator. Keys must be strings. */
void dictPrintAll(dict *d) {
    dictIterator iter;
    dictInitIterator(&iter, d, 0);
    printf("Dict keys:");
    for (;;) {
        dictEntry *e = dictNext(&iter);
        if (e == NULL) break;
        printf(" %s", (char*)e->key);
    }
    printf("\n");
}

/* ------------------------------- Benchmark ---------------------------------*/

#ifdef REDIS_TEST

uint64_t hashCallback(const void *key) {
    return dictGenHashFunction((unsigned char*)key, strlen((char*)key));
}

int compareCallback(void *privdata, const void *key1, const void *key2) {
    int l1,l2;
    DICT_NOTUSED(privdata);

    l1 = strlen((char*)key1);
    l2 = strlen((char*)key2);
    if (l1 != l2) return 0;
    return memcmp(key1, key2, l1) == 0;
}

void freeCallback(void *privdata, void *val) {
    DICT_NOTUSED(privdata);

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

dictType BenchmarkDictType = {
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

/* ./redis-server test dict [<count> | --accurate] */
int dictTest(int argc, char **argv, int accurate) {
    long j;
    long long start, elapsed;
    dict *dict = dictCreate(&BenchmarkDictType,NULL);
    long count = 0;

    if (argc == 4) {
        if (accurate) {
            count = 5000000;
        } else {
            count = strtol(argv[3],NULL,10);
        }
    } else {
        count = 5000;
    }

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
#endif
