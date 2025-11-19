/* Ring -- A circular ring buffer implementation.
 *
 * Copyright (c) 2017-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv3); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include "ring.h"
#include "zmalloc.h"
#include "redisassert.h"

/* ----------------------------- Internal helpers -------------------------- */

/* Resize the ring buffer to a new capacity.
 * Returns 1 on success, 0 on out of memory.
 * This function copies all existing elements to the new buffer. */
static int ringResize(ring *r, size_t new_capacity) {
    void **new_items = zmalloc(sizeof(void*) * new_capacity);
    if (new_items == NULL) {
        errno = ENOMEM;
        return 0;
    }

    /* Copy existing items to new buffer in order */
    if (r->count > 0) {
        if (r->head < r->tail) {
            /* Continuous block: head...tail */
            memcpy(new_items, r->items + r->head, sizeof(void*) * r->count);
        } else {
            /* Wrapped around: head...end and start...tail */
            size_t first_part = r->capacity - r->head;
            memcpy(new_items, r->items + r->head, sizeof(void*) * first_part);
            memcpy(new_items + first_part, r->items, sizeof(void*) * r->tail);
        }
    }

    /* Free old buffer and update ring structure */
    zfree(r->items);
    r->items = new_items;
    r->capacity = new_capacity;
    r->head = 0;
    r->tail = r->count;

    return 1;
}

/* Check if the ring buffer should grow and grow it if needed.
 * Returns 1 on success or if no resize needed, 0 on out of memory. */
static int ringGrow(ring *r) {
    if (r->count >= r->capacity) {
        size_t new_capacity = r->capacity * 2;
        return ringResize(r, new_capacity);
    }
    return 1;
}

/* Check if the ring buffer should shrink and shrink it if needed.
 * Shrinks when usage drops below 25% and capacity is above minimum.
 * Returns 1 on success or if no resize needed, 0 on out of memory. */
static int ringShrink(ring *r) {
    if (r->capacity > RING_MIN_CAPACITY && r->count < r->capacity / 4) {
        size_t new_capacity = r->capacity / 2;
        if (new_capacity < RING_MIN_CAPACITY) {
            new_capacity = RING_MIN_CAPACITY;
        }
        return ringResize(r, new_capacity);
    }
    return 1;
}

/* -------------------------- Ring buffer API ------------------------------ */

/* Create a new ring buffer with default initial capacity. */
ring *ringNew(void) {
    return ringNewWithCapacity(RING_MIN_CAPACITY);
}

/* Create a new ring buffer with specified initial capacity. */
ring *ringNewWithCapacity(size_t initial_capacity) {
    if (initial_capacity < RING_MIN_CAPACITY) {
        initial_capacity = RING_MIN_CAPACITY;
    }

    ring *r = zmalloc(sizeof(ring));
    if (r == NULL) return NULL;

    r->items = zmalloc(sizeof(void*) * initial_capacity);
    if (r->items == NULL) {
        zfree(r);
        return NULL;
    }

    r->capacity = initial_capacity;
    r->head = 0;
    r->tail = 0;
    r->count = 0;
    r->free_callback = NULL;

    return r;
}

/* Push an item onto the end of the ring buffer. */
int ringPush(ring *r, void *item) {
    if (r == NULL) return 0;

    /* Grow the buffer if necessary */
    if (!ringGrow(r)) {
        return 0;
    }

    /* Add the item at the tail position */
    r->items[r->tail] = item;
    r->tail = (r->tail + 1) % r->capacity;
    r->count++;

    return 1;
}

/* Remove and return the first item from the ring buffer. */
void *ringPop(ring *r) {
    if (r == NULL || r->count == 0) {
        return NULL;
    }

    /* Get the item at the head position */
    void *item = r->items[r->head];
    r->head = (r->head + 1) % r->capacity;
    r->count--;

    /* Call the free callback if set */
    if (r->free_callback != NULL) {
        r->free_callback(item);
    }

    /* Shrink the buffer if it's mostly empty */
    ringShrink(r);

    return item;
}

/* Return the first item without removing it. */
void *ringFront(ring *r) {
    if (r == NULL || r->count == 0) {
        return NULL;
    }
    return r->items[r->head];
}

/* Return the number of items currently in the ring buffer. */
size_t ringSize(ring *r) {
    if (r == NULL) return 0;
    return r->count;
}

/* Return the current capacity of the ring buffer. */
size_t ringCapacity(ring *r) {
    if (r == NULL) return 0;
    return r->capacity;
}

/* Check if the ring buffer is empty. */
int ringIsEmpty(ring *r) {
    return (r == NULL || r->count == 0);
}

/* Free the ring buffer structure. */
void ringFree(ring *r) {
    if (r == NULL) return;
    if (r->items != NULL) {
        zfree(r->items);
    }
    zfree(r);
}

/* Free the ring buffer and call the provided callback for each item. */
void ringFreeWithCallback(ring *r, void (*free_callback)(void*)) {
    if (r == NULL) return;
    
    if (free_callback != NULL) {
        /* Free all items in the buffer */
        while (r->count > 0) {
            void *item = ringPop(r);
            free_callback(item);
        }
    }
    
    ringFree(r);
}

/* Clear all items from the ring buffer without freeing them. */
void ringClear(ring *r) {
    if (r == NULL) return;
    r->head = 0;
    r->tail = 0;
    r->count = 0;
}

/* Set a callback function to be called when an item is removed from the ring. */
void ringSetFreeCallback(ring *r, void (*callback)(void*)) {
    if (r == NULL) return;
    r->free_callback = callback;
}

/* --------------------------------- Tests --------------------------------- */

#ifdef REDIS_TEST
#include <assert.h>

#define UNUSED(x) (void)(x)

/* Test callback functions */
static int test_callback_count = 0;
static int test_freed_count = 0;

static void test_count_callback(void *item) {
    UNUSED(item);
    test_callback_count++;
}

static void test_tracking_free(void *item) {
    test_freed_count++;
    zfree(item);
}

int ringTest(int argc, char *argv[], int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    int errors = 0;

    printf("Testing ring buffer... ");

    /* Test 1: Basic creation and destruction */
    {
        ring *r = ringNew();
        assert(r != NULL);
        assert(ringSize(r) == 0);
        assert(ringIsEmpty(r) == 1);
        assert(ringCapacity(r) == RING_MIN_CAPACITY);
        ringFree(r);
    }

    /* Test 2: Push and pop single item */
    {
        ring *r = ringNew();
        int value = 42;
        int *ptr = &value;
        
        assert(ringPush(r, ptr) == 1);
        assert(ringSize(r) == 1);
        assert(ringIsEmpty(r) == 0);
        
        void *result = ringPop(r);
        assert(result == ptr);
        assert(ringSize(r) == 0);
        assert(ringIsEmpty(r) == 1);
        
        ringFree(r);
    }

    /* Test 3: Push multiple items */
    {
        ring *r = ringNew();
        int values[5] = {1, 2, 3, 4, 5};
        
        for (int i = 0; i < 5; i++) {
            assert(ringPush(r, &values[i]) == 1);
            assert(ringSize(r) == (size_t)(i + 1));
        }
        
        for (int i = 0; i < 5; i++) {
            int *result = ringPop(r);
            assert(result == &values[i]);
            assert(*result == values[i]);
        }
        
        assert(ringSize(r) == 0);
        ringFree(r);
    }

    /* Test 4: ringFront (peek without removing) */
    {
        ring *r = ringNew();
        int value = 99;
        
        ringPush(r, &value);
        
        void *front1 = ringFront(r);
        assert(front1 == &value);
        assert(ringSize(r) == 1);  /* Size unchanged */
        
        void *front2 = ringFront(r);
        assert(front2 == &value);
        assert(ringSize(r) == 1);  /* Size still unchanged */
        
        void *popped = ringPop(r);
        assert(popped == &value);
        assert(ringSize(r) == 0);
        
        assert(ringFront(r) == NULL);  /* Empty buffer */
        
        ringFree(r);
    }

    /* Test 5: Automatic growth */
    {
        ring *r = ringNew();
        size_t initial_capacity = ringCapacity(r);
        int values[100];
        
        /* Fill beyond initial capacity */
        for (int i = 0; i < 100; i++) {
            values[i] = i;
            assert(ringPush(r, &values[i]) == 1);
        }
        
        assert(ringSize(r) == 100);
        assert(ringCapacity(r) > initial_capacity);
        
        /* Verify order is preserved */
        for (int i = 0; i < 100; i++) {
            int *result = ringPop(r);
            assert(*result == i);
        }
        
        ringFree(r);
    }

    /* Test 6: Automatic shrinking */
    {
        ring *r = ringNew();
        int values[100];
        
        /* Fill the buffer */
        for (int i = 0; i < 100; i++) {
            values[i] = i;
            ringPush(r, &values[i]);
        }
        
        size_t max_capacity = ringCapacity(r);
        
        /* Remove most items */
        for (int i = 0; i < 90; i++) {
            ringPop(r);
        }
        
        assert(ringSize(r) == 10);
        /* Capacity should have shrunk */
        assert(ringCapacity(r) < max_capacity);
        
        ringFree(r);
    }

    /* Test 7: Wrap-around behavior */
    {
        ring *r = ringNewWithCapacity(4);
        int values[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
        
        /* Push and pop to cause wrap-around */
        ringPush(r, &values[0]);
        ringPush(r, &values[1]);
        ringPush(r, &values[2]);
        
        int *v0 = ringPop(r);
        int *v1 = ringPop(r);
        assert(*v0 == 0 && *v1 == 1);
        
        ringPush(r, &values[3]);
        ringPush(r, &values[4]);
        ringPush(r, &values[5]);
        
        /* Now we should have wrapped around */
        assert(ringSize(r) == 4);
        
        int *v2 = ringPop(r);
        assert(*v2 == 2);
        
        ringFree(r);
    }

    /* Test 8: ringClear */
    {
        ring *r = ringNew();
        int values[5] = {1, 2, 3, 4, 5};
        
        for (int i = 0; i < 5; i++) {
            ringPush(r, &values[i]);
        }
        
        assert(ringSize(r) == 5);
        ringClear(r);
        assert(ringSize(r) == 0);
        assert(ringIsEmpty(r) == 1);
        
        /* Should be able to use after clear */
        ringPush(r, &values[0]);
        assert(ringSize(r) == 1);
        
        ringFree(r);
    }

    /* Test 9: ringFreeWithCallback */
    {
        ring *r = ringNew();
        
        /* Allocate some items */
        for (int i = 0; i < 5; i++) {
            int *item = zmalloc(sizeof(int));
            *item = i;
            ringPush(r, item);
        }
        
        /* Free with callback that frees each item */
        ringFreeWithCallback(r, zfree);
        /* If we got here without crashing, the test passed */
    }

    /* Test 10: Edge cases - NULL handling */
    {
        assert(ringSize(NULL) == 0);
        assert(ringCapacity(NULL) == 0);
        assert(ringIsEmpty(NULL) == 1);
        assert(ringFront(NULL) == NULL);
        assert(ringPop(NULL) == NULL);
        ringFree(NULL);  /* Should not crash */
        ringClear(NULL);  /* Should not crash */
        ringSetFreeCallback(NULL, zfree);  /* Should not crash */
    }

    /* Test 11: Callback function on pop */
    {
        ring *r = ringNew();
        test_callback_count = 0;
        
        /* Set the callback */
        ringSetFreeCallback(r, test_count_callback);
        
        /* Push some items */
        int values[5] = {1, 2, 3, 4, 5};
        for (int i = 0; i < 5; i++) {
            ringPush(r, &values[i]);
        }
        
        /* Pop items - callback should be invoked each time */
        ringPop(r);
        assert(test_callback_count == 1);
        
        ringPop(r);
        assert(test_callback_count == 2);
        
        ringPop(r);
        assert(test_callback_count == 3);
        
        /* Disable callback */
        ringSetFreeCallback(r, NULL);
        ringPop(r);
        assert(test_callback_count == 3);  /* Should still be 3 */
        
        ringPop(r);
        assert(test_callback_count == 3);  /* Should still be 3 */
        
        ringFree(r);
    }

    /* Test 12: Callback with actual memory allocation */
    {
        ring *r = ringNew();
        test_freed_count = 0;
        
        ringSetFreeCallback(r, test_tracking_free);
        
        /* Allocate and push items */
        for (int i = 0; i < 5; i++) {
            int *item = zmalloc(sizeof(int));
            *item = i;
            ringPush(r, item);
        }
        
        /* Pop all items - callback should free them */
        while (!ringIsEmpty(r)) {
            ringPop(r);
        }
        
        assert(test_freed_count == 5);
        ringFree(r);
    }

    printf("PASSED! All %d tests successful.\n", 12);
    return errors;
}

#endif

