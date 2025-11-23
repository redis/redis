/* TRING -- AVL tree with ring buffer storage implementation.
 *
 * Copyright (c) 2024-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include "streamtring.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "zmalloc.h"

/* Helper functions for AVL tree operations */

/* Get the height of a node. Returns 0 for TRING_NULL nodes. */
static inline uint32_t getHeight(tringTree *tree, uint32_t idx) {
    return (idx == TRING_NULL) ? 0 : tree->nodes[idx].height;
}

/* Calculate the balance factor of a node.
 * Balance factor = height(right) - height(left).
 * A balance factor of -1, 0, or 1 indicates the tree is balanced. */
static inline int getBalance(tringTree *tree, uint32_t idx) {
    if (idx == TRING_NULL) return 0;
    return (int)getHeight(tree, tree->nodes[idx].right) - 
           (int)getHeight(tree, tree->nodes[idx].left);
}

/* Update the height of a node based on its children's heights. */
static void updateHeight(tringTree *tree, uint32_t idx) {
    if (idx == TRING_NULL) return;
    uint32_t leftHeight = getHeight(tree, tree->nodes[idx].left);
    uint32_t rightHeight = getHeight(tree, tree->nodes[idx].right);
    tree->nodes[idx].height = (leftHeight > rightHeight ? leftHeight : rightHeight) + 1;
}

/* Right rotation around node y.
 * Returns the new root of the subtree. */
static uint32_t rotateRight(tringTree *tree, uint32_t y) {
    uint32_t x = tree->nodes[y].left;
    uint32_t T2 = tree->nodes[x].right;
    
    /* Perform rotation */
    tree->nodes[x].right = y;
    tree->nodes[y].left = T2;
    
    /* Update parents */
    tree->nodes[x].parent = tree->nodes[y].parent;
    tree->nodes[y].parent = x;
    if (T2 != TRING_NULL) {
        tree->nodes[T2].parent = y;
    }
    
    /* Update heights */
    updateHeight(tree, y);
    updateHeight(tree, x);
    
    return x;
}

/* Left rotation around node x.
 * Returns the new root of the subtree. */
static uint32_t rotateLeft(tringTree *tree, uint32_t x) {
    uint32_t y = tree->nodes[x].right;
    uint32_t T2 = tree->nodes[y].left;
    
    /* Perform rotation */
    tree->nodes[y].left = x;
    tree->nodes[x].right = T2;
    
    /* Update parents */
    tree->nodes[y].parent = tree->nodes[x].parent;
    tree->nodes[x].parent = y;
    if (T2 != TRING_NULL) {
        tree->nodes[T2].parent = x;
    }
    
    /* Update heights */
    updateHeight(tree, x);
    updateHeight(tree, y);
    
    return y;
}

/* Rebalance the tree at the given node if needed.
 * Returns the new root of the subtree. */
static uint32_t rebalance(tringTree *tree, uint32_t idx) {
    if (idx == TRING_NULL) return idx;
    
    updateHeight(tree, idx);
    int balance = getBalance(tree, idx);
    
    /* Left Left Case */
    if (balance < -1 && getBalance(tree, tree->nodes[idx].left) <= 0) {
        return rotateRight(tree, idx);
    }
    
    /* Right Right Case */
    if (balance > 1 && getBalance(tree, tree->nodes[idx].right) >= 0) {
        return rotateLeft(tree, idx);
    }
    
    /* Left Right Case */
    if (balance < -1 && getBalance(tree, tree->nodes[idx].left) > 0) {
        tree->nodes[idx].left = rotateLeft(tree, tree->nodes[idx].left);
        return rotateRight(tree, idx);
    }
    
    /* Right Left Case */
    if (balance > 1 && getBalance(tree, tree->nodes[idx].right) < 0) {
        tree->nodes[idx].right = rotateRight(tree, tree->nodes[idx].right);
        return rotateLeft(tree, idx);
    }
    
    return idx;
}

/* Allocate a new node in the ring buffer.
 * Returns the index of the new node, or TRING_NULL on out of memory. */
static uint32_t allocateNode(tringTree *tree, void *value) {
    /* Check if we need to resize */
    if (tree->count >= tree->capacity && tree->capacity < tree->max_capacity) {        
        uint32_t newCapacity = tree->capacity * 2;
        
        /* Cap new capacity at max_capacity*/
        if (newCapacity > tree->max_capacity) {
            newCapacity = tree->max_capacity;
        }
        
        /* Reallocate node array */
        size_t usable, old_usable;
        tringNode *newNodes = zrealloc_usable(tree->nodes, newCapacity * sizeof(tringNode), &usable, &old_usable);
        if (!newNodes) return TRING_NULL;
        
        if (tree->alloc_size) {
            *tree->alloc_size -= old_usable;
            *tree->alloc_size += usable;
        }
        
        tree->nodes = newNodes;
        tree->capacity = newCapacity;
    }
    
    if (tree->tail >= tree->capacity) {
        tree->tail = 0;
    }

    /* Allocate node at tail */
    uint32_t idx = tree->tail;
    tree->nodes[idx].value = value;
    tree->nodes[idx].left = TRING_NULL;
    tree->nodes[idx].right = TRING_NULL;
    tree->nodes[idx].height = 1;
    tree->nodes[idx].parent = TRING_NULL;
    
    /* Update ring buffer pointers - wrap tail on capacity */
    tree->tail++;
    tree->count++;
    
    return idx;
}

/* Recursive helper for insertion.
 * Returns the new root of the subtree. */
static uint32_t insertRecursive(tringTree *tree, uint32_t idx, void *value, void **existing) {
    /* Base case: found insertion point */
    if (idx == TRING_NULL) {
        uint32_t newIdx = allocateNode(tree, value);
        if (newIdx == TRING_NULL) {
            *existing = NULL;
            return TRING_NULL;
        }
        *existing = NULL;
        return newIdx;
    }
    
    /* Compare and recurse */
    int cmp = tree->compare(value, tree->nodes[idx].value);
    
    if (cmp < 0) {
        uint32_t newLeft = insertRecursive(tree, tree->nodes[idx].left, value, existing);
        if (*existing != NULL) return idx;
        assert(newLeft != idx); /* Prevent loops */
        tree->nodes[idx].left = newLeft;
        if (newLeft != TRING_NULL) {
            tree->nodes[newLeft].parent = idx;
        }
    } else if (cmp > 0) {
        uint32_t newRight = insertRecursive(tree, tree->nodes[idx].right, value, existing);
        if (*existing != NULL) return idx;
        assert(newRight != idx); /* Prevent loops */
        tree->nodes[idx].right = newRight;
        if (newRight != TRING_NULL) {
            tree->nodes[newRight].parent = idx;
        }
    } else {
        /* Duplicate value - don't insert, return existing value */
        *existing = tree->nodes[idx].value;
        return idx;
    }
    
    /* Rebalance and return */
    return rebalance(tree, idx);
}

/* Public API implementation */

tringTree *tringNew(tringCompareFunc compare, size_t *alloc_size) {
    if (!compare) return NULL;
    
    size_t usable;
    tringTree *tree = zmalloc_usable(sizeof(tringTree), &usable);
    if (!tree) return NULL;
    
    tree->alloc_size = alloc_size;
    if (tree->alloc_size) *tree->alloc_size += usable;
    
    tree->nodes = zmalloc_usable(TRING_INITIAL_CAPACITY * sizeof(tringNode), &usable);
    if (!tree->nodes) {
        if (tree->alloc_size) *tree->alloc_size -= zmalloc_usable_size(tree);
        zfree(tree);
        return NULL;
    }
    
    if (tree->alloc_size) *tree->alloc_size += usable;
    
    tree->capacity = TRING_INITIAL_CAPACITY;
    tree->max_capacity = TRING_DEFAULT_MAX_CAPACITY;
    tree->count = 0;
    tree->head = 0;
    tree->tail = 0;
    tree->root = TRING_NULL;
    tree->compare = compare;
    tree->free_callback = NULL;
    
    return tree;
}

void tringFree(tringTree *tree) {
    if (!tree) return;
    
    /* Optionally call free callback for all values - handle ring buffer wraparound */
    if (tree->free_callback) {
        for (uint32_t i = 0; i < tree->count; i++) {
            uint32_t idx = (tree->head + i) % tree->capacity;
            if (tree->nodes[idx].value) {
                tree->free_callback(tree->nodes[idx].value);
            }
        }
    }
    
    if (tree->alloc_size) {
        *tree->alloc_size -= zmalloc_usable_size(tree->nodes);
        *tree->alloc_size -= zmalloc_usable_size(tree);
    }
    
    zfree(tree->nodes);
    zfree(tree);
}

int tringInsert(tringTree *tree, void *value, void **existingOut) {
    if (!tree || !value || !tree->max_capacity) return 0;
    
    /* If adding a new element would exceed max_capacity, pop the oldest one first */
    if (tree->count + 1 > tree->max_capacity) {
        if(!tringPopFront(tree)) {
            return 0;
        }
    }
    
    void *existing = NULL;
    uint32_t newRoot = insertRecursive(tree, tree->root, value, &existing);
    
    if (existing == NULL) {
        tree->root = newRoot;
        if (newRoot != TRING_NULL) {
            tree->nodes[newRoot].parent = TRING_NULL;
        }
        return 1;
    }
    
    /* Duplicate found - store existing value if caller wants it */
    if (existingOut) {
        *existingOut = existing;
    }
    
    return 0;
}

void *tringFind(tringTree *tree, void *value) {
    if (!tree || !value) return NULL;
    
    uint32_t current = tree->root;
    
    while (current != TRING_NULL) {
        int cmp = tree->compare(value, tree->nodes[current].value);
        
        if (cmp == 0) {
            return tree->nodes[current].value;
        } else if (cmp < 0) {
            assert(current != tree->nodes[current].left); /* Prevent loops */
            current = tree->nodes[current].left;
        } else {
            assert(current != tree->nodes[current].right); /* Prevent loops */
            current = tree->nodes[current].right;
        }
    }
    
    return NULL;
}

size_t tringSize(tringTree *tree) {
    return tree ? tree->count : 0;
}

int tringEmpty(tringTree *tree) {
    return tree ? (tree->count == 0) : 1;
}

/* Helper function to find the minimum node in a subtree. */
static uint32_t findMin(tringTree *tree, uint32_t idx) {
    if (idx == TRING_NULL) return TRING_NULL;
    
    while (tree->nodes[idx].left != TRING_NULL) {
        idx = tree->nodes[idx].left;
    }
    
    return idx;
}

/* Helper function to rebalance up the tree from a given node to the root.
 * This is used after deletion to ensure the tree remains balanced. */
static void rebalanceUp(tringTree *tree, uint32_t startIdx) {
    uint32_t current = startIdx;
    
    while (current != TRING_NULL) {
        uint32_t parent = tree->nodes[current].parent;
        uint32_t newCurrent = rebalance(tree, current);
        
        /* If rebalancing changed the subtree root, update parent's pointer */
        if (newCurrent != current) {
            if (parent == TRING_NULL) {
                /* This is the root */
                tree->root = newCurrent;
            } else if (tree->nodes[parent].left == current) {
                assert(parent != newCurrent); /* Prevent loops */
                tree->nodes[parent].left = newCurrent;
            } else {
                assert(parent != newCurrent);
                tree->nodes[parent].right = newCurrent;
            }
            tree->nodes[newCurrent].parent = parent;
        }
        
        current = parent;
    }
}

/* Remove a node by its index directly (no search required).
 * Returns 1 on success, 0 on failure.
 * This is more efficient than removeNode() when you already know the index. */
static int removeNodeByIndex(tringTree *tree, uint32_t idx) {
    if (idx == TRING_NULL) {
        return 0;
    }
    
    uint32_t parent = tree->nodes[idx].parent;
    uint32_t replacement = TRING_NULL;
    
    /* Case 1: Node has no left child */
    if (tree->nodes[idx].left == TRING_NULL) {
        replacement = tree->nodes[idx].right;
    }
    /* Case 2: Node has no right child */
    else if (tree->nodes[idx].right == TRING_NULL) {
        replacement = tree->nodes[idx].left;
    }
    /* Case 3: Node has two children - replace with inorder successor */
    else {
        /* Find the leftmost node (minimum) in the right subtree */
        uint32_t leftmost = findMin(tree, tree->nodes[idx].right);
        
        /* Attach the left subtree to the leftmost node */
        tree->nodes[leftmost].left = tree->nodes[idx].left;
        
        /* Update parent pointer of the left subtree */
        if (tree->nodes[idx].left != TRING_NULL) {
            tree->nodes[tree->nodes[idx].left].parent = leftmost;
        }
        
        /* Update heights and rebalance from leftmost up to the right child */
        uint32_t curr = leftmost;
        while (curr != tree->nodes[idx].right) {
            uint32_t parentNode = tree->nodes[curr].parent;
            uint32_t newCurr = rebalance(tree, curr);
            
            /* If rebalancing changed the node, update parent's pointer */
            if (newCurr != curr) {
                if (tree->nodes[parentNode].left == curr) {
                    tree->nodes[parentNode].left = newCurr;
                } else {
                    tree->nodes[parentNode].right = newCurr;
                }
                tree->nodes[newCurr].parent = parentNode;
            }
            
            curr = parentNode;
        }
        
        /* Rebalance the right child itself (root of right subtree) */
        replacement = rebalance(tree, tree->nodes[idx].right);
    }
    
    /* Update parent's pointer to replacement */
    if (parent == TRING_NULL) {
        /* Removing the root */
        tree->root = replacement;
    } else if (tree->nodes[parent].left == idx) {
        assert(parent != replacement); /* Prevent loops */
        tree->nodes[parent].left = replacement;
    } else {
        assert(parent != replacement);
        tree->nodes[parent].right = replacement;
    }
    
    /* Update replacement's parent pointer */
    if (replacement != TRING_NULL) {
        tree->nodes[replacement].parent = parent;
    }
    
    /* Clear the removed node's pointers to prevent loops */
    tree->nodes[idx].value = NULL;
    tree->nodes[idx].left = TRING_NULL;
    tree->nodes[idx].right = TRING_NULL;
    tree->nodes[idx].parent = TRING_NULL;
    tree->nodes[idx].height = 0;

    
    /* Rebalance from parent up to root */
    if (parent != TRING_NULL) {
        rebalanceUp(tree, parent);
    }
    
    return 1;
}

void *tringFront(tringTree *tree) {
    if (!tree || tree->count == 0) return NULL;
    
    void *headValue = tree->nodes[tree->head].value;
    return headValue;
}

void *tringBack(tringTree *tree) {
    if (!tree || tree->count == 0) return NULL;
    
    /* Get the last added element (tail - 1) */
    void *tailValue = tree->nodes[tree->tail - 1].value;
    return tailValue;
}

int tringPopFront(tringTree *tree) {
    if (!tree || tree->count == 0) return 0;
    
    /* Remove the node at head index directly from the AVL tree */
    int success = removeNodeByIndex(tree, tree->head);
    
    if (success) {
        /* Update ring buffer: wrap head on capacity and decrement count */
        tree->head = (tree->head + 1) % tree->capacity;
        tree->count--;
    }
    
    return success;
}

int tringPopBack(tringTree *tree) {
    if (!tree || tree->count == 0) return 0;
    
    /* Calculate the index of the last added element (tail - 1) */
    uint32_t tailIdx = (tree->tail == 0) ? (tree->capacity - 1) : (tree->tail - 1);
    
    /* Remove the node at tail-1 index directly from the AVL tree */
    int success = removeNodeByIndex(tree, tailIdx);
    
    if (success) {
        /* Update ring buffer: decrement tail (with wraparound) and decrement count */
        tree->tail = tailIdx;
        tree->count--;
    }
    
    return success;
}

void tringSetMaxCapacity(tringTree *tree, uint32_t max_capacity) {
    if (!tree) return;
    tree->max_capacity = max_capacity;
}

#ifdef REDIS_TEST
#include <assert.h>
#include <stdio.h>

#define UNUSED(x) (void)(x)
#define TEST(name) printf("test — %s\n", name);

/* Simple integer comparison function for testing */
static int intCompare(const void *a, const void *b) {
    long la = (long)a;
    long lb = (long)b;
    return (la > lb) - (la < lb);
}

/* Test callback counter */
static int freeCallbackCount = 0;

/* Test callback function */
static void testFreeCallback(void *ptr) {
    UNUSED(ptr);
    freeCallbackCount++;
}

/* Verify AVL properties: balance factor and heights.
 * Returns the number of errors found. */
static int verifyAVLProperties(tringTree *tree, uint32_t nodeIdx, int *heightOut) {
    if (nodeIdx == TRING_NULL) {
        *heightOut = 0;
        return 0;
    }
    
    int leftHeight = 0, rightHeight = 0;
    int errors = 0;
    
    errors += verifyAVLProperties(tree, tree->nodes[nodeIdx].left, &leftHeight);
    errors += verifyAVLProperties(tree, tree->nodes[nodeIdx].right, &rightHeight);
    
    int balance = rightHeight - leftHeight;
    if (balance < -1 || balance > 1) {
        printf("ERROR: Node at index %u has invalid balance factor %d\n", nodeIdx, balance);
        errors++;
    }
    
    int expectedHeight = 1 + (leftHeight > rightHeight ? leftHeight : rightHeight);
    if (tree->nodes[nodeIdx].height != (uint32_t)expectedHeight) {
        printf("ERROR: Node at index %u has wrong height: expected %d, got %u\n", 
               nodeIdx, expectedHeight, tree->nodes[nodeIdx].height);
        errors++;
    }
    
    *heightOut = expectedHeight;
    return errors;
}

/* Verify no loops in tree structure.
 * Checks that:
 * 1. No node has itself as a child (child index != current node index)
 * 2. Parent index is different from both children indices
 * 3. Children's parent pointers correctly point back to parent
 * Returns the number of errors found. */
static int verifyNoLoops(tringTree *tree, uint32_t nodeIdx, uint32_t expectedParent) {
    if (nodeIdx == TRING_NULL) {
        return 0;
    }
    
    int errors = 0;
    uint32_t leftChild = tree->nodes[nodeIdx].left;
    uint32_t rightChild = tree->nodes[nodeIdx].right;
    
    /* Check that no child node has the same index as current node */
    if (leftChild != TRING_NULL && leftChild == nodeIdx) {
        printf("ERROR: Node at index %u has itself as left child (loop)\n", nodeIdx);
        errors++;
    }
    
    if (rightChild != TRING_NULL && rightChild == nodeIdx) {
        printf("ERROR: Node at index %u has itself as right child (loop)\n", nodeIdx);
        errors++;
    }
    
    /* Check that parent pointer is correct */
    if (tree->nodes[nodeIdx].parent != expectedParent) {
        printf("ERROR: Node at index %u has wrong parent: expected %u, got %u\n", 
               nodeIdx, expectedParent, tree->nodes[nodeIdx].parent);
        errors++;
    }
    
    /* Recursively verify left subtree */
    if (leftChild != TRING_NULL && leftChild != nodeIdx) {
        errors += verifyNoLoops(tree, leftChild, nodeIdx);
    }
    
    /* Recursively verify right subtree */
    if (rightChild != TRING_NULL && rightChild != nodeIdx) {
        errors += verifyNoLoops(tree, rightChild, nodeIdx);
    }
    
    return errors;
}

/* Combined verification: checks both AVL properties and absence of loops.
 * Returns the number of errors found. */
static int verifyTreeIntegrity(tringTree *tree) {
    if (!tree || tree->count == 0) {
        return 0;
    }
    
    int errors = 0;
    int height;
    
    /* Verify AVL balance and heights */
    errors += verifyAVLProperties(tree, tree->root, &height);
    
    /* Verify no loops in structure */
    errors += verifyNoLoops(tree, tree->root, TRING_NULL);
    
    return errors;
}

int tringTest(int argc, char *argv[], int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    
    int errors = 0;
    
    TEST("tringNew creates empty tree") {
        tringTree *tree = tringNew(intCompare, NULL);
        assert(tree != NULL);
        assert(tringEmpty(tree));
        assert(tringSize(tree) == 0);
        tringFree(tree);
    }
    
    TEST("tringInsert adds values") {
        tringTree *tree = tringNew(intCompare, NULL);
        assert(tringInsert(tree, (void*)10L, NULL));
        assert(tringInsert(tree, (void*)20L, NULL));
        assert(tringInsert(tree, (void*)5L, NULL));
        assert(tringSize(tree) == 3);
        assert(!tringEmpty(tree));
        
        /* Verify tree integrity (AVL properties and no loops) */
        errors += verifyTreeIntegrity(tree);
        
        tringFree(tree);
    }
    
    TEST("tringInsert rejects duplicates") {
        tringTree *tree = tringNew(intCompare, NULL);
        tringInsert(tree, (void*)10L, NULL);
        assert(!tringInsert(tree, (void*)10L, NULL));
        assert(tringSize(tree) == 1);
        
        /* Test that existingOut returns the existing value on duplicate */
        void *existing = NULL;
        assert(!tringInsert(tree, (void*)10L, &existing));
        assert(existing == (void*)10L);
        assert(tringSize(tree) == 1);
        
        tringFree(tree);
    }
    
    TEST("tringFind locates values") {
        tringTree *tree = tringNew(intCompare, NULL);
        tringInsert(tree, (void*)10L, NULL);
        tringInsert(tree, (void*)20L, NULL);
        tringInsert(tree, (void*)5L, NULL);
        assert(tringFind(tree, (void*)10L) == (void*)10L);
        assert(tringFind(tree, (void*)20L) == (void*)20L);
        assert(tringFind(tree, (void*)5L) == (void*)5L);
        assert(tringFind(tree, (void*)99L) == NULL);
        
        /* Verify tree integrity (AVL properties and no loops) */
        errors += verifyTreeIntegrity(tree);
        
        tringFree(tree);
    }
    
    TEST("tringInsert maintains balance with many insertions") {
        tringTree *tree = tringNew(intCompare, NULL);
        /* Insert values that would create an unbalanced tree without rotations */
        for (long i = 1; i <= 100; i++) {
            assert(tringInsert(tree, (void*)i, NULL));
        }
        assert(tringSize(tree) == 100);
        
        /* Verify tree integrity after all insertions */
        errors += verifyTreeIntegrity(tree);
        
        /* Verify all values are findable */
        for (long i = 1; i <= 100; i++) {
            assert(tringFind(tree, (void*)i) == (void*)i);
        }
        tringFree(tree);
    }
    
    TEST("tringSize and tringEmpty work correctly") {
        tringTree *tree = tringNew(intCompare, NULL);
        assert(tringEmpty(tree));
        tringInsert(tree, (void*)1L, NULL);
        assert(!tringEmpty(tree));
        assert(tringSize(tree) == 1);
        tringInsert(tree, (void*)2L, NULL);
        assert(tringSize(tree) == 2);
        tringFree(tree);
    }
    
    TEST("tringFront returns head value (first inserted)") {
        tringTree *tree = tringNew(intCompare, NULL);
        tringInsert(tree, (void*)50L, NULL);
        tringInsert(tree, (void*)30L, NULL);
        tringInsert(tree, (void*)70L, NULL);
        tringInsert(tree, (void*)20L, NULL);
        tringInsert(tree, (void*)40L, NULL);
        /* tringFront should return the first inserted value (50), not minimum */
        assert(tringFront(tree) == (void*)50L);
        
        /* After popping, tringFront should return the next value */
        assert(tringPopFront(tree) == 1);
        assert(tringFront(tree) == (void*)30L);
        errors += verifyTreeIntegrity(tree);
        
        tringFree(tree);
    }
    
    TEST("tringBack returns last added value") {
        tringTree *tree = tringNew(intCompare, NULL);
        tringInsert(tree, (void*)50L, NULL);
        tringInsert(tree, (void*)30L, NULL);
        tringInsert(tree, (void*)70L, NULL);
        tringInsert(tree, (void*)20L, NULL);
        tringInsert(tree, (void*)80L, NULL);
        /* tringBack should return the last inserted value (80), which happens to also be max */
        assert(tringBack(tree) == (void*)80L);
        
        /* Insert a smaller value - tringBack should return it (not the max) */
        tringInsert(tree, (void*)10L, NULL);
        assert(tringBack(tree) == (void*)10L);
        
        tringFree(tree);
    }
    
    TEST("tringPop removes items in FIFO order") {
        tringTree *tree = tringNew(intCompare, NULL);
        tringInsert(tree, (void*)50L, NULL);
        tringInsert(tree, (void*)30L, NULL);
        tringInsert(tree, (void*)70L, NULL);
        tringInsert(tree, (void*)20L, NULL);
        tringInsert(tree, (void*)40L, NULL);
        
        /* Verify tree integrity after insertions */
        errors += verifyTreeIntegrity(tree);
        
        /* Pop should remove in insertion order (FIFO) */
        assert(tringFront(tree) == (void*)50L);
        assert(tringPopFront(tree) == 1);
        assert(tringSize(tree) == 4);
        assert(tringFind(tree, (void*)50L) == NULL);
        
        /* Verify tree integrity after first pop */
        errors += verifyTreeIntegrity(tree);
        
        assert(tringFront(tree) == (void*)30L);
        assert(tringPopFront(tree) == 1);
        assert(tringSize(tree) == 3);
        assert(tringFind(tree, (void*)30L) == NULL);
        
        /* Verify tree integrity after second pop */
        errors += verifyTreeIntegrity(tree);
        
        tringFree(tree);
    }
    
    TEST("tringFront/Back return NULL on empty tree") {
        tringTree *tree = tringNew(intCompare, NULL);
        assert(tringFront(tree) == NULL);
        assert(tringBack(tree) == NULL);
        assert(tringPopFront(tree) == 0);
        tringFree(tree);
    }
    
    TEST("tringPop works until tree is empty") {
        tringTree *tree = tringNew(intCompare, NULL);
        tringInsert(tree, (void*)3L, NULL);
        tringInsert(tree, (void*)1L, NULL);
        tringInsert(tree, (void*)2L, NULL);
        
        /* Verify tree integrity after insertions */
        errors += verifyTreeIntegrity(tree);
        
        /* Pop in insertion order: 3, 1, 2 */
        assert(tringFront(tree) == (void*)3L);
        assert(tringPopFront(tree) == 1);
        errors += verifyTreeIntegrity(tree);
        
        assert(tringFront(tree) == (void*)1L);
        assert(tringPopFront(tree) == 1);
        errors += verifyTreeIntegrity(tree);
        
        assert(tringFront(tree) == (void*)2L);
        assert(tringPopFront(tree) == 1);
        assert(tringEmpty(tree));
        assert(tringPopFront(tree) == 0);
        
        tringFree(tree);
    }
    
    TEST("AVL properties maintained with mixed operations") {
        tringTree *tree = tringNew(intCompare, NULL);
        
        /* Insert values that trigger various rotations */
        long values[] = {50, 25, 75, 10, 30, 60, 80, 5, 15, 27, 35};
        size_t n = sizeof(values) / sizeof(values[0]);
        
        for (size_t i = 0; i < n; i++) {
            assert(tringInsert(tree, (void*)values[i], NULL));
            /* Verify tree integrity after each insertion */
            errors += verifyTreeIntegrity(tree);
        }
        
        assert(tringSize(tree) == n);
        
        /* Pop some items and verify balance is maintained */
        for (int i = 0; i < 3; i++) {
            int success = tringPopFront(tree);
            assert(success == 1);
            /* Verify tree integrity after each pop */
            errors += verifyTreeIntegrity(tree);
        }
        
        assert(tringSize(tree) == n - 3);
        
        /* Verify remaining values are still findable */
        for (size_t i = 3; i < n; i++) {
            assert(tringFind(tree, (void*)values[i]) == (void*)values[i]);
        }
        
        tringFree(tree);
    }
    
    TEST("AVL properties with sequential insertions") {
        tringTree *tree = tringNew(intCompare, NULL);
        
        /* Sequential insertions should trigger rotations */
        for (long i = 1; i <= 50; i++) {
            assert(tringInsert(tree, (void*)i, NULL));
            /* Check integrity every 10 insertions */
            if (i % 10 == 0) {
                errors += verifyTreeIntegrity(tree);
            }
        }
        
        /* Final verification */
        errors += verifyTreeIntegrity(tree);
        assert(tringSize(tree) == 50);
        
        /* Pop half the items */
        for (int i = 0; i < 25; i++) {
            assert(tringPopFront(tree) == 1);
        }
        
        /* Verify balance after pops */
        errors += verifyTreeIntegrity(tree);
        assert(tringSize(tree) == 25);
        
        tringFree(tree);
    }
    
    TEST("NULL parameter checks") {
        /* Test NULL tree parameter */
        assert(tringSize(NULL) == 0);
        assert(tringEmpty(NULL) == 1);
        assert(tringFind(NULL, (void*)1L) == NULL);
        assert(tringInsert(NULL, (void*)1L, NULL) == 0);
        assert(tringFront(NULL) == NULL);
        assert(tringBack(NULL) == NULL);
        assert(tringPopFront(NULL) == 0);
        tringFree(NULL);  /* Should not crash */
        
        /* Test NULL compare function */
        assert(tringNew(NULL, NULL) == NULL);
        
        /* Test NULL value parameter */
        tringTree *tree = tringNew(intCompare, NULL);
        assert(tringInsert(tree, NULL, NULL) == 0);
        assert(tringFind(tree, NULL) == NULL);
        tringFree(tree);
    }
    
    TEST("Right-Left rotation case") {
        tringTree *tree = tringNew(intCompare, NULL);
        
        /* Insert values to trigger Right-Left case:
         * Insert 10, 30, 20 - this creates right-heavy then needs right-left rotation */
        tringInsert(tree, (void*)10L, NULL);
        tringInsert(tree, (void*)30L, NULL);
        tringInsert(tree, (void*)20L, NULL);
        
        errors += verifyTreeIntegrity(tree);
        
        /* Another Right-Left case pattern */
        tringInsert(tree, (void*)50L, NULL);
        tringInsert(tree, (void*)70L, NULL);
        tringInsert(tree, (void*)60L, NULL);
        
        errors += verifyTreeIntegrity(tree);
        
        tringFree(tree);
    }
    
    TEST("Node removal with only left child") {
        tringTree *tree = tringNew(intCompare, NULL);
        
        /* Create a structure where we remove a node with only left child */
        tringInsert(tree, (void*)50L, NULL);
        tringInsert(tree, (void*)30L, NULL);
        tringInsert(tree, (void*)20L, NULL);  /* Left child of 30 */
        
        /* Now pop 50 (head), then pop 30 which will have only left child (20) */
        assert(tringPopFront(tree) == 1);  /* Remove 50 */
        assert(tringPopFront(tree) == 1);  /* Remove 30, which has only left child 20 */
        
        errors += verifyTreeIntegrity(tree);
        assert(tringSize(tree) == 1);
        assert(tringFind(tree, (void*)20L) == (void*)20L);
        
        tringFree(tree);
    }
    
    TEST("Free callback functionality") {
        tringTree *tree = tringNew(intCompare, NULL);
        
        /* Set up callback */
        tree->free_callback = testFreeCallback;
        freeCallbackCount = 0;
        
        /* Insert some values */
        tringInsert(tree, (void*)10L, NULL);
        tringInsert(tree, (void*)20L, NULL);
        tringInsert(tree, (void*)30L, NULL);
        
        /* Free the tree - callback should be called for each value */
        tringFree(tree);
        
        /* Verify callback was called for all 3 values */
        assert(freeCallbackCount == 3);
    }
    
    TEST("tringSize returns correct count") {
        tringTree *tree = tringNew(intCompare, NULL);
        
        /* Verify size on non-NULL tree with 0 elements */
        size_t size = tringSize(tree);
        assert(size == 0);
        
        /* Add elements and check size */
        tringInsert(tree, (void*)1L, NULL);
        size = tringSize(tree);
        assert(size == 1);
        
        tringInsert(tree, (void*)2L, NULL);
        size = tringSize(tree);
        assert(size == 2);
        
        tringInsert(tree, (void*)3L, NULL);
        size = tringSize(tree);
        assert(size == 3);
        
        /* Pop and verify size decreases */
        assert(tringPopFront(tree) == 1);
        size = tringSize(tree);
        assert(size == 2);
        
        assert(tringPopFront(tree) == 1);
        size = tringSize(tree);
        assert(size == 1);
        
        assert(tringPopFront(tree) == 1);
        size = tringSize(tree);
        assert(size == 0);
        
        tringFree(tree);
    }
    
    TEST("max_capacity evicts oldest elements when exceeded") {
        tringTree *tree = tringNew(intCompare, NULL);
        
        /* Set max capacity to 6 */
        tringSetMaxCapacity(tree, 6);
        
        /* Insert 12 elements */
        assert(tringInsert(tree, (void*)1L, NULL));
        assert(tringSize(tree) == 1);
        assert(tringFind(tree, (void*)1L) != NULL);

        assert(tringInsert(tree, (void*)2L, NULL));
        assert(tringSize(tree) == 2);
        assert(tringFind(tree, (void*)2L) != NULL);

        assert(tringInsert(tree, (void*)3L, NULL));
        assert(tringSize(tree) == 3);
        assert(tringFind(tree, (void*)3L) != NULL);

        assert(tringInsert(tree, (void*)4L, NULL));
        assert(tringSize(tree) == 4);
        assert(tringFind(tree, (void*)4L) != NULL);

        assert(tringInsert(tree, (void*)5L, NULL));
        assert(tringSize(tree) == 5);
        assert(tringFind(tree, (void*)5L) != NULL);

        assert(tringInsert(tree, (void*)6L, NULL));
        assert(tringSize(tree) == 6);
        assert(tringFind(tree, (void*)6L) != NULL);

        assert(tringInsert(tree, (void*)7L, NULL));
        assert(tringSize(tree) == 6);
        assert(tringFind(tree, (void*)7L) != NULL);

        assert(tringInsert(tree, (void*)8L, NULL));
        assert(tringSize(tree) == 6);
        assert(tringFind(tree, (void*)8L) != NULL);

        assert(tringInsert(tree, (void*)9L, NULL));
        assert(tringSize(tree) == 6);
        assert(tringFind(tree, (void*)9L) != NULL);

        assert(tringInsert(tree, (void*)10L, NULL));
        assert(tringSize(tree) == 6);
        assert(tringFind(tree, (void*)10L) != NULL);

        assert(tringInsert(tree, (void*)11L, NULL));
        assert(tringSize(tree) == 6);
        assert(tringFind(tree, (void*)11L) != NULL);

        assert(tringInsert(tree, (void*)12L, NULL));
        assert(tringSize(tree) == 6);
        assert(tringFind(tree, (void*)12L) != NULL);

        /* Should have exactly 6 elements (max_capacity) */
        assert(tringSize(tree) == 6);
        
        /* First 6 elements (1 to 6) should have been auto-evicted */
        assert(tringFind(tree, (void*)1L) == NULL);
        assert(tringFind(tree, (void*)2L) == NULL);
        assert(tringFind(tree, (void*)3L) == NULL);
        assert(tringFind(tree, (void*)4L) == NULL);
        assert(tringFind(tree, (void*)5L) == NULL);
        assert(tringFind(tree, (void*)6L) == NULL);
        
        /* Elements 7-12 should still be present */
        for (long i = 7; i <= 12; i++) {
            assert(tringFind(tree, (void*)i) == (void*)i);
        }
        
        /* Verify AVL properties */
        errors += verifyTreeIntegrity(tree);
        
        tringFree(tree);
    }
    
    TEST("ring buffer wraps multiple times with many insertions") {
        tringTree *tree = tringNew(intCompare, NULL);
        
        /* Set small max capacity to force multiple wraps */
        tringSetMaxCapacity(tree, 8);
        
        /* Insert 100 elements - this will cause many wraps */
        for (long i = 1; i <= 100; i++) {
            assert(tringInsert(tree, (void*)i, NULL));
            
            /* Verify AVL properties periodically */
            if (i % 10 == 0) {
                errors += verifyTreeIntegrity(tree);
            }
        }
        
        /* Should still have exactly 8 elements */
        assert(tringSize(tree) == 8);
        
        /* Only the last 8 elements (93-100) should be present */
        for (long i = 1; i <= 92; i++) {
            assert(tringFind(tree, (void*)i) == NULL);
        }
        
        for (long i = 93; i <= 100; i++) {
            assert(tringFind(tree, (void*)i) == (void*)i);
        }
        
        /* Final AVL verification after all operations */
        errors += verifyTreeIntegrity(tree);
        
        /* Verify head and tail have wrapped (tail should be less than head or wrapped around) */
        /* With 100 insertions and capacity 8, we expect multiple wraps */
        assert(tree->count == 8);
        
        tringFree(tree);
    }
    
    TEST("removeNodeByIndex - remove leaf node") {
        tringTree *tree = tringNew(intCompare, NULL);
        
        /* Create a tree where we can remove a leaf node */
        tringInsert(tree, (void*)50L, NULL);
        tringInsert(tree, (void*)30L, NULL);
        tringInsert(tree, (void*)70L, NULL);
        tringInsert(tree, (void*)20L, NULL);
        tringInsert(tree, (void*)40L, NULL);
        
        /* At this point, head is at index 0 (value 50) */
        /* Pop removes the head, which is the root */
        assert(tringFront(tree) == (void*)50L);
        assert(tringPopFront(tree) == 1);
        assert(tringSize(tree) == 4);
        assert(tringFind(tree, (void*)50L) == NULL);
        
        /* Verify remaining elements */
        assert(tringFind(tree, (void*)30L) == (void*)30L);
        assert(tringFind(tree, (void*)70L) == (void*)70L);
        assert(tringFind(tree, (void*)20L) == (void*)20L);
        assert(tringFind(tree, (void*)40L) == (void*)40L);
        
        errors += verifyTreeIntegrity(tree);
        tringFree(tree);
    }
    
    TEST("removeNodeByIndex - remove node with only left child") {
        tringTree *tree = tringNew(intCompare, NULL);
        
        /* Create structure: insert in order that creates node with only left child */
        tringInsert(tree, (void*)50L, NULL);  /* head at index 0 */
        tringInsert(tree, (void*)30L, NULL);
        tringInsert(tree, (void*)20L, NULL);
        
        /* Remove head (50), which is root with two children (30 and NULL) */
        assert(tringFront(tree) == (void*)50L);
        assert(tringPopFront(tree) == 1);
        assert(tringSize(tree) == 2);
        
        errors += verifyTreeIntegrity(tree);
        
        /* Now remove next head (30), which has only left child (20) */
        assert(tringFront(tree) == (void*)30L);
        assert(tringPopFront(tree) == 1);
        assert(tringSize(tree) == 1);
        assert(tringFind(tree, (void*)20L) == (void*)20L);
        
        errors += verifyTreeIntegrity(tree);
        tringFree(tree);
    }
    
    TEST("removeNodeByIndex - remove node with only right child") {
        tringTree *tree = tringNew(intCompare, NULL);
        
        /* Create structure with node having only right child */
        tringInsert(tree, (void*)10L, NULL);  /* head at index 0 */
        tringInsert(tree, (void*)30L, NULL);
        tringInsert(tree, (void*)40L, NULL);
        
        /* Remove head (10), which is root */
        assert(tringFront(tree) == (void*)10L);
        assert(tringPopFront(tree) == 1);
        assert(tringSize(tree) == 2);
        
        errors += verifyTreeIntegrity(tree);
        
        /* Now remove next head (30), which has only right child (40) */
        assert(tringFront(tree) == (void*)30L);
        assert(tringPopFront(tree) == 1);
        assert(tringSize(tree) == 1);
        assert(tringFind(tree, (void*)40L) == (void*)40L);
        
        errors += verifyTreeIntegrity(tree);
        tringFree(tree);
    }
    
    TEST("removeNodeByIndex - remove node with two children") {
        tringTree *tree = tringNew(intCompare, NULL);
        
        /* Create a tree where head node has two children */
        tringInsert(tree, (void*)50L, NULL);  /* head at index 0 - will have two children */
        tringInsert(tree, (void*)30L, NULL);  /* left child of 50 */
        tringInsert(tree, (void*)70L, NULL);  /* right child of 50 */
        tringInsert(tree, (void*)60L, NULL);
        tringInsert(tree, (void*)80L, NULL);
        
        /* Remove head (50), which has two children (30 and 70) */
        assert(tringFront(tree) == (void*)50L);
        assert(tringPopFront(tree) == 1);
        assert(tringSize(tree) == 4);
        assert(tringFind(tree, (void*)50L) == NULL);
        
        /* Verify all other elements still present */
        assert(tringFind(tree, (void*)30L) == (void*)30L);
        assert(tringFind(tree, (void*)70L) == (void*)70L);
        assert(tringFind(tree, (void*)60L) == (void*)60L);
        assert(tringFind(tree, (void*)80L) == (void*)80L);
        
        errors += verifyTreeIntegrity(tree);
        tringFree(tree);
    }
    
    TEST("removeNodeByIndex - remove root node") {
        tringTree *tree = tringNew(intCompare, NULL);
        
        /* Test removing root with no children */
        tringInsert(tree, (void*)50L, NULL);
        assert(tringFront(tree) == (void*)50L);
        assert(tringPopFront(tree) == 1);
        assert(tringSize(tree) == 0);
        assert(tringEmpty(tree));
        assert(tree->root == TRING_NULL);
        
        /* Test removing root with one child */
        tringInsert(tree, (void*)50L, NULL);
        tringInsert(tree, (void*)30L, NULL);
        assert(tringFront(tree) == (void*)50L);
        assert(tringPopFront(tree) == 1);
        assert(tringSize(tree) == 1);
        assert(tree->root != TRING_NULL);
        errors += verifyTreeIntegrity(tree);
        
        tringFree(tree);
    }
    
    TEST("removeNodeByIndex - sequential removes maintain balance") {
        tringTree *tree = tringNew(intCompare, NULL);
        
        /* Insert multiple elements */
        long values[] = {50, 30, 70, 20, 40, 60, 80, 10, 25, 35, 45};
        size_t n = sizeof(values) / sizeof(values[0]);
        
        for (size_t i = 0; i < n; i++) {
            tringInsert(tree, (void*)values[i], NULL);
        }
        
        errors += verifyTreeIntegrity(tree);
        
        /* Remove elements one by one and verify balance */
        for (size_t i = 0; i < n; i++) {
            assert(tringFront(tree) == (void*)values[i]);
            assert(tringPopFront(tree) == 1);
            assert(tringSize(tree) == n - i - 1);
            
            /* Verify AVL properties after each removal */
            if (tree->count > 0) {
                errors += verifyTreeIntegrity(tree);
            }
        }
        
        assert(tringEmpty(tree));
        tringFree(tree);
    }
    
    TEST("removeNodeByIndex - remove from complex tree structure") {
        tringTree *tree = tringNew(intCompare, NULL);

        /* Build a complex tree */
        for (long i = 1; i <= 15; i++) {
            tringInsert(tree, (void*)i, NULL);
        }
        
        errors += verifyTreeIntegrity(tree);
        
        /* Remove first 5 elements */
        for (int i = 0; i < 5; i++) {
            assert(tringPopFront(tree) == 1);
            errors += verifyTreeIntegrity(tree);
        }
        
        assert(tringSize(tree) == 10);
        
        /* Verify elements 1-5 are gone, 6-15 remain */
        for (long i = 1; i <= 5; i++) {
            assert(tringFind(tree, (void*)i) == NULL);
        }
        for (long i = 6; i <= 15; i++) {
            assert(tringFind(tree, (void*)i) == (void*)i);
        }
        
        tringFree(tree);
    }
    
    TEST("removeNodeByIndex - empty tree handling") {
        tringTree *tree = tringNew(intCompare, NULL);
        
        /* Try to pop from empty tree */
        assert(tringPopFront(tree) == 0);
        assert(tringSize(tree) == 0);
        
        /* Insert one, remove it, try again */
        tringInsert(tree, (void*)10L, NULL);
        assert(tringFront(tree) == (void*)10L);
        assert(tringPopFront(tree) == 1);
        
        assert(tringPopFront(tree) == 0);
        
        tringFree(tree);
    }
    
    TEST("tringPopBack removes last added item") {
        tringTree *tree = tringNew(intCompare, NULL);
        
        /* Insert some values */
        tringInsert(tree, (void*)50L, NULL);
        tringInsert(tree, (void*)30L, NULL);
        tringInsert(tree, (void*)70L, NULL);
        tringInsert(tree, (void*)20L, NULL);
        tringInsert(tree, (void*)40L, NULL);
        
        /* Verify tree integrity after insertions */
        errors += verifyTreeIntegrity(tree);
        
        /* tringBack should return the last inserted value (40) */
        assert(tringBack(tree) == (void*)40L);
        
        /* Pop back should remove it */
        assert(tringPopBack(tree) == 1);
        assert(tringSize(tree) == 4);
        assert(tringFind(tree, (void*)40L) == NULL);
        
        /* Verify tree integrity after pop */
        errors += verifyTreeIntegrity(tree);
        
        /* Now tringBack should return the previous last value (20) */
        assert(tringBack(tree) == (void*)20L);
        assert(tringPopBack(tree) == 1);
        assert(tringSize(tree) == 3);
        assert(tringFind(tree, (void*)20L) == NULL);
        
        /* Verify tree integrity after second pop */
        errors += verifyTreeIntegrity(tree);
        
        tringFree(tree);
    }
    
    TEST("tringPopBack on empty tree") {
        tringTree *tree = tringNew(intCompare, NULL);
        
        /* Try to pop from empty tree */
        assert(tringPopBack(tree) == 0);
        assert(tringSize(tree) == 0);
        
        /* Insert one, remove it, try again */
        tringInsert(tree, (void*)10L, NULL);
        assert(tringBack(tree) == (void*)10L);
        assert(tringPopBack(tree) == 1);
        assert(tringEmpty(tree));
        
        assert(tringPopBack(tree) == 0);
        
        tringFree(tree);
    }
    
    TEST("tringPopBack works until tree is empty") {
        tringTree *tree = tringNew(intCompare, NULL);
        
        /* Insert values in specific order */
        tringInsert(tree, (void*)3L, NULL);
        tringInsert(tree, (void*)1L, NULL);
        tringInsert(tree, (void*)2L, NULL);
        
        /* Verify tree integrity after insertions */
        errors += verifyTreeIntegrity(tree);
        
        /* Pop in reverse insertion order: 2, 1, 3 */
        assert(tringBack(tree) == (void*)2L);
        assert(tringPopBack(tree) == 1);
        errors += verifyTreeIntegrity(tree);
        
        assert(tringBack(tree) == (void*)1L);
        assert(tringPopBack(tree) == 1);
        errors += verifyTreeIntegrity(tree);
        
        assert(tringBack(tree) == (void*)3L);
        assert(tringPopBack(tree) == 1);
        assert(tringEmpty(tree));
        assert(tringPopBack(tree) == 0);
        
        tringFree(tree);
    }
    
    TEST("tringPopBack maintains AVL properties") {
        tringTree *tree = tringNew(intCompare, NULL);
        
        /* Insert values that trigger various rotations */
        long values[] = {50, 25, 75, 10, 30, 60, 80, 5, 15, 27, 35};
        size_t n = sizeof(values) / sizeof(values[0]);
        
        for (size_t i = 0; i < n; i++) {
            assert(tringInsert(tree, (void*)values[i], NULL));
        }
        
        errors += verifyTreeIntegrity(tree);
        
        /* Pop last 3 items from back and verify balance is maintained */
        for (int i = 0; i < 3; i++) {
            int success = tringPopBack(tree);
            assert(success == 1);
            /* Verify tree integrity after each pop */
            errors += verifyTreeIntegrity(tree);
        }
        
        assert(tringSize(tree) == n - 3);
        
        /* Verify first n-3 values are still findable */
        for (size_t i = 0; i < n - 3; i++) {
            assert(tringFind(tree, (void*)values[i]) == (void*)values[i]);
        }
        
        /* Verify last 3 values were removed */
        for (size_t i = n - 3; i < n; i++) {
            assert(tringFind(tree, (void*)values[i]) == NULL);
        }
        
        tringFree(tree);
    }
    
    TEST("tringPopBack with sequential insertions") {
        tringTree *tree = tringNew(intCompare, NULL);
        
        /* Sequential insertions */
        for (long i = 1; i <= 20; i++) {
            assert(tringInsert(tree, (void*)i, NULL));
        }
        
        errors += verifyTreeIntegrity(tree);
        assert(tringSize(tree) == 20);
        
        /* Pop last 10 items from back */
        for (int i = 0; i < 10; i++) {
            assert(tringPopBack(tree) == 1);
        }
        
        /* Verify balance after pops */
        errors += verifyTreeIntegrity(tree);
        assert(tringSize(tree) == 10);
        
        /* Verify first 10 elements remain, last 10 are gone */
        for (long i = 1; i <= 10; i++) {
            assert(tringFind(tree, (void*)i) == (void*)i);
        }
        for (long i = 11; i <= 20; i++) {
            assert(tringFind(tree, (void*)i) == NULL);
        }
        
        tringFree(tree);
    }
    
    TEST("tringPopBack and tringPopFront mixed operations") {
        tringTree *tree = tringNew(intCompare, NULL);
        
        /* Insert 10 values */
        for (long i = 1; i <= 10; i++) {
            assert(tringInsert(tree, (void*)i, NULL));
        }
        
        errors += verifyTreeIntegrity(tree);
        
        /* Pop from front and back alternately */
        assert(tringFront(tree) == (void*)1L);
        assert(tringPopFront(tree) == 1);  /* Remove 1 */
        errors += verifyTreeIntegrity(tree);
        
        assert(tringBack(tree) == (void*)10L);
        assert(tringPopBack(tree) == 1);   /* Remove 10 */
        errors += verifyTreeIntegrity(tree);
        
        assert(tringFront(tree) == (void*)2L);
        assert(tringPopFront(tree) == 1);  /* Remove 2 */
        errors += verifyTreeIntegrity(tree);
        
        assert(tringBack(tree) == (void*)9L);
        assert(tringPopBack(tree) == 1);   /* Remove 9 */
        errors += verifyTreeIntegrity(tree);
        
        assert(tringSize(tree) == 6);
        
        /* Verify remaining elements: 3, 4, 5, 6, 7, 8 */
        for (long i = 3; i <= 8; i++) {
            assert(tringFind(tree, (void*)i) == (void*)i);
        }
        
        /* Verify removed elements are gone */
        assert(tringFind(tree, (void*)1L) == NULL);
        assert(tringFind(tree, (void*)2L) == NULL);
        assert(tringFind(tree, (void*)9L) == NULL);
        assert(tringFind(tree, (void*)10L) == NULL);
        
        tringFree(tree);
    }
    
    TEST("tringPopBack with ring buffer wraparound") {
        tringTree *tree = tringNew(intCompare, NULL);
        
        /* Set small max capacity to force wraparound */
        tringSetMaxCapacity(tree, 5);
        
        /* Insert 10 elements - this will cause wraparound and auto-eviction */
        for (long i = 1; i <= 10; i++) {
            assert(tringInsert(tree, (void*)i, NULL));
        }
        
        /* Should have exactly 5 elements (max_capacity) */
        assert(tringSize(tree) == 5);
        errors += verifyTreeIntegrity(tree);
        
        /* Last 5 elements (6-10) should be present */
        for (long i = 6; i <= 10; i++) {
            assert(tringFind(tree, (void*)i) == (void*)i);
        }
        
        /* tringBack should return 10 (last inserted) */
        assert(tringBack(tree) == (void*)10L);
        
        /* Pop from back */
        assert(tringPopBack(tree) == 1);
        assert(tringSize(tree) == 4);
        assert(tringFind(tree, (void*)10L) == NULL);
        errors += verifyTreeIntegrity(tree);
        
        /* tringBack should now return 9 */
        assert(tringBack(tree) == (void*)9L);
        
        tringFree(tree);
    }
    
    TEST("tringPopBack NULL parameter check") {
        /* Test NULL tree parameter */
        assert(tringPopBack(NULL) == 0);
    }
    
    TEST("alloc_size tracking with insertions") {
        size_t alloc_size = 0;
        tringTree *tree = tringNew(intCompare, &alloc_size);
        
        /* Verify initial allocation is tracked */
        assert(alloc_size > 0);
        size_t initial_size = alloc_size;
        
        /* Insert values and verify size increases */
        for (long i = 1; i <= 10; i++) {
            assert(tringInsert(tree, (void*)i, NULL));
        }
        
        /* Size should have increased after allocations */
        assert(alloc_size >= initial_size);
        
        /* Verify tree integrity */
        errors += verifyTreeIntegrity(tree);
        
        /* Free tree and verify size goes back to 0 */
        tringFree(tree);
        assert(alloc_size == 0);
    }
    
    TEST("alloc_size tracking with resize") {
        size_t alloc_size = 0;
        tringTree *tree = tringNew(intCompare, &alloc_size);
        
        size_t size_after_init = alloc_size;
        assert(size_after_init > 0);
        
        /* Insert enough elements to trigger resize (TRING_INITIAL_CAPACITY is 8) */
        for (long i = 1; i <= 20; i++) {
            assert(tringInsert(tree, (void*)i, NULL));
        }
        
        /* Size should have increased significantly after resize */
        assert(alloc_size > size_after_init);
        
        /* Verify tree integrity */
        errors += verifyTreeIntegrity(tree);
        
        tringFree(tree);
        assert(alloc_size == 0);
    }
    
    if (errors > 0) {
        printf("FAILED! %d AVL property violations found.\n", errors);
    } else {
        printf("PASSED! All 40 tests successful.\n");
    }
    return errors;
}
#endif

