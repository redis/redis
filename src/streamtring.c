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

/* Helper functions for AVL tree operations */

/* Get the height of a node. Returns 0 for TRING_NULL nodes. */
static inline uint8_t getHeight(tringTree *tree, uint32_t idx) {
    return (idx == TRING_NULL) ? 0 : tree->heights[idx];
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
    uint8_t leftHeight = getHeight(tree, tree->nodes[idx].left);
    uint8_t rightHeight = getHeight(tree, tree->nodes[idx].right);
    tree->heights[idx] = (leftHeight > rightHeight ? leftHeight : rightHeight) + 1;
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
    tree->parents[x] = tree->parents[y];
    tree->parents[y] = x;
    if (T2 != TRING_NULL) {
        tree->parents[T2] = y;
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
    tree->parents[y] = tree->parents[x];
    tree->parents[x] = y;
    if (T2 != TRING_NULL) {
        tree->parents[T2] = x;
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
    if (tree->count >= tree->capacity) {
        uint32_t newCapacity = tree->capacity * 2;
        
        /* Reallocate arrays */
        tringNode *newNodes = realloc(tree->nodes, newCapacity * sizeof(tringNode));
        if (!newNodes) return TRING_NULL;
        
        uint8_t *newHeights = realloc(tree->heights, newCapacity * sizeof(uint8_t));
        if (!newHeights) {
            free(newNodes);
            return TRING_NULL;
        }
        
        uint32_t *newParents = realloc(tree->parents, newCapacity * sizeof(uint32_t));
        if (!newParents) {
            free(newNodes);
            free(newHeights);
            return TRING_NULL;
        }
        
        tree->nodes = newNodes;
        tree->heights = newHeights;
        tree->parents = newParents;
        tree->capacity = newCapacity;
    }
    
    /* Allocate node at tail */
    uint32_t idx = tree->tail;
    tree->nodes[idx].value = value;
    tree->nodes[idx].left = TRING_NULL;
    tree->nodes[idx].right = TRING_NULL;
    tree->heights[idx] = 1;
    tree->parents[idx] = TRING_NULL;
    
    /* Update ring buffer pointers - no wrapping for tail during allocation */
    tree->tail++;
    tree->count++;
    
    return idx;
}

/* Recursive helper for insertion.
 * Returns the new root of the subtree. */
static uint32_t insertRecursive(tringTree *tree, uint32_t idx, void *value, int *inserted) {
    /* Base case: found insertion point */
    if (idx == TRING_NULL) {
        uint32_t newIdx = allocateNode(tree, value);
        if (newIdx == TRING_NULL) {
            *inserted = 0;
            return TRING_NULL;
        }
        *inserted = 1;
        return newIdx;
    }
    
    /* Compare and recurse */
    int cmp = tree->compare(value, tree->nodes[idx].value);
    
    if (cmp < 0) {
        uint32_t newLeft = insertRecursive(tree, tree->nodes[idx].left, value, inserted);
        if (!(*inserted)) return idx;
        tree->nodes[idx].left = newLeft;
        if (newLeft != TRING_NULL) {
            tree->parents[newLeft] = idx;
        }
    } else if (cmp > 0) {
        uint32_t newRight = insertRecursive(tree, tree->nodes[idx].right, value, inserted);
        if (!(*inserted)) return idx;
        tree->nodes[idx].right = newRight;
        if (newRight != TRING_NULL) {
            tree->parents[newRight] = idx;
        }
    } else {
        /* Duplicate value - don't insert */
        *inserted = 0;
        return idx;
    }
    
    /* Rebalance and return */
    return rebalance(tree, idx);
}

/* Public API implementation */

tringTree *tringNew(tringCompareFunc compare) {
    if (!compare) return NULL;
    
    tringTree *tree = malloc(sizeof(tringTree));
    if (!tree) return NULL;
    
    tree->nodes = malloc(TRING_INITIAL_CAPACITY * sizeof(tringNode));
    if (!tree->nodes) {
        free(tree);
        return NULL;
    }
    
    tree->heights = malloc(TRING_INITIAL_CAPACITY * sizeof(uint8_t));
    if (!tree->heights) {
        free(tree->nodes);
        free(tree);
        return NULL;
    }
    
    tree->parents = malloc(TRING_INITIAL_CAPACITY * sizeof(uint32_t));
    if (!tree->parents) {
        free(tree->heights);
        free(tree->nodes);
        free(tree);
        return NULL;
    }
    
    tree->capacity = TRING_INITIAL_CAPACITY;
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
    
    /* Optionally call free callback for all values */
    if (tree->free_callback) {
        for (uint32_t idx = tree->head; idx < tree->tail; idx++) {
            if (tree->nodes[idx].value) {
                tree->free_callback(tree->nodes[idx].value);
            }
        }
    }
    
    free(tree->parents);
    free(tree->heights);
    free(tree->nodes);
    free(tree);
}

int tringInsert(tringTree *tree, void *value) {
    if (!tree || !value) return 0;
    
    int inserted = 0;
    uint32_t newRoot = insertRecursive(tree, tree->root, value, &inserted);
    
    if (inserted) {
        tree->root = newRoot;
        if (newRoot != TRING_NULL) {
            tree->parents[newRoot] = TRING_NULL;
        }
    }
    
    return inserted;
}

void *tringFind(tringTree *tree, void *value) {
    if (!tree || !value) return NULL;
    
    uint32_t current = tree->root;
    
    while (current != TRING_NULL) {
        int cmp = tree->compare(value, tree->nodes[current].value);
        
        if (cmp == 0) {
            return tree->nodes[current].value;
        } else if (cmp < 0) {
            current = tree->nodes[current].left;
        } else {
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

/* Helper function to remove a node from the tree.
 * Returns the new root of the subtree. */
static uint32_t removeNode(tringTree *tree, uint32_t idx, void *value, int *removed, void **removedValue) {
    if (idx == TRING_NULL) {
        *removed = 0;
        return TRING_NULL;
    }
    
    int cmp = tree->compare(value, tree->nodes[idx].value);
    
    if (cmp < 0) {
        /* Value is in left subtree */
        uint32_t newLeft = removeNode(tree, tree->nodes[idx].left, value, removed, removedValue);
        if (*removed) {
            tree->nodes[idx].left = newLeft;
            if (newLeft != TRING_NULL) {
                tree->parents[newLeft] = idx;
            }
        }
    } else if (cmp > 0) {
        /* Value is in right subtree */
        uint32_t newRight = removeNode(tree, tree->nodes[idx].right, value, removed, removedValue);
        if (*removed) {
            tree->nodes[idx].right = newRight;
            if (newRight != TRING_NULL) {
                tree->parents[newRight] = idx;
            }
        }
    } else {
        /* Found the node to remove */
        *removed = 1;
        *removedValue = tree->nodes[idx].value;
        
        /* Case 1: Node with only one child or no child */
        if (tree->nodes[idx].left == TRING_NULL) {
            uint32_t temp = tree->nodes[idx].right;
            if (temp != TRING_NULL) {
                tree->parents[temp] = tree->parents[idx];
            }
            return temp;
        } else if (tree->nodes[idx].right == TRING_NULL) {
            uint32_t temp = tree->nodes[idx].left;
            if (temp != TRING_NULL) {
                tree->parents[temp] = tree->parents[idx];
            }
            return temp;
        }
        
        /* Case 2: Node with two children */
        /* Get the inorder successor (smallest in the right subtree) */
        uint32_t successor = findMin(tree, tree->nodes[idx].right);
        
        /* Copy the successor's value to this node */
        tree->nodes[idx].value = tree->nodes[successor].value;
        
        /* Delete the successor */
        int successorRemoved = 0;
        void *dummyValue = NULL;
        tree->nodes[idx].right = removeNode(tree, tree->nodes[idx].right, 
                                            tree->nodes[successor].value, 
                                            &successorRemoved, &dummyValue);
        if (tree->nodes[idx].right != TRING_NULL) {
            tree->parents[tree->nodes[idx].right] = idx;
        }
    }
    
    if (!(*removed)) return idx;
    
    /* Rebalance and return */
    return rebalance(tree, idx);
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

void *tringPop(tringTree *tree) {
    if (!tree || tree->count == 0) return NULL;
    
    /* Get the item pointed to by head */
    void *headValue = tree->nodes[tree->head].value;
    
    /* Remove that item from the AVL tree */
    int removed = 0;
    void *removedValue = NULL;
    uint32_t newRoot = removeNode(tree, tree->root, headValue, &removed, &removedValue);
    
    if (removed) {
        tree->root = newRoot;
        if (newRoot != TRING_NULL) {
            tree->parents[newRoot] = TRING_NULL;
        }
        
        /* Update ring buffer: increment head and decrement count */
        tree->head++;
        tree->count--;
        
        return removedValue;
    }
    
    return NULL;
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
    if (tree->heights[nodeIdx] != expectedHeight) {
        printf("ERROR: Node at index %u has wrong height: expected %d, got %u\n", 
               nodeIdx, expectedHeight, tree->heights[nodeIdx]);
        errors++;
    }
    
    *heightOut = expectedHeight;
    return errors;
}

int tringTest(int argc, char *argv[], int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    
    int errors = 0;
    
    TEST("tringNew creates empty tree") {
        tringTree *tree = tringNew(intCompare);
        assert(tree != NULL);
        assert(tringEmpty(tree));
        assert(tringSize(tree) == 0);
        tringFree(tree);
    }
    
    TEST("tringInsert adds values") {
        tringTree *tree = tringNew(intCompare);
        assert(tringInsert(tree, (void*)10L));
        assert(tringInsert(tree, (void*)20L));
        assert(tringInsert(tree, (void*)5L));
        assert(tringSize(tree) == 3);
        assert(!tringEmpty(tree));
        
        /* Verify AVL properties */
        int height;
        errors += verifyAVLProperties(tree, tree->root, &height);
        
        tringFree(tree);
    }
    
    TEST("tringInsert rejects duplicates") {
        tringTree *tree = tringNew(intCompare);
        tringInsert(tree, (void*)10L);
        assert(!tringInsert(tree, (void*)10L));
        assert(tringSize(tree) == 1);
        tringFree(tree);
    }
    
    TEST("tringFind locates values") {
        tringTree *tree = tringNew(intCompare);
        tringInsert(tree, (void*)10L);
        tringInsert(tree, (void*)20L);
        tringInsert(tree, (void*)5L);
        assert(tringFind(tree, (void*)10L) == (void*)10L);
        assert(tringFind(tree, (void*)20L) == (void*)20L);
        assert(tringFind(tree, (void*)5L) == (void*)5L);
        assert(tringFind(tree, (void*)99L) == NULL);
        
        /* Verify AVL properties */
        int height;
        errors += verifyAVLProperties(tree, tree->root, &height);
        
        tringFree(tree);
    }
    
    TEST("tringInsert maintains balance with many insertions") {
        tringTree *tree = tringNew(intCompare);
        /* Insert values that would create an unbalanced tree without rotations */
        for (long i = 1; i <= 100; i++) {
            assert(tringInsert(tree, (void*)i));
        }
        assert(tringSize(tree) == 100);
        
        /* Verify AVL properties after all insertions */
        int height;
        errors += verifyAVLProperties(tree, tree->root, &height);
        
        /* Verify all values are findable */
        for (long i = 1; i <= 100; i++) {
            assert(tringFind(tree, (void*)i) == (void*)i);
        }
        tringFree(tree);
    }
    
    TEST("tringSize and tringEmpty work correctly") {
        tringTree *tree = tringNew(intCompare);
        assert(tringEmpty(tree));
        tringInsert(tree, (void*)1L);
        assert(!tringEmpty(tree));
        assert(tringSize(tree) == 1);
        tringInsert(tree, (void*)2L);
        assert(tringSize(tree) == 2);
        tringFree(tree);
    }
    
    TEST("tringFront returns head value (first inserted)") {
        tringTree *tree = tringNew(intCompare);
        tringInsert(tree, (void*)50L);
        tringInsert(tree, (void*)30L);
        tringInsert(tree, (void*)70L);
        tringInsert(tree, (void*)20L);
        tringInsert(tree, (void*)40L);
        /* tringFront should return the first inserted value (50), not minimum */
        assert(tringFront(tree) == (void*)50L);
        
        /* After popping, tringFront should return the next value */
        tringPop(tree);
        assert(tringFront(tree) == (void*)30L);
        
        tringFree(tree);
    }
    
    TEST("tringBack returns last added value") {
        tringTree *tree = tringNew(intCompare);
        tringInsert(tree, (void*)50L);
        tringInsert(tree, (void*)30L);
        tringInsert(tree, (void*)70L);
        tringInsert(tree, (void*)20L);
        tringInsert(tree, (void*)80L);
        /* tringBack should return the last inserted value (80), which happens to also be max */
        assert(tringBack(tree) == (void*)80L);
        
        /* Insert a smaller value - tringBack should return it (not the max) */
        tringInsert(tree, (void*)10L);
        assert(tringBack(tree) == (void*)10L);
        
        tringFree(tree);
    }
    
    TEST("tringPop removes items in FIFO order") {
        tringTree *tree = tringNew(intCompare);
        tringInsert(tree, (void*)50L);
        tringInsert(tree, (void*)30L);
        tringInsert(tree, (void*)70L);
        tringInsert(tree, (void*)20L);
        tringInsert(tree, (void*)40L);
        
        /* Verify AVL properties after insertions */
        int height;
        errors += verifyAVLProperties(tree, tree->root, &height);
        
        /* Pop should remove in insertion order (FIFO) */
        assert(tringPop(tree) == (void*)50L);
        assert(tringSize(tree) == 4);
        assert(tringFind(tree, (void*)50L) == NULL);
        
        /* Verify AVL properties after first pop */
        errors += verifyAVLProperties(tree, tree->root, &height);
        
        assert(tringPop(tree) == (void*)30L);
        assert(tringSize(tree) == 3);
        assert(tringFind(tree, (void*)30L) == NULL);
        
        /* Verify AVL properties after second pop */
        errors += verifyAVLProperties(tree, tree->root, &height);
        
        tringFree(tree);
    }
    
    TEST("tringFront/Back return NULL on empty tree") {
        tringTree *tree = tringNew(intCompare);
        assert(tringFront(tree) == NULL);
        assert(tringBack(tree) == NULL);
        assert(tringPop(tree) == NULL);
        tringFree(tree);
    }
    
    TEST("tringPop works until tree is empty") {
        tringTree *tree = tringNew(intCompare);
        tringInsert(tree, (void*)3L);
        tringInsert(tree, (void*)1L);
        tringInsert(tree, (void*)2L);
        
        /* Verify AVL properties after insertions */
        int height;
        errors += verifyAVLProperties(tree, tree->root, &height);
        
        /* Pop in insertion order: 3, 1, 2 */
        assert(tringPop(tree) == (void*)3L);
        errors += verifyAVLProperties(tree, tree->root, &height);
        
        assert(tringPop(tree) == (void*)1L);
        errors += verifyAVLProperties(tree, tree->root, &height);
        
        assert(tringPop(tree) == (void*)2L);
        assert(tringEmpty(tree));
        assert(tringPop(tree) == NULL);
        
        tringFree(tree);
    }
    
    TEST("AVL properties maintained with mixed operations") {
        tringTree *tree = tringNew(intCompare);
        int height;
        
        /* Insert values that trigger various rotations */
        long values[] = {50, 25, 75, 10, 30, 60, 80, 5, 15, 27, 35};
        size_t n = sizeof(values) / sizeof(values[0]);
        
        for (size_t i = 0; i < n; i++) {
            assert(tringInsert(tree, (void*)values[i]));
            /* Verify AVL properties after each insertion */
            errors += verifyAVLProperties(tree, tree->root, &height);
        }
        
        assert(tringSize(tree) == n);
        
        /* Pop some items and verify balance is maintained */
        for (int i = 0; i < 3; i++) {
            void *popped = tringPop(tree);
            assert(popped != NULL);
            /* Verify AVL properties after each pop */
            errors += verifyAVLProperties(tree, tree->root, &height);
        }
        
        assert(tringSize(tree) == n - 3);
        
        /* Verify remaining values are still findable */
        for (size_t i = 3; i < n; i++) {
            assert(tringFind(tree, (void*)values[i]) == (void*)values[i]);
        }
        
        tringFree(tree);
    }
    
    TEST("AVL properties with sequential insertions") {
        tringTree *tree = tringNew(intCompare);
        int height;
        
        /* Sequential insertions should trigger rotations */
        for (long i = 1; i <= 50; i++) {
            assert(tringInsert(tree, (void*)i));
            /* Check balance every 10 insertions */
            if (i % 10 == 0) {
                errors += verifyAVLProperties(tree, tree->root, &height);
            }
        }
        
        /* Final verification */
        errors += verifyAVLProperties(tree, tree->root, &height);
        assert(tringSize(tree) == 50);
        
        /* Pop half the items */
        for (int i = 0; i < 25; i++) {
            assert(tringPop(tree) != NULL);
        }
        
        /* Verify balance after pops */
        errors += verifyAVLProperties(tree, tree->root, &height);
        assert(tringSize(tree) == 25);
        
        tringFree(tree);
    }
    
    TEST("NULL parameter checks") {
        /* Test NULL tree parameter */
        assert(tringSize(NULL) == 0);
        assert(tringEmpty(NULL) == 1);
        assert(tringFind(NULL, (void*)1L) == NULL);
        assert(tringInsert(NULL, (void*)1L) == 0);
        assert(tringFront(NULL) == NULL);
        assert(tringBack(NULL) == NULL);
        assert(tringPop(NULL) == NULL);
        tringFree(NULL);  /* Should not crash */
        
        /* Test NULL compare function */
        assert(tringNew(NULL) == NULL);
        
        /* Test NULL value parameter */
        tringTree *tree = tringNew(intCompare);
        assert(tringInsert(tree, NULL) == 0);
        assert(tringFind(tree, NULL) == NULL);
        tringFree(tree);
    }
    
    TEST("Right-Left rotation case") {
        tringTree *tree = tringNew(intCompare);
        int height;
        
        /* Insert values to trigger Right-Left case:
         * Insert 10, 30, 20 - this creates right-heavy then needs right-left rotation */
        tringInsert(tree, (void*)10L);
        tringInsert(tree, (void*)30L);
        tringInsert(tree, (void*)20L);
        
        errors += verifyAVLProperties(tree, tree->root, &height);
        
        /* Another Right-Left case pattern */
        tringInsert(tree, (void*)50L);
        tringInsert(tree, (void*)70L);
        tringInsert(tree, (void*)60L);
        
        errors += verifyAVLProperties(tree, tree->root, &height);
        
        tringFree(tree);
    }
    
    TEST("Node removal with only left child") {
        tringTree *tree = tringNew(intCompare);
        int height;
        
        /* Create a structure where we remove a node with only left child */
        tringInsert(tree, (void*)50L);
        tringInsert(tree, (void*)30L);
        tringInsert(tree, (void*)20L);  /* Left child of 30 */
        
        /* Now pop 50 (head), then pop 30 which will have only left child (20) */
        tringPop(tree);  /* Remove 50 */
        tringPop(tree);  /* Remove 30, which has only left child 20 */
        
        errors += verifyAVLProperties(tree, tree->root, &height);
        assert(tringSize(tree) == 1);
        assert(tringFind(tree, (void*)20L) == (void*)20L);
        
        tringFree(tree);
    }
    
    TEST("Free callback functionality") {
        tringTree *tree = tringNew(intCompare);
        
        /* Set up callback */
        tree->free_callback = testFreeCallback;
        freeCallbackCount = 0;
        
        /* Insert some values */
        tringInsert(tree, (void*)10L);
        tringInsert(tree, (void*)20L);
        tringInsert(tree, (void*)30L);
        
        /* Free the tree - callback should be called for each value */
        tringFree(tree);
        
        /* Verify callback was called for all 3 values */
        assert(freeCallbackCount == 3);
    }
    
    TEST("tringSize returns correct count") {
        tringTree *tree = tringNew(intCompare);
        
        /* Verify size on non-NULL tree with 0 elements */
        size_t size = tringSize(tree);
        assert(size == 0);
        
        /* Add elements and check size */
        tringInsert(tree, (void*)1L);
        size = tringSize(tree);
        assert(size == 1);
        
        tringInsert(tree, (void*)2L);
        size = tringSize(tree);
        assert(size == 2);
        
        tringInsert(tree, (void*)3L);
        size = tringSize(tree);
        assert(size == 3);
        
        /* Pop and verify size decreases */
        tringPop(tree);
        size = tringSize(tree);
        assert(size == 2);
        
        tringPop(tree);
        size = tringSize(tree);
        assert(size == 1);
        
        tringPop(tree);
        size = tringSize(tree);
        assert(size == 0);
        
        tringFree(tree);
    }
    
    if (errors > 0) {
        printf("FAILED! %d AVL property violations found.\n", errors);
    } else {
        printf("PASSED! All 19 tests successful.\n");
    }
    return errors;
}
#endif

