/* Trie data structure implementation
 *
 * A Trie is a fast, memory efficient ordered tree data structure used to store
 * associative arrays.
 *
 * Its lookup, insert, delete and replace complexity is O(k), where k is the
 * length of the key. Unlike hash tables, there are no collisions meaning its
 * worst case complexity is still O(k) and no re-hashing is required.
 *
 * No node in the tree stores the key associated with that node.
 * Instead, its position in the tree defines the key with which it's associated.
 * This results in memory savings as common key prefixes are stored only once.
 *
 * For instance, the layout of a Trie containig the words "hello" and "hey"
 * is the following:
 *
 *               [\0]
 *              /
 *            [h]
 *             |
 *            [e]
 *           /   \
 *         [l]   [y *]
 *          |
 *         [l]
 *          |
 *         [o *]
 *
 * (*) denotes a final node containing a value
 *
 * This implementation uses double-chained trees, in which all children of a
 * node are placed in a linked list. Each node has a pointer to the next node
 * as well as to the first child, resulting in a small overhead.
 *
 * Copyright (c) 2012, Andrea Luzzardi <aluzzardi at gmail dot com>
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

#ifndef __TRIE_H
#define __TRIE_H

#define TRIE_OK 0
#define TRIE_ERR 1

typedef struct trieNode {
    unsigned char key;
    void *val;
    struct trieNode *next;
    struct trieNode *children;
} trieNode;

typedef struct trie {
    size_t size;
    size_t used;
    trieNode *root;
} trie;

/* Callback used when traversing a Trie. */
typedef int (*trieWalkCallback)(trieNode *node, const unsigned char *key, size_t len, void *data);

/* Callback used to destroy a Trie value. */
typedef void (*trieValDestructor)(void *);

/* While traversing a Trie, we need to keep a buffer that holds the full
 * key. The buffer will be incrementally re-allocated at TRIE_WALK_BUFFERSTEP
 * size each time. */
#define TRIE_WALK_BUFFERSTEP 128

/* ------------------------------- Macros ------------------------------------*/
#define trieGetKey(node) ((node)->key)
#define trieGetVal(node) ((node)->val)

#define trieSetKey(node, key) do { \
    node->key = (key); \
} while(0)

#define trieSetVal(node, _val_) do { \
    node->val = (_val_); \
} while(0)

#define trieFreeVal(node, destructor) do { \
    if (destructor != NULL && node->val != NULL) { \
        destructor(node->val); \
    } \
    node->val = NULL; \
} while(0)

#define trieReplaceVal(node, _val_, destructor) do { \
    void *tmp = node->val; \
    trieSetVal(node, _val_); \
    if (destructor != NULL && tmp != NULL) { \
        destructor(tmp); \
    } \
} while(0)

#define trieNodeIsFinal(node) \
    (node->val != NULL)

#define trieSize(t) ((t)->used)
#define trieAllocatedSize(t) ((t)->size)

/* API */
trie *trieCreate();
int trieAdd(trie *t, const unsigned char *key, size_t len, void *val);
int trieReplace(trie *t, const unsigned char *key, size_t len, void *val, trieValDestructor destructor);
int trieAddOrReplace(trie *t, const unsigned char *key, size_t len, void *val, trieValDestructor destructor);
trieNode *trieFind(trie *d, const unsigned char *key, size_t len);
int trieDelete(trie *d, const unsigned char  *key, size_t len, trieValDestructor destructor);
int trieWalk(trie *t, trieWalkCallback callback, void *data);
int trieWalkFromPrefix(trie *t, trieWalkCallback callback, void *data, const unsigned char *prefix, size_t len);
void trieRelease(trie *t, trieValDestructor destructor);

#endif /* !__TRIE_H */
