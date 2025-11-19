/* AVL -- Array-based AVL tree implementation.
 *
 * Copyright (c) 2024-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include "avl.h"
#include <stdlib.h>
#include <string.h>

/* ------------------------- Helper functions ------------------------------ */

/* Return the maximum of two integers. */
static int avlMax(int a, int b) {
    return (a > b) ? a : b;
}

/* Get height of a node. Returns 0 for null nodes. */
static uint8_t avlGetHeight(avlTree *tree, uint32_t nodeIdx) {
    if (nodeIdx == AVL_NULL) return 0;
    return tree->heights[nodeIdx];
}

/* Get balance factor of a node. */
static int avlGetBalance(avlTree *tree, uint32_t nodeIdx) {
    if (nodeIdx == AVL_NULL) return 0;
    return (int)avlGetHeight(tree, tree->nodes[nodeIdx].left) - 
           (int)avlGetHeight(tree, tree->nodes[nodeIdx].right);
}

/* Update height of a node based on its children heights. */
static void avlUpdateHeight(avlTree *tree, uint32_t nodeIdx) {
    if (nodeIdx == AVL_NULL) return;
    uint8_t leftHeight = avlGetHeight(tree, tree->nodes[nodeIdx].left);
    uint8_t rightHeight = avlGetHeight(tree, tree->nodes[nodeIdx].right);
    tree->heights[nodeIdx] = 1 + avlMax(leftHeight, rightHeight);
}

/* ------------------------- AVL rotations --------------------------------- */

/* Perform left rotation on node x. Returns the new root of the subtree. */
static uint32_t avlRotateLeft(avlTree *tree, uint32_t x) {
    uint32_t y = tree->nodes[x].right;
    uint32_t T2 = tree->nodes[y].left;
    uint32_t xParent = tree->parents[x];
    
    /* Perform rotation. */
    tree->nodes[y].left = x;
    tree->nodes[x].right = T2;
    
    /* Update parents. */
    tree->parents[y] = xParent;
    tree->parents[x] = y;
    if (T2 != AVL_NULL) tree->parents[T2] = x;
    
    /* Update heights. */
    avlUpdateHeight(tree, x);
    avlUpdateHeight(tree, y);
    
    return y;
}

/* Perform right rotation on node y. Returns the new root of the subtree. */
static uint32_t avlRotateRight(avlTree *tree, uint32_t y) {
    uint32_t x = tree->nodes[y].left;
    uint32_t T2 = tree->nodes[x].right;
    uint32_t yParent = tree->parents[y];
    
    /* Perform rotation. */
    tree->nodes[x].right = y;
    tree->nodes[y].left = T2;
    
    /* Update parents. */
    tree->parents[x] = yParent;
    tree->parents[y] = x;
    if (T2 != AVL_NULL) tree->parents[T2] = y;
    
    /* Update heights. */
    avlUpdateHeight(tree, y);
    avlUpdateHeight(tree, x);
    
    return x;
}

/* Balance a node and return new root of subtree. Handles all four cases:
 * Left-Left, Left-Right, Right-Right, Right-Left. */
static uint32_t avlBalance(avlTree *tree, uint32_t nodeIdx) {
    if (nodeIdx == AVL_NULL) return AVL_NULL;
    
    avlUpdateHeight(tree, nodeIdx);
    int balance = avlGetBalance(tree, nodeIdx);
    
    /* Left-Left case. */
    if (balance > 1 && avlGetBalance(tree, tree->nodes[nodeIdx].left) >= 0) {
        return avlRotateRight(tree, nodeIdx);
    }
    
    /* Left-Right case. */
    if (balance > 1 && avlGetBalance(tree, tree->nodes[nodeIdx].left) < 0) {
        tree->nodes[nodeIdx].left = avlRotateLeft(tree, tree->nodes[nodeIdx].left);
        return avlRotateRight(tree, nodeIdx);
    }
    
    /* Right-Right case. */
    if (balance < -1 && avlGetBalance(tree, tree->nodes[nodeIdx].right) <= 0) {
        return avlRotateLeft(tree, nodeIdx);
    }
    
    /* Right-Left case. */
    if (balance < -1 && avlGetBalance(tree, tree->nodes[nodeIdx].right) > 0) {
        tree->nodes[nodeIdx].right = avlRotateRight(tree, tree->nodes[nodeIdx].right);
        return avlRotateLeft(tree, nodeIdx);
    }
    
    return nodeIdx;
}

/* ----------------------- Memory management ------------------------------ */

/* Allocate a node from free list or new space. Returns the index of the
 * allocated node. */
static uint32_t avlAllocateNode(avlTree * tree) {
    uint32_t nodeIdx;
    
    /* Try to use a node from the free list first. */
    if (tree->firstFree != AVL_NULL) {
        nodeIdx = tree->firstFree;
        tree->firstFree = tree->nodes[nodeIdx].left; /* Next in free list. */
    } else {
        /* Use a new index. */
        nodeIdx = tree->size;
    }
    
    /* Initialize the node. */
    tree->nodes[nodeIdx].value = NULL;
    tree->nodes[nodeIdx].left = AVL_NULL;
    tree->nodes[nodeIdx].right = AVL_NULL;
    tree->heights[nodeIdx] = 1;
    tree->parents[nodeIdx] = AVL_NULL;
    
    tree->size++;
    return nodeIdx;
}

/* Free a node by adding it to the free list. */
static void avlFreeNode(avlTree * tree, uint32_t nodeIdx) {
    tree->nodes[nodeIdx].left = tree->firstFree;
    tree->firstFree = nodeIdx;
    tree->size--;
}

/* ---------------- Insertion and deletion operations --------------------- */

/* Insert a value into subtree rooted at nodeIdx. Returns the new root
 * of the subtree. Duplicates are not allowed. */
static uint32_t avlInsertNode(avlTree * tree, uint32_t nodeIdx, void * value) {
    /* Base case: found insertion point. */
    if (nodeIdx == AVL_NULL) {
        uint32_t newIdx = avlAllocateNode(tree);
        tree->nodes[newIdx].value = value;
        return newIdx;
    }
    
    int cmp = tree->compare(value, tree->nodes[nodeIdx].value);
    
    /* Duplicate values not allowed. */
    if (cmp == 0) return nodeIdx;
    
    /* Recursive insertion. */
    if (cmp < 0) {
        uint32_t newLeft = avlInsertNode(tree, tree->nodes[nodeIdx].left, value);
        tree->nodes[nodeIdx].left = newLeft;
        tree->parents[newLeft] = nodeIdx;
    } else {
        uint32_t newRight = avlInsertNode(tree, tree->nodes[nodeIdx].right, value);
        tree->nodes[nodeIdx].right = newRight;
        tree->parents[newRight] = nodeIdx;
    }
    
    /* Balance and return new root of this subtree. */
    return avlBalance(tree, nodeIdx);
}

/* Find minimum value node in subtree rooted at nodeIdx. */
static uint32_t avlFindMin(avlTree * tree, uint32_t nodeIdx) {
    while (tree->nodes[nodeIdx].left != AVL_NULL) {
        nodeIdx = tree->nodes[nodeIdx].left;
    }
    return nodeIdx;
}

/* Remove a value from subtree rooted at nodeIdx. Sets *found to 1 if
 * the value was found and removed. Returns the new root of the subtree. */
static uint32_t avlRemoveNode(avlTree * tree, uint32_t nodeIdx, void * value, int * found) {
    if (nodeIdx == AVL_NULL) {
        *found = 0;
        return AVL_NULL;
    }
    
    int cmp = tree->compare(value, tree->nodes[nodeIdx].value);
    
    if (cmp < 0) {
        uint32_t newLeft = avlRemoveNode(tree, tree->nodes[nodeIdx].left, value, found);
        tree->nodes[nodeIdx].left = newLeft;
        if (newLeft != AVL_NULL) tree->parents[newLeft] = nodeIdx;
    } else if (cmp > 0) {
        uint32_t newRight = avlRemoveNode(tree, tree->nodes[nodeIdx].right, value, found);
        tree->nodes[nodeIdx].right = newRight;
        if (newRight != AVL_NULL) tree->parents[newRight] = nodeIdx;
    } else {
        /* Found the node to delete. */
        *found = 1;
        
        /* Node with only one child or no child. */
        if (tree->nodes[nodeIdx].left == AVL_NULL || 
            tree->nodes[nodeIdx].right == AVL_NULL) {
            uint32_t temp = (tree->nodes[nodeIdx].left != AVL_NULL) ? 
                            tree->nodes[nodeIdx].left : tree->nodes[nodeIdx].right;
            
            if (temp != AVL_NULL) tree->parents[temp] = tree->parents[nodeIdx];
            
            /* Call the free callback if set. */
            if (tree->free_callback != NULL) {
                tree->free_callback(tree->nodes[nodeIdx].value);
            }
            
            avlFreeNode(tree, nodeIdx);
            return temp;
        } else {
            /* Node with two children: get in-order successor. */
            uint32_t successor = avlFindMin(tree, tree->nodes[nodeIdx].right);
            
            /* Call the free callback if set, on the original value. */
            if (tree->free_callback != NULL) {
                tree->free_callback(tree->nodes[nodeIdx].value);
            }
            
            /* Copy successor's value to this node. */
            tree->nodes[nodeIdx].value = tree->nodes[successor].value;
            
            /* Delete the successor (don't call callback again). */
            void (*saved_callback)(void*) = tree->free_callback;
            tree->free_callback = NULL;
            
            int dummyFound;
            uint32_t newRight = avlRemoveNode(tree, tree->nodes[nodeIdx].right, 
                                              tree->nodes[successor].value, &dummyFound);
            tree->nodes[nodeIdx].right = newRight;
            if (newRight != AVL_NULL) tree->parents[newRight] = nodeIdx;
            
            tree->free_callback = saved_callback;
        }
    }
    
    if (!(*found)) return nodeIdx;
    
    /* Balance and return new root of this subtree. */
    return avlBalance(tree, nodeIdx);
}

/* Check if a node index is in the free list. */
static int avlIsInFreeList(avlTree * tree, uint32_t nodeIdx) {
    uint32_t current = tree->firstFree;
    while (current != AVL_NULL) {
        if (current == nodeIdx) return 1;
        current = tree->nodes[current].left;
    }
    return 0;
}

/* Remove a node from the free list. */
static void avlRemoveFromFreeList(avlTree * tree, uint32_t nodeIdx) {
    if (tree->firstFree == nodeIdx) {
        tree->firstFree = tree->nodes[nodeIdx].left;
        return;
    }
    
    uint32_t current = tree->firstFree;
    while (current != AVL_NULL && tree->nodes[current].left != nodeIdx) {
        current = tree->nodes[current].left;
    }
    
    if (current != AVL_NULL) {
        tree->nodes[current].left = tree->nodes[nodeIdx].left;
    }
}

/* Find the highest index of any active node (not in free list). Uses a
 * stack-based traversal to find all used indices. */
static uint32_t avlFindMaxActiveIndex(avlTree * tree) {
    uint32_t maxIdx = 0;
    int found = 0;
    
    if (tree->root == AVL_NULL) return AVL_NULL;
    
    /* Use a stack-based approach to avoid recursion. */
    uint32_t stack[1000]; /* Sufficient for most trees. */
    uint32_t stackTop = 0;
    stack[stackTop++] = tree->root;
    
    while (stackTop > 0) {
        uint32_t nodeIdx = stack[--stackTop];
        if (!found || nodeIdx > maxIdx) {
            maxIdx = nodeIdx;
            found = 1;
        }
        
        if (tree->nodes[nodeIdx].left != AVL_NULL) {
            stack[stackTop++] = tree->nodes[nodeIdx].left;
        }
        if (tree->nodes[nodeIdx].right != AVL_NULL) {
            stack[stackTop++] = tree->nodes[nodeIdx].right;
        }
    }
    
    return found ? maxIdx : AVL_NULL;
}

/* -------------------- Dynamic array resizing ---------------------------- */

/* Resize the node arrays to newCapacity. When shrinking, moves nodes beyond
 * newCapacity to lower indices and rebuilds the free list. Returns 1 on
 * success, 0 on memory allocation failure. */
static int avlResize(avlTree * tree, uint32_t newCapacity) {
    if (newCapacity < INITIAL_CAPACITY) {
        newCapacity = INITIAL_CAPACITY;
    }
    
    /* When shrinking, check if any active nodes are beyond the new capacity. */
    if (newCapacity < tree->capacity) {
        uint32_t maxActiveIdx = avlFindMaxActiveIndex(tree);
        
        /* If there are active nodes beyond new capacity, we need to move them. */
        if (maxActiveIdx != AVL_NULL && maxActiveIdx >= newCapacity) {
            /* Create a mapping to track moved nodes: oldIndex -> newIndex. */
            uint32_t *indexMap = (uint32_t *)malloc(tree->capacity * sizeof(uint32_t));
            if (!indexMap) return 0;
            
            /* Initialize mapping (identity mapping). */
            for (uint32_t i = 0; i < tree->capacity; i++) {
                indexMap[i] = i;
            }
            
            /* Find and move nodes beyond new capacity. */
            for (uint32_t i = newCapacity; i < tree->capacity; i++) {
                /* Skip if this node is in the free list (not in use). */
                if (avlIsInFreeList(tree, i)) continue;
                
                /* This node is in use, find a free slot to move it to. */
                uint32_t newIdx = AVL_NULL;
                
                /* Look for a free slot in the lower part. */
                for (uint32_t j = 0; j < newCapacity; j++) {
                    if (avlIsInFreeList(tree, j)) {
                        newIdx = j;
                        avlRemoveFromFreeList(tree, j);
                        break;
                    }
                }
                
                /* If we found a free slot, move the node. */
                if (newIdx != AVL_NULL) {
                    /* Copy node data. */
                    tree->nodes[newIdx] = tree->nodes[i];
                    tree->heights[newIdx] = tree->heights[i];
                    
                    /* Map parent index (in case parent was moved). */
                    uint32_t parentIdx = tree->parents[i];
                    if (parentIdx != AVL_NULL && parentIdx < tree->capacity) {
                        parentIdx = indexMap[parentIdx];
                    }
                    tree->parents[newIdx] = parentIdx;
                    
                    /* Update mapping. */
                    indexMap[i] = newIdx;
                    
                    /* Update parent's reference using parent pointer. */
                    if (parentIdx == AVL_NULL) {
                        /* This is the root. */
                        tree->root = newIdx;
                    } else {
                        /* Update parent's child pointer. */
                        if (tree->nodes[parentIdx].left == i) {
                            tree->nodes[parentIdx].left = newIdx;
                        } else if (tree->nodes[parentIdx].right == i) {
                            tree->nodes[parentIdx].right = newIdx;
                        }
                    }
                    
                    /* Map and update children's parent pointers. */
                    uint32_t leftIdx = tree->nodes[newIdx].left;
                    if (leftIdx != AVL_NULL && leftIdx < tree->capacity) {
                        leftIdx = indexMap[leftIdx];
                        tree->nodes[newIdx].left = leftIdx;
                        tree->parents[leftIdx] = newIdx;
                    }
                    
                    uint32_t rightIdx = tree->nodes[newIdx].right;
                    if (rightIdx != AVL_NULL && rightIdx < tree->capacity) {
                        rightIdx = indexMap[rightIdx];
                        tree->nodes[newIdx].right = rightIdx;
                        tree->parents[rightIdx] = newIdx;
                    }
                } else {
                    /* No free slot found, can't shrink. */
                    free(indexMap);
                    return 0;
                }
            }
            
            free(indexMap);
        }
        
        /* Rebuild free list to only include nodes < newCapacity. */
        uint32_t newFreeList = AVL_NULL;
        uint32_t current = tree->firstFree;
        while (current != AVL_NULL && current < tree->capacity) {
            uint32_t next = tree->nodes[current].left;
            if (current < newCapacity) {
                /* Add to new free list. */
                tree->nodes[current].left = newFreeList;
                newFreeList = current;
            }
            current = next;
        }
        tree->firstFree = newFreeList;
    }
    
    /* Allocate new arrays. */
    avlNode *newNodes = (avlNode *)realloc(tree->nodes, newCapacity * sizeof(avlNode));
    if (!newNodes) return 0;
    
    uint8_t *newHeights = (uint8_t *)realloc(tree->heights, newCapacity * sizeof(uint8_t));
    if (!newHeights) {
        tree->nodes = newNodes;
        return 0;
    }
    
    uint32_t *newParents = (uint32_t *)realloc(tree->parents, newCapacity * sizeof(uint32_t));
    if (!newParents) {
        tree->nodes = newNodes;
        tree->heights = newHeights;
        return 0;
    }
    
    tree->nodes = newNodes;
    tree->heights = newHeights;
    tree->parents = newParents;
    tree->capacity = newCapacity;
    
    return 1;
}

/* Grow the tree capacity by doubling it. */
static int avlGrow(avlTree * tree) {
    return avlResize(tree, tree->capacity * 2);
}

/* Shrink the tree capacity by halving it if size allows. */
static int avlShrink(avlTree * tree) {
    uint32_t newCapacity = tree->capacity / 2;
    if (newCapacity < INITIAL_CAPACITY) {
        newCapacity = INITIAL_CAPACITY;
    }
    if (newCapacity >= tree->capacity) {
        return 1; /* No need to shrink. */
    }
    
    /* Make sure we have enough space for all active nodes. */
    if (tree->size > newCapacity) {
        return 1; /* Can't shrink, not enough space. */
    }
    
    return avlResize(tree, newCapacity);
}

/* ------------------------- AVL tree API ---------------------------------- */

/* Create a new AVL tree with the given comparison function. */
avlTree * avlNew(avlCompareFunc compare) {
    if (!compare) return NULL;
    
    avlTree * tree = (avlTree *)malloc(sizeof(avlTree));
    if (!tree) return NULL;
    
    tree->capacity = INITIAL_CAPACITY;
    tree->size = 0;
    tree->root = AVL_NULL;
    tree->firstFree = AVL_NULL;
    tree->compare = compare;
    tree->free_callback = NULL;
    
    tree->nodes = (avlNode *)malloc(INITIAL_CAPACITY * sizeof(avlNode));
    tree->heights = (uint8_t *)malloc(INITIAL_CAPACITY * sizeof(uint8_t));
    tree->parents = (uint32_t *)malloc(INITIAL_CAPACITY * sizeof(uint32_t));
    
    if (!tree->nodes || !tree->heights || !tree->parents) {
        free(tree->nodes);
        free(tree->heights);
        free(tree->parents);
        free(tree);
        return NULL;
    }
    
    return tree;
}

/* Free an AVL tree and all its associated memory. */
void avlFree(avlTree * tree) {
    if (!tree) return;
    
    free(tree->nodes);
    free(tree->heights);
    free(tree->parents);
    free(tree);
}

/* Helper function to traverse tree and call callback on all values. */
static void avlFreeValuesHelper(avlTree * tree, uint32_t nodeIdx, void (*free_callback)(void*)) {
    if (nodeIdx == AVL_NULL) return;
    
    /* Recursively free left and right subtrees. */
    avlFreeValuesHelper(tree, tree->nodes[nodeIdx].left, free_callback);
    avlFreeValuesHelper(tree, tree->nodes[nodeIdx].right, free_callback);
    
    /* Free the value at this node. */
    if (tree->nodes[nodeIdx].value != NULL) {
        free_callback(tree->nodes[nodeIdx].value);
    }
}

/* Free the AVL tree and call the provided callback for each value.
 * This is useful when you want to free both the tree and its contents. */
void avlFreeWithCallback(avlTree * tree, void (*free_callback)(void*)) {
    if (!tree) return;
    
    if (free_callback != NULL && tree->root != AVL_NULL) {
        avlFreeValuesHelper(tree, tree->root, free_callback);
    }
    
    free(tree->nodes);
    free(tree->heights);
    free(tree->parents);
    free(tree);
}

/* Set a callback function to be called when a value is removed from the tree. */
void avlSetFreeCallback(avlTree * tree, void (*callback)(void*)) {
    if (!tree) return;
    tree->free_callback = callback;
}

/* Insert a value into the tree. The tree is automatically grown if needed.
 * Returns 1 if a new node was inserted, 0 if the value already exists
 * or on memory allocation failure. Duplicate values are not allowed. */
int avlInsert(avlTree * tree, void * value) {
    if (!tree) return 0;
    
    /* Check if we need to grow. */
    if (tree->size >= tree->capacity) {
        if (!avlGrow(tree)) return 0;
    }
    
    uint32_t oldSize = tree->size;
    tree->root = avlInsertNode(tree, tree->root, value);
    tree->parents[tree->root] = AVL_NULL;
    
    /* Return 1 only if a new node was actually inserted. */
    return tree->size > oldSize;
}

/* Remove a value from the tree. The tree is automatically shrunk if the
 * capacity is more than 4 times the size. Returns 1 if the value was
 * found and removed, 0 otherwise. */
int avlRemove(avlTree * tree, void * value) {
    if (!tree || tree->root == AVL_NULL) return 0;
    
    int found = 0;
    tree->root = avlRemoveNode(tree, tree->root, value, &found);
    if (tree->root != AVL_NULL) {
        tree->parents[tree->root] = AVL_NULL;
    }
    
    /* Check if we should shrink. */
    if (found && tree->size > 0 && tree->capacity > INITIAL_CAPACITY && 
        tree->size * 4 <= tree->capacity) {
        avlShrink(tree);
    }
    
    return found;
}

/* Search for a value in the tree. Returns the value if found, NULL otherwise. */
void * avlFind(avlTree * tree, void * value) {
    if (!tree) return NULL;
    
    uint32_t current = tree->root;
    while (current != AVL_NULL) {
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

/* Return the number of elements in the tree. */
int avlGetSize(avlTree * tree) {
    return tree ? tree->size : 0;
}

/* Return 1 if the tree is empty, 0 otherwise. */
int avlIsEmpty(avlTree * tree) {
    return !tree || tree->size == 0;
}

/* The rest of this file is test cases and test helpers. */
#ifdef REDIS_TEST
#include "testhelp.h"
#include <stdio.h>

#define UNUSED(x) (void)(x)

/* Error macro for simple messages without format arguments. */
#define ERR_SIMPLE(x)                                                          \
    do {                                                                       \
        printf("%s:%s:%d:\t", __FILE__, __func__, __LINE__);                   \
        printf("ERROR! " x "\n");                                              \
        err++;                                                                 \
    } while (0)

/* Error macro for formatted messages with variadic arguments. */
#define ERR(x, ...)                                                            \
    do {                                                                       \
        printf("%s:%s:%d:\t", __FILE__, __func__, __LINE__);                   \
        printf("ERROR! " x "\n", __VA_ARGS__);                                 \
        err++;                                                                 \
    } while (0)

#define TEST(name) printf("test — %s\n", name);

/* Compare function for integers. */
static int compareInt(const void * a, const void * b) {
    int ia = *(int *)a;
    int ib = *(int *)b;
    return ia - ib;
}

/* Compare function for strings. */
static int compareStr(const void * a, const void * b) {
    return strcmp((const char *)a, (const char *)b);
}

/* Helper to verify AVL tree properties (balance and heights). */
static int verifyAVLProperties(avlTree * tree, uint32_t nodeIdx, int *heightOut) {
    if (nodeIdx == AVL_NULL) {
        *heightOut = 0;
        return 0;
    }
    
    int leftHeight = 0, rightHeight = 0;
    int errors = 0;
    
    errors += verifyAVLProperties(tree, tree->nodes[nodeIdx].left, &leftHeight);
    errors += verifyAVLProperties(tree, tree->nodes[nodeIdx].right, &rightHeight);
    
    int balance = leftHeight - rightHeight;
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

/* Test helpers: global counter for callback tests. */
static int testFreeCount = 0;

/* Callback that counts invocations and frees memory. */
static void testCountingFreeCallback(void *ptr) {
    testFreeCount++;
    free(ptr);
}

/* Callback that only counts invocations. */
static void testCountingCallback(void *ptr) {
    UNUSED(ptr);
    testFreeCount++;
}

int avlTest(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    
    int err = 0;
    
    TEST("Create and destroy empty tree") {
        avlTree * tree = avlNew(compareInt);
        if (!tree) {
            ERR_SIMPLE("Failed to create tree");
        } else {
            if (!avlIsEmpty(tree)) ERR_SIMPLE("New tree should be empty");
            if (avlGetSize(tree) != 0) ERR("New tree should have size 0, got %d", avlGetSize(tree));
            avlFree(tree);
        }
    }
    
    TEST("Insert and find integers") {
        avlTree * tree = avlNew(compareInt);
        int values[] = {10, 20, 30, 40, 50, 25, 35, 5, 15, 27};
        int n = sizeof(values) / sizeof(values[0]);
        
        for (int i = 0; i < n; i++) {
            if (!avlInsert(tree, &values[i])) {
                ERR("Failed to insert %d", values[i]);
            }
        }
        
        if (avlGetSize(tree) != n) {
            ERR("Expected size %d, got %d", n, avlGetSize(tree));
        }
        
        for (int i = 0; i < n; i++) {
            int *found = (int *)avlFind(tree, &values[i]);
            if (!found || *found != values[i]) {
                ERR("Failed to find %d", values[i]);
            }
        }
        
        int notPresent = 999;
        if (avlFind(tree, &notPresent) != NULL) {
            ERR("Found value that shouldn't exist: %d", notPresent);
        }
        
        int height;
        err += verifyAVLProperties(tree, tree->root, &height);
        
        avlFree(tree);
    }
    
    TEST("Remove integers") {
        avlTree * tree = avlNew(compareInt);
        int values[] = {50, 30, 70, 20, 40, 60, 80};
        int n = sizeof(values) / sizeof(values[0]);
        
        for (int i = 0; i < n; i++) {
            avlInsert(tree, &values[i]);
        }
        
        int toRemove[] = {20, 30, 50};
        for (int i = 0; i < 3; i++) {
            if (!avlRemove(tree, &toRemove[i])) {
                ERR("Failed to remove %d", toRemove[i]);
            }
            if (avlFind(tree, &toRemove[i]) != NULL) {
                ERR("Value %d still found after removal", toRemove[i]);
            }
        }
        
        if (avlGetSize(tree) != n - 3) {
            ERR("Expected size %d after removals, got %d", n - 3, avlGetSize(tree));
        }
        
        int height;
        err += verifyAVLProperties(tree, tree->root, &height);
        
        avlFree(tree);
    }
    
    TEST("Duplicate insertion") {
        avlTree * tree = avlNew(compareInt);
        int value = 42;
        
        if (!avlInsert(tree, &value)) {
            ERR("Failed to insert initial value %d", value);
        }
        
        if (avlInsert(tree, &value)) {
            ERR_SIMPLE("Duplicate insertion should fail but returned success");
        }
        
        if (avlGetSize(tree) != 1) {
            ERR("Size should be 1 after duplicate insertion, got %d", avlGetSize(tree));
        }
        
        avlFree(tree);
    }
    
    TEST("String tree operations") {
        avlTree * tree = avlNew(compareStr);
        char* words[] = {"apple", "banana", "cherry", "date", "fig"};
        int n = sizeof(words) / sizeof(words[0]);
        
        for (int i = 0; i < n; i++) {
            avlInsert(tree, words[i]);
        }
        
        if (avlGetSize(tree) != n) {
            ERR("Expected size %d, got %d", n, avlGetSize(tree));
        }
        
        for (int i = 0; i < n; i++) {
            char* found = (char*)avlFind(tree, words[i]);
            if (!found || strcmp(found, words[i]) != 0) {
                ERR("Failed to find string '%s'", words[i]);
            }
        }
        
        avlRemove(tree, "banana");
        if (avlFind(tree, "banana") != NULL) {
            ERR_SIMPLE("'banana' still found after removal");
        }
        
        int height;
        err += verifyAVLProperties(tree, tree->root, &height);
        
        avlFree(tree);
    }
    
    TEST("Large tree with sequential insertions") {
        avlTree * tree = avlNew(compareInt);
        int values[100];
        
        for (int i = 0; i < 100; i++) {
            values[i] = i;
            avlInsert(tree, &values[i]);
        }
        
        if (avlGetSize(tree) != 100) {
            ERR("Expected size 100, got %d", avlGetSize(tree));
        }
        
        int height;
        err += verifyAVLProperties(tree, tree->root, &height);
        
        for (int i = 0; i < 100; i++) {
            int *found = (int *)avlFind(tree, &values[i]);
            if (!found || *found != values[i]) {
                ERR("Failed to find %d in large tree", values[i]);
            }
        }
        
        avlFree(tree);
    }
    
    TEST("Remove all elements") {
        avlTree * tree = avlNew(compareInt);
        int values[] = {5, 3, 7, 2, 4, 6, 8};
        int n = sizeof(values) / sizeof(values[0]);
        
        for (int i = 0; i < n; i++) {
            avlInsert(tree, &values[i]);
        }
        
        for (int i = 0; i < n; i++) {
            if (!avlRemove(tree, &values[i])) {
                ERR("Failed to remove %d", values[i]);
            }
        }
        
        if (!avlIsEmpty(tree)) {
            ERR_SIMPLE("Tree should be empty after removing all elements");
        }
        
        if (avlGetSize(tree) != 0) {
            ERR("Size should be 0 after removing all, got %d", avlGetSize(tree));
        }
        
        avlFree(tree);
    }
    
    TEST("Free with callback") {
        avlTree * tree = avlNew(compareInt);
        int values[] = {10, 20, 30, 40, 50};
        int n = sizeof(values) / sizeof(values[0]);
        
        /* Allocate dynamic copies of values. */
        int **dynamicValues = (int **)malloc(n * sizeof(int *));
        for (int i = 0; i < n; i++) {
            dynamicValues[i] = (int *)malloc(sizeof(int));
            *dynamicValues[i] = values[i];
            avlInsert(tree, dynamicValues[i]);
        }
        
        /* Track how many times the callback is called. */
        testFreeCount = 0;
        
        /* Free tree with callback. */
        avlFreeWithCallback(tree, testCountingFreeCallback);
        
        if (testFreeCount != n) {
            ERR("Expected callback to be called %d times, but was called %d times", n, testFreeCount);
        }
        
        free(dynamicValues);
    }
    
    TEST("Set free callback and remove") {
        avlTree * tree = avlNew(compareInt);
        int values[] = {10, 20, 30, 40, 50};
        int n = sizeof(values) / sizeof(values[0]);
        
        /* Allocate dynamic copies of values. */
        int **dynamicValues = (int **)malloc(n * sizeof(int *));
        for (int i = 0; i < n; i++) {
            dynamicValues[i] = (int *)malloc(sizeof(int));
            *dynamicValues[i] = values[i];
            avlInsert(tree, dynamicValues[i]);
        }
        
        /* Track how many times the callback is called. */
        testFreeCount = 0;
        
        /* Set the callback. */
        avlSetFreeCallback(tree, testCountingFreeCallback);
        
        /* Remove some values. */
        int toRemove[] = {20, 40};
        int removeCount = 2;
        
        for (int i = 0; i < removeCount; i++) {
            if (!avlRemove(tree, &toRemove[i])) {
                ERR("Failed to remove %d", toRemove[i]);
            }
        }
        
        if (testFreeCount != removeCount) {
            ERR("Expected callback to be called %d times during removal, but was called %d times", 
                removeCount, testFreeCount);
        }
        
        if (avlGetSize(tree) != n - removeCount) {
            ERR("Expected size %d after removals, got %d", n - removeCount, avlGetSize(tree));
        }
        
        /* Free remaining values manually since we still have the callback set. */
        testFreeCount = 0;
        for (int i = 0; i < n; i++) {
            int *found = (int *)avlFind(tree, &values[i]);
            if (found) {
                avlRemove(tree, &values[i]);
            }
        }
        
        if (testFreeCount != (n - removeCount)) {
            ERR("Expected callback to be called %d times for remaining values, but was called %d times", 
                n - removeCount, testFreeCount);
        }
        
        avlFree(tree);
        free(dynamicValues);
    }
    
    TEST("Callback with NULL parameter") {
        avlTree * tree = avlNew(compareInt);
        int value = 42;
        avlInsert(tree, &value);
        
        /* Set callback then disable it. */
        testFreeCount = 0;
        
        avlSetFreeCallback(tree, testCountingCallback);
        avlSetFreeCallback(tree, NULL);  /* Disable callback. */
        
        avlRemove(tree, &value);
        
        if (testFreeCount != 0) {
            ERR("Callback should not be called when set to NULL, but was called %d times", testFreeCount);
        }
        
        avlFree(tree);
    }
    
    TEST("Dynamic resizing - growth") {
        avlTree * tree = avlNew(compareInt);
        
        /* Initial capacity should be INITIAL_CAPACITY (8). */
        uint32_t initialCapacity = tree->capacity;
        if (initialCapacity != INITIAL_CAPACITY) {
            ERR("Initial capacity should be %d, got %u", INITIAL_CAPACITY, initialCapacity);
        }
        
        /* Insert enough elements to trigger growth. */
        int values[100];
        for (int i = 0; i < 100; i++) {
            values[i] = i;
            if (!avlInsert(tree, &values[i])) {
                ERR("Failed to insert value %d", values[i]);
            }
        }
        
        /* Capacity should have grown beyond initial. */
        if (tree->capacity <= initialCapacity) {
            ERR("Capacity should have grown beyond %u, but is %u", initialCapacity, tree->capacity);
        }
        
        /* Verify all values are still findable after growth. */
        for (int i = 0; i < 100; i++) {
            int *found = (int *)avlFind(tree, &values[i]);
            if (!found || *found != values[i]) {
                ERR("Failed to find value %d after growth", values[i]);
            }
        }
        
        /* Verify tree properties are maintained. */
        int height;
        err += verifyAVLProperties(tree, tree->root, &height);
        
        if (avlGetSize(tree) != 100) {
            ERR("Expected size 100, got %d", avlGetSize(tree));
        }
        
        avlFree(tree);
    }
    
    TEST("Dynamic resizing - shrinking") {
        avlTree * tree = avlNew(compareInt);
        
        /* Insert many elements to grow the tree. */
        int values[100];
        for (int i = 0; i < 100; i++) {
            values[i] = i;
            avlInsert(tree, &values[i]);
        }
        
        uint32_t grownCapacity = tree->capacity;
        
        /* Remove most elements to trigger shrinking. */
        for (int i = 0; i < 95; i++) {
            if (!avlRemove(tree, &values[i])) {
                ERR("Failed to remove value %d", values[i]);
            }
        }
        
        /* Capacity should have shrunk. */
        if (tree->capacity >= grownCapacity) {
            ERR("Capacity should have shrunk from %u, but is %u", grownCapacity, tree->capacity);
        }
        
        /* Verify remaining values are still findable after shrinking. */
        for (int i = 95; i < 100; i++) {
            int *found = (int *)avlFind(tree, &values[i]);
            if (!found || *found != values[i]) {
                ERR("Failed to find value %d after shrinking", values[i]);
            }
        }
        
        /* Verify tree properties are maintained. */
        int height;
        err += verifyAVLProperties(tree, tree->root, &height);
        
        if (avlGetSize(tree) != 5) {
            ERR("Expected size 5, got %d", avlGetSize(tree));
        }
        
        avlFree(tree);
    }
    
    TEST("Resize with deletions and reinsertions") {
        avlTree * tree = avlNew(compareInt);
        
        /* Insert, remove, and reinsert to test resize with fragmentation. */
        int values[50];
        for (int i = 0; i < 50; i++) {
            values[i] = i;
            avlInsert(tree, &values[i]);
        }
        
        /* Remove every other element. */
        for (int i = 0; i < 50; i += 2) {
            avlRemove(tree, &values[i]);
        }
        
        if (avlGetSize(tree) != 25) {
            ERR("Expected size 25 after removals, got %d", avlGetSize(tree));
        }
        
        /* Reinsert the removed elements. */
        for (int i = 0; i < 50; i += 2) {
            if (!avlInsert(tree, &values[i])) {
                ERR("Failed to reinsert value %d", values[i]);
            }
        }
        
        if (avlGetSize(tree) != 50) {
            ERR("Expected size 50 after reinsertions, got %d", avlGetSize(tree));
        }
        
        /* Verify all values are findable. */
        for (int i = 0; i < 50; i++) {
            int *found = (int *)avlFind(tree, &values[i]);
            if (!found || *found != values[i]) {
                ERR("Failed to find value %d after reinsertions", values[i]);
            }
        }
        
        /* Verify tree properties. */
        int height;
        err += verifyAVLProperties(tree, tree->root, &height);
        
        avlFree(tree);
    }
    
    TEST("Minimum capacity maintained") {
        avlTree * tree = avlNew(compareInt);
        
        /* Insert a few elements. */
        int values[3] = {10, 20, 30};
        for (int i = 0; i < 3; i++) {
            avlInsert(tree, &values[i]);
        }
        
        /* Remove all but one. */
        avlRemove(tree, &values[0]);
        avlRemove(tree, &values[1]);
        
        /* Capacity should not go below INITIAL_CAPACITY. */
        if (tree->capacity < INITIAL_CAPACITY) {
            ERR("Capacity %u should not be less than INITIAL_CAPACITY %d", 
                tree->capacity, INITIAL_CAPACITY);
        }
        
        /* Verify remaining element is still findable. */
        int *found = (int *)avlFind(tree, &values[2]);
        if (!found || *found != values[2]) {
            ERR("Failed to find remaining value %d", values[2]);
        }
        
        avlFree(tree);
    }
    
    if (!err)
        printf("ALL TESTS PASSED!\n");
    else
        ERR("Sorry, not all tests passed! In fact, %d tests failed.", err);
    
    return err;
}

#endif
