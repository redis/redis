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

#include <stdlib.h>
#include <string.h>

#include "trie.h"
#include "zmalloc.h"

static trieNode *_trieCreateNode(trie *t, const unsigned char key)
{
    trieNode *node;

    node = zmalloc(sizeof(*node));
    trieSetKey(node, key);
    trieSetVal(node, NULL);
    node->children = NULL;
    node->next = NULL;
    t->size++;
    return node;
}

static void _trieDestroyNode(trie *t, trieNode *node)
{
    zfree(node);
    t->size--;
}

/* Create and initialize an empty Trie */
trie *trieCreate()
{
    trie *t = zmalloc(sizeof(*t));
    t->size = 0;
    t->used = 0;
    t->root = _trieCreateNode(t, '\0');
    return t;
}

/* Search the node's children for the given key */
static trieNode *_trieGetNextState(trieNode *node, const unsigned char key)
{
    trieNode *children = node->children;

    while (children != NULL) {
        if (trieGetKey(children) == key) {
            return children;
        }
        children = children->next;
    }
    return NULL;
}

/* Add a new key to the Trie. If the key already exists, returns TRIE_ERR */
int trieAdd(trie *t, const unsigned char *key, size_t len, void *val)
{
    trieNode *current = t->root;


    while (len-- > 0) {
        trieNode *next = _trieGetNextState(current, *key);
        if (next == NULL) {
            next = _trieCreateNode(t, *key);
            next->next = current->children;
            current->children = next;
        }
        current = next;
        ++key;
    }

    /* Make sure the node was *not* already in use.
    * trieAdd won't replace an already in use node */
    if (trieNodeIsFinal(current)) {
        return TRIE_ERR;
    }

    trieSetVal(current, val);
    t->used++;
    return TRIE_OK;
}

/* Replace an existing key with a new value. If the key doesn't exist,
 * returns TRIE_ERR */
int trieReplace(trie *t, const unsigned char *key, size_t len, void *val,
    trieValDestructor destructor)
{
    trieNode *node;

    node = trieFind(t, key, len);
    if (node == NULL) {
        return TRIE_ERR;
    }
    trieReplaceVal(node, val, destructor);
    return TRIE_OK;
}

/* Add OR Replace in a single traversal.
 * Return 1 if the key was added, 0 if it was replaced.
 * This could be implemented in a much simpler way by doing a trieFind and
 * either trieAdd or trieReplace, but that would require two Trie traversals */
int trieAddOrReplace(trie *t, const unsigned char *key, size_t len, void *val,
    trieValDestructor destructor)
{
    trieNode *current = t->root;

    while (len-- > 0) {
        trieNode *next = _trieGetNextState(current, *key);
        if (next == NULL) {
            next = _trieCreateNode(t, *key);
            next->next = current->children;
            current->children = next;
        }
        current = next;
        ++key;
    }
    // The entry already existed, replace it
    if (trieNodeIsFinal(current)) {
        trieReplaceVal(current, val, destructor);
        return 0;
    }
    trieSetVal(current, val);
    t->used++;
    return 1;
}

/* Low level trieFind() that returns a node even if it's NOT in use.
 * Callers have to check the node with trieNodeIsFinal() in order to know if
 * the node holds a value. */
static trieNode *_trieLookupNode(trie *t, const unsigned char *key, size_t len)
{
    trieNode *current = t->root;
    while (len-- && current != NULL) {
        current = _trieGetNextState(current, *key);
        ++key;
    }
    return current;
}

/* Lookup a key by traversing the Trie. */
trieNode *trieFind(trie *t, const unsigned char *key, size_t len)
{
    trieNode *e = _trieLookupNode(t, key, len);
    if (e == NULL || !trieNodeIsFinal(e)) {
        return NULL;
    }
    return e;
}

/* Low level function to remove a node from the Trie.
 * This not only destroys the node, but it also removes it from the tree by
 * updating the parent or previous node pointers */
static void _trieRemoveNode(trie *t, trieNode *node, trieNode *parent,
    trieNode *prev)
{
    if (prev == NULL) {
        parent->children = node->next;
    } else {
        prev->next = node->next;
    }
    _trieDestroyNode(t, node);
}

/* Internal implementation of trieDelete that recursively traverses the Trie
 * in order to find and destroy the target node.
 * Once the target node is destroyed, it makes sure to destroy any node that
 * have become useless, that is, nodes that don't hold a value and don't have
 * any children. */
static int _trieDelete(trie *t, trieNode *parent, const unsigned char *key,
    size_t len, int *ret, trieValDestructor destructor)
{
    trieNode *node = parent->children;
    trieNode *prev = NULL;

    /* Find the next node and keep a pointer to the previous one */
    while (node != NULL && trieGetKey(node) != *key) {
        prev = node;
        node = node->next;
    }
    /* Make sure we've found the node */
    if (node == NULL || trieGetKey(node) != *key) return TRIE_ERR;

    /* We've reached our final node (len == 0), delete it */
    if (len == 0) {
        if (!trieNodeIsFinal(node)) return TRIE_ERR;

        trieFreeVal(node, destructor);
        t->used--;
        *ret = TRIE_OK;
        if (node->children == NULL) {
            /* The node is useless, we can safely remove it */
            _trieRemoveNode(t, node, parent, prev);
            return TRIE_OK;
        }
    } else {
        /* If we didn't reach the end of the key yet (len > 0),
        * continue traversing the tree recursively */
        if (_trieDelete(t, node, ++key, --len, ret, destructor) == TRIE_OK) {
            /* If _trieDelete() returns TRIE_OK, it means that our child got
             * destroyed as a result of the delete. If the current node
             * is useless, then we can safely destroy it as well. */
            if (!trieNodeIsFinal(node) && node->children == NULL) {
                _trieRemoveNode(t, node, parent, prev);
                return TRIE_OK;
            }
        }
    }
    return TRIE_ERR;
}

/* Delete an entry from a Trie. If the key wasn't found, returns TRIE_ERR */
int trieDelete(trie *t, const unsigned char *key, size_t len,
    trieValDestructor destructor)
{
    int ret = TRIE_ERR;

    /* Special casing the root node */
    if (len == 0 && trieNodeIsFinal(t->root)) {
        trieFreeVal(t->root, destructor);
        return TRIE_OK;
    }
    _trieDelete(t, t->root, key, --len, &ret, destructor);
    return ret;
}

/* Internal function used to traverse a trie from a given node. */
static int _trieWalkFromNode(trieNode *node, char **buffer, size_t len,
    trieWalkCallback callback, void *data)
{
    /* Reallocate the key buffer if necessary */
    if (len >= TRIE_WALK_BUFFERSTEP && (len % TRIE_WALK_BUFFERSTEP) == 0) {
        *buffer = zrealloc(*buffer, sizeof(char) *
            (len + TRIE_WALK_BUFFERSTEP));
    }

    while (node != NULL) {
        trieNode *next = node->next;
        (*buffer)[len] = trieGetKey(node);
        if (_trieWalkFromNode(node->children, buffer, len + 1, callback,
            data) == TRIE_ERR) {
            return TRIE_ERR;
        }
        if (trieNodeIsFinal(node)) {
            if (callback(node, (const unsigned char *)*buffer, len + 1,
                data) == TRIE_ERR) {
                return TRIE_ERR;
            }
        }
        node = next;
    }
    return TRIE_OK;
}

/* Same as trieWalk(), except it starts traversing the Trie from the given
 * prefix */
int trieWalkFromPrefix(trie *t, trieWalkCallback callback, void *data,
    const unsigned char *prefix, size_t len)
{
    char *buffer;
    int ret = TRIE_ERR;
    trieNode *root = NULL;

    root = _trieLookupNode(t, prefix, len);
    if (root == NULL) {
        return TRIE_ERR;
    }

    buffer = zmalloc(sizeof(char) * TRIE_WALK_BUFFERSTEP *
        (len / TRIE_WALK_BUFFERSTEP + 1));
    memcpy(buffer, prefix, len);
    ret = _trieWalkFromNode(root->children, &buffer, len, callback, data);
    if (trieNodeIsFinal(root)) {
        if (callback(root, prefix, len, data) == TRIE_ERR) {
            return TRIE_ERR;
        }
    }
    zfree(buffer);
    return ret;
}

/* Traverse an entire Trie in depth first search. For every final node,
 * the callback is called with the provided user data */
int trieWalk(trie *t, trieWalkCallback callback, void *data)
{
    return trieWalkFromPrefix(t, callback, data, (const unsigned char *)"", 0);
}

/* Internal function to release a Trie, starting from a given node. */
static void _trieReleaseFromNode(trie *t, trieNode *node,
    trieValDestructor destructor)
{
    trieNode *next = NULL;

    while (node != NULL) {
        next = node->next;
        _trieReleaseFromNode(t, node->children, destructor);
        trieFreeVal(node, destructor);
        _trieDestroyNode(t, node);
        node = next;
    }
}

/* Destroy a Trie, including every value stored in it */
void trieRelease(trie *t, trieValDestructor destructor)
{
    _trieReleaseFromNode(t, t->root, destructor);
    zfree(t);
}
