/* HNSW (Hierarchical Navigable Small World) Implementation.
 *
 * Based on the paper by Yu. A. Malkov, D. A. Yashunin.
 *
 * Many details of this implementation, not covered in the paper, were
 * obtained simulating different workloads and checking the connection
 * quality of the graph.
 *
 * Notably, this implementation:
 *
 * 1. Only uses bi-directional links, implementing strategies in order to
 *    link new nodes even when candidates are full, and our new node would
 *    be not close enough to replace old links in candidate.
 *
 * 2. We normalize on-insert, making cosine similarity and dot product the
 *    same. This means we can't use euclidean distance or alike here.
 *    Together with quantization, this provides an important speedup that
 *    makes HNSW more practical.
 *
 * 3. The quantization used is int8. And it is performed per-vector, so the
 *    "range" (max abs value) is also stored alongside with the quantized data.
 *
 * 4. This library implements true elements deletion, not just marking the
 *    element as deleted, but removing it (we can do it since our links are
 *    bidirectional), and reliking the nodes orphaned of one link among
 *    them.
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 * Originally authored by: Salvatore Sanfilippo.
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <float.h>  /* for INFINITY if not in math.h */
#include <assert.h>
#include "hnsw.h"
#include "mixer.h"

#if 0
#define debugmsg printf
#else
#define debugmsg if(0) printf
#endif

#ifndef INFINITY
#define INFINITY (1.0/0.0)
#endif

#define MIN(a,b) ((a) < (b) ? (a) : (b))

/* Algorithm parameters. */

#define HNSW_P 0.25         /* Probability of level increase. */
#define HNSW_MAX_LEVEL 16   /* Max level nodes can reach. */
#define HNSW_EF_C 200       /* Default size of dynamic candidate list while
                             * inserting a new node, in case 0 is passed to
                             * the 'ef' argument while inserting. This is also
                             * used when deleting nodes for the search step
                             * needed sometimes to reconnect nodes that remain
                             * orphaned of one link. */

static void (*hfree)(void *p) = free;
static void *(*hmalloc)(size_t s) = malloc;
static void *(*hrealloc)(void *old, size_t s) = realloc;

void hnsw_set_allocator(void (*free_ptr)(void*), void *(*malloc_ptr)(size_t),
                        void *(*realloc_ptr)(void*, size_t))
{
    hfree = free_ptr;
    hmalloc = malloc_ptr;
    hrealloc = realloc_ptr;
}

// Get a warning if you use the libc allocator functions for mistake.
#define malloc use_hmalloc_instead
#define realloc use_hrealloc_instead
#define free use_hfree_instead

/* ============================== Prototypes ================================ */
void hnsw_cursor_element_deleted(HNSW *index, hnswNode *deleted);

/* ============================ Priority queue ================================
 * We need a priority queue to take an ordered list of candidates. Right now
 * it is implemented as a linear array, since it is relatively small.
 *
 * You may find it to be odd that we take the best element (smaller distance)
 * at the end of the array, but this way popping from the pqueue is O(1), as
 * we need to just decrement the count, and this is a very used operation
 * in a critical code path. This makes the priority queue implementation a
 * bit more complex in the insertion, but for good reasons. */

/* Maximum number of candidates we'll ever need (cit. Bill Gates). */
#define HNSW_MAX_CANDIDATES 256

typedef struct {
    hnswNode *node;
    float distance;
} pqitem;

typedef struct {
    pqitem *items;         /* Array of items. */
    uint32_t count;        /* Current number of items. */
    uint32_t cap;          /* Maximum capacity. */
} pqueue;

/* The HNSW algorithms access the pqueue conceptually from nearest (index 0)
 * to farthest (larger indexes) node, so the following macros are used to
 * access the pqueue in this fashion, even if the internal order is
 * actually reversed. */
#define pq_get_node(q,i) ((q)->items[(q)->count-(i+1)].node)
#define pq_get_distance(q,i) ((q)->items[(q)->count-(i+1)].distance)

/* Create a new priority queue with given capacity. Adding to the
 * pqueue only retains 'capacity' elements with the shortest distance. */
pqueue *pq_new(uint32_t capacity) {
    pqueue *pq = hmalloc(sizeof(*pq));
    if (!pq) return NULL;

    pq->items = hmalloc(sizeof(pqitem) * capacity);
    if (!pq->items) {
        hfree(pq);
        return NULL;
    }

    pq->count = 0;
    pq->cap = capacity;
    return pq;
}

/* Free a priority queue. */
void pq_free(pqueue *pq) {
    if (!pq) return;
    hfree(pq->items);
    hfree(pq);
}

/* Insert maintaining distance order (higher distances first). */
void pq_push(pqueue *pq, hnswNode *node, float distance) {
    if (pq->count < pq->cap) {
        /* Queue not full: shift right from high distances to make room. */
        uint32_t i = pq->count;
        while (i > 0 && pq->items[i-1].distance < distance) {
            pq->items[i] = pq->items[i-1];
            i--;
        }
        pq->items[i].node = node;
        pq->items[i].distance = distance;
        pq->count++;
    } else {
        /* Queue full: if new item is worse than worst, ignore it. */
        if (distance >= pq->items[0].distance) return;

        /* Otherwise shift left from low distances to drop worst. */
        uint32_t i = 0;
        while (i < pq->cap-1 && pq->items[i+1].distance > distance) {
            pq->items[i] = pq->items[i+1];
            i++;
        }
        pq->items[i].node = node;
        pq->items[i].distance = distance;
    }
}

/* Remove and return the top (closest) element, which is at count-1
 * since we store elements with higher distances first.
 * Runs in constant time. */
hnswNode *pq_pop(pqueue *pq, float *distance) {
    if (pq->count == 0) return NULL;
    pq->count--;
    *distance = pq->items[pq->count].distance;
    return pq->items[pq->count].node;
}

/* Get distance of the furthest element.
 * An empty priority queue has infinite distance as its furthest element,
 * note that this behavior is needed by the algorithms below. */
float pq_max_distance(pqueue *pq) {
    if (pq->count == 0) return INFINITY;
    return pq->items[0].distance;
}

/* ============================ HNSW algorithm ============================== */

/* Dot product: our vectors are already normalized.
 * Version for not quantized vectors of floats. */
float vectors_distance_float(const float *x, const float *y, uint32_t dim) {
    /* Use two accumulators to reduce dependencies among multiplications.
     * This provides a clear speed boost in Apple silicon, but should be
     * help in general. */
    float dot0 = 0.0f, dot1 = 0.0f;
    uint32_t i;

    // Process 8 elements per iteration, 50/50 with the two accumulators.
    for (i = 0; i + 7 < dim; i += 8) {
        dot0 += x[i] * y[i] +
                x[i+1] * y[i+1] +
                x[i+2] * y[i+2] +
                x[i+3] * y[i+3];

        dot1 += x[i+4] * y[i+4] +
                x[i+5] * y[i+5] +
                x[i+6] * y[i+6] +
                x[i+7] * y[i+7];
    }

    /* Handle the remaining elements. These are a minority in the case
     * of a small vector, don't optimize this part. */
    for (; i < dim; i++) dot0 += x[i] * y[i];

    /* The following line may be counter intuitive. The dot product of
     * normalized vectors is equivalent to their cosine similarity. The
     * cosine will be from -1 (vectors facing opposite directions in the
     * N-dim space) to 1 (vectors are facing in the same direction).
     *
     * We kinda want a "score" of distance from 0 to 2 (this is a distance
     * function and we want minimize the distance for K-NN searches), so we
     * can't just add 1: that would return a number in the 0-2 range, with
     * 0 meaning opposite vectors and 2 identical vectors, so this is
     * similarity, not distance.
     *
     * Returning instead (1 - dotprod) inverts the meaning: 0 is identical
     * and 2 is opposite, hence it is their distance.
     *
     * Why don't normalize the similarity right now, and return from 0 to
     * 1? Because division is costly. */
    return 1.0f - (dot0 + dot1);
}

/* Q8 quants dotproduct. We do integer math and later fix it by range. */
float vectors_distance_q8(const int8_t *x, const int8_t *y, uint32_t dim,
                        float range_a, float range_b) {
    // Handle zero vectors special case.
    if (range_a == 0 || range_b == 0) {
        /* Zero vector distance from anything is 1.0
         * (since 1.0 - dot_product where dot_product = 0). */
        return 1.0f;
    }

    /* Each vector is quantized from [-max_abs, +max_abs] to [-127, 127]
     * where range = 2*max_abs. */
    const float scale_product = (range_a/127) * (range_b/127);

    int32_t dot0 = 0, dot1 = 0;
    uint32_t i;

    // Process 8 elements at a time for better pipeline utilization.
    for (i = 0; i + 7 < dim; i += 8) {
        dot0 += ((int32_t)x[i]) * ((int32_t)y[i]) +
                ((int32_t)x[i+1]) * ((int32_t)y[i+1]) +
                ((int32_t)x[i+2]) * ((int32_t)y[i+2]) +
                ((int32_t)x[i+3]) * ((int32_t)y[i+3]);

        dot1 += ((int32_t)x[i+4]) * ((int32_t)y[i+4]) +
                ((int32_t)x[i+5]) * ((int32_t)y[i+5]) +
                ((int32_t)x[i+6]) * ((int32_t)y[i+6]) +
                ((int32_t)x[i+7]) * ((int32_t)y[i+7]);
    }

    // Handle remaining elements.
    for (; i < dim; i++) dot0 += ((int32_t)x[i]) * ((int32_t)y[i]);

    // Convert to original range.
    float dotf = (dot0 + dot1) * scale_product;
    float distance = 1.0f - dotf;

    // Clamp distance to [0, 2].
    if (distance < 0) distance = 0;
    else if (distance > 2) distance = 2;
    return distance;
}

static inline int popcount64(uint64_t x) {
    x = (x & 0x5555555555555555) + ((x >> 1) & 0x5555555555555555);
    x = (x & 0x3333333333333333) + ((x >> 2) & 0x3333333333333333);
    x = (x & 0x0F0F0F0F0F0F0F0F) + ((x >> 4) & 0x0F0F0F0F0F0F0F0F);
    x = (x & 0x00FF00FF00FF00FF) + ((x >> 8) & 0x00FF00FF00FF00FF);
    x = (x & 0x0000FFFF0000FFFF) + ((x >> 16) & 0x0000FFFF0000FFFF);
    x = (x & 0x00000000FFFFFFFF) + ((x >> 32) & 0x00000000FFFFFFFF);
    return x;
}

/* Binary vectors distance. */
float vectors_distance_bin(const uint64_t *x, const uint64_t *y, uint32_t dim) {
    uint32_t len = (dim+63)/64;
    uint32_t opposite = 0;
    for (uint32_t j = 0; j < len; j++) {
        uint64_t xor = x[j]^y[j];
        opposite += popcount64(xor);
    }
    return (float)opposite*2/dim;
}

/* Dot product between nodes. Will call the right version depending on the
 * quantization used. */
float hnsw_distance(HNSW *index, hnswNode *a, hnswNode *b) {
    switch(index->quant_type) {
    case HNSW_QUANT_NONE:
        return vectors_distance_float(a->vector,b->vector,index->vector_dim);
    case HNSW_QUANT_Q8:
        return vectors_distance_q8(a->vector,b->vector,index->vector_dim,a->quants_range,b->quants_range);
    case HNSW_QUANT_BIN:
        return vectors_distance_bin(a->vector,b->vector,index->vector_dim);
    default:
        assert(1 != 1);
        return 0;
    }
}

/* This do Q8 'range' quantization.
 * For people looking at this code thinking: Oh, I could use min/max
 * quants instead! Well: I tried with min/max normalization but the dot
 * product needs to accumulate the sum for later correction, and it's slower. */
void quantize_to_q8(float *src, int8_t *dst, uint32_t dim, float *rangeptr) {
    float max_abs = 0;
    for (uint32_t j = 0; j < dim; j++) {
        if (src[j] > max_abs) max_abs = src[j];
        if (-src[j] > max_abs) max_abs = -src[j];
    }

    if (max_abs == 0) {
        if (rangeptr) *rangeptr = 0;
        memset(dst, 0, dim);
        return;
    }

    const float scale = 127.0f / max_abs;  // Scale to map to [-127, 127].

    for (uint32_t j = 0; j < dim; j++) {
        dst[j] = (int8_t)roundf(src[j] * scale);
    }
    if (rangeptr) *rangeptr = max_abs;  // Return max_abs instead of 2*max_abs.
}

/* Binary quantization of vector 'src' to 'dst'. We use full words of
 * 64 bit as smallest unit, we will just set all the unused bits to 0
 * so that they'll be the same in all the vectors, and when xor+popcount
 * is used to compute the distance, such bits are not considered. This
 * allows to go faster. */
void quantize_to_bin(float *src, uint64_t *dst, uint32_t dim) {
    memset(dst,0,(dim+63)/64*sizeof(uint64_t));
    for (uint32_t j = 0; j < dim; j++) {
        uint32_t word = j/64;
        uint32_t bit = j&63;
        /* Since cosine similarity checks the vector direction and
         * not magnitudo, we do likewise in the binary quantization and
         * just remember if the component is positive or negative. */
        if (src[j] > 0) dst[word] |= 1ULL<<bit;
    }
}

/* L2 normalization of the float vector.
 *
 * Store the L2 value on 'l2ptr' if not NULL. This way the process
 * can be reversed even if some precision will be lost. */
void hnsw_normalize_vector(float *x, float *l2ptr, uint32_t dim) {
    float l2 = 0;
    uint32_t i;
    for (i = 0; i + 3 < dim; i += 4) {
        l2 += x[i]*x[i] +
              x[i+1]*x[i+1] +
              x[i+2]*x[i+2] +
              x[i+3]*x[i+3];
    }
    for (; i < dim; i++) l2 += x[i]*x[i];
    if (l2 == 0) return; // All zero vector, can't normalize.

    l2 = sqrtf(l2);
    if (l2ptr) *l2ptr = l2;
    for (i = 0; i < dim; i++) x[i] /= l2;
}

/* Helper function to generate random level. */
uint32_t random_level(void) {
    static const int threshold = HNSW_P * RAND_MAX;
    uint32_t level = 0;

    while (rand() < threshold && level < HNSW_MAX_LEVEL)
        level += 1;
    return level;
}

/* Create new HNSW index, quantized or not. */
HNSW *hnsw_new(uint32_t vector_dim, uint32_t quant_type, uint32_t m) {
    HNSW *index = hmalloc(sizeof(HNSW));
    if (!index) return NULL;

    /* M parameter sanity check. */
    if (m == 0) m = HNSW_DEFAULT_M;
    else if (m > HNSW_MAX_M) m = HNSW_MAX_M;

    index->M = m;
    index->quant_type = quant_type;
    index->enter_point = NULL;
    index->max_level = 0;
    index->vector_dim = vector_dim;
    index->node_count = 0;
    index->last_id = 0;
    index->head = NULL;
    index->cursors = NULL;

    /* Initialize epochs array. */
    for (int i = 0; i < HNSW_MAX_THREADS; i++)
        index->current_epoch[i] = 0;

    /* Initialize locks. */
    if (pthread_rwlock_init(&index->global_lock, NULL) != 0) {
        hfree(index);
        return NULL;
    }

    for (int i = 0; i < HNSW_MAX_THREADS; i++) {
        if (pthread_mutex_init(&index->slot_locks[i], NULL) != 0) {
            /* Clean up previously initialized mutexes. */
            for (int j = 0; j < i; j++)
                pthread_mutex_destroy(&index->slot_locks[j]);
            pthread_rwlock_destroy(&index->global_lock);
            hfree(index);
            return NULL;
        }
    }

    /* Initialize atomic variables. */
    index->next_slot = 0;
    index->version = 0;
    return index;
}

/* Fill 'vec' with the node vector, de-normalizing and de-quantizing it
 * as needed. Note that this function will return an approximated version
 * of the original vector. */
void hnsw_get_node_vector(HNSW *index, hnswNode *node, float *vec) {
    if (index->quant_type == HNSW_QUANT_NONE) {
        memcpy(vec,node->vector,index->vector_dim*sizeof(float));
    } else if (index->quant_type == HNSW_QUANT_Q8) {
        int8_t *quants = node->vector;
        for (uint32_t j = 0; j < index->vector_dim; j++)
            vec[j] = (quants[j]*node->quants_range)/127;
    } else if (index->quant_type == HNSW_QUANT_BIN) {
        uint64_t *bits = node->vector;
        for (uint32_t j = 0; j < index->vector_dim; j++) {
            uint32_t word = j/64;
            uint32_t bit = j&63;
            vec[j] = (bits[word] & (1ULL<<bit)) ? 1.0f : -1.0f;
        }
    }

    // De-normalize.
    if (index->quant_type != HNSW_QUANT_BIN) {
        for (uint32_t j = 0; j < index->vector_dim; j++)
            vec[j] *= node->l2;
    }
}

/* Return the number of bytes needed to represent a vector in the index,
 * that is function of the dimension of the vectors and the quantization
 * type used. */
uint32_t hnsw_quants_bytes(HNSW *index) {
    switch(index->quant_type) {
    case HNSW_QUANT_NONE: return index->vector_dim * sizeof(float);
    case HNSW_QUANT_Q8: return index->vector_dim;
    case HNSW_QUANT_BIN: return (index->vector_dim+63)/64*8;
    default: assert(0 && "Quantization type not supported.");
    }
}

/* Create new node. Returns NULL on out of memory.
 * It is possible to pass the vector as floats or, in case this index
 * was already stored on disk and is being loaded, or serialized and
 * transmitted in any form, the already quantized version in
 * 'qvector'.
 *
 * Only vector or qvector should be non-NULL. The reason why passing
 * a quantized vector is useful, is that because re-normalizing and
 * re-quantizing several times the same vector may accumulate rounding
 * errors. So if you work with quantized indexes, you should save
 * the quantized indexes.
 *
 * Note that, together with qvector, the quantization range is needed,
 * since this library uses per-vector quantization. In case of quantized
 * vectors the l2 is considered to be '1', so if you want to restore
 * the right l2 (to use the API that returns an approximation of the
 * original vector) make sure to save the l2 on disk and set it back
 * after the node creation (see later for the serialization API that
 * handles this and more). */
hnswNode *hnsw_node_new(HNSW *index, uint64_t id, const float *vector, const int8_t *qvector, float qrange, uint32_t level, int normalize) {
    hnswNode *node = hmalloc(sizeof(hnswNode)+(sizeof(hnswNodeLayer)*(level+1)));
    if (!node) return NULL;

    if (id == 0) id = ++index->last_id;
    node->level = level;
    node->id = id;
    node->next = NULL;
    node->vector = NULL;
    node->l2 = 1;   // Default in case of already quantized vectors. It is
                    // up to the caller to fill this later, if needed.

    /* Initialize visited epoch array. */
    for (int i = 0; i < HNSW_MAX_THREADS; i++)
        node->visited_epoch[i] = 0;

    if (qvector == NULL) {
        /* Copy input vector. */
        node->vector = hmalloc(sizeof(float) * index->vector_dim);
        if (!node->vector) {
            hfree(node);
            return NULL;
        }
        memcpy(node->vector, vector, sizeof(float) * index->vector_dim);
        if (normalize)
            hnsw_normalize_vector(node->vector,&node->l2,index->vector_dim);

        /* Handle quantization. */
        if (index->quant_type != HNSW_QUANT_NONE) {
            void *quants = hmalloc(hnsw_quants_bytes(index));
            if (quants == NULL) {
                hfree(node->vector);
                hfree(node);
                return NULL;
            }

            // Quantize.
            switch(index->quant_type) {
            case HNSW_QUANT_Q8:
                quantize_to_q8(node->vector,quants,index->vector_dim,&node->quants_range);
                break;
            case HNSW_QUANT_BIN:
                quantize_to_bin(node->vector,quants,index->vector_dim);
                break;
            default:
                assert(0 && "Quantization type not handled.");
                break;
            }

            // Discard the full precision vector.
            hfree(node->vector);
            node->vector = quants;
        }
    } else {
        // We got the already quantized vector. Just copy it.
        assert(index->quant_type != HNSW_QUANT_NONE);
        uint32_t vector_bytes = hnsw_quants_bytes(index);
        node->vector = hmalloc(vector_bytes);
        node->quants_range = qrange;
        if (node->vector == NULL) {
            hfree(node);
            return NULL;
        }
        memcpy(node->vector,qvector,vector_bytes);
    }

    /* Initialize each layer. */
    for (uint32_t i = 0; i <= level; i++) {
        uint32_t max_links = (i == 0) ? index->M*2 : index->M;
        node->layers[i].max_links = max_links;
        node->layers[i].num_links = 0;
        node->layers[i].worst_distance = 0;
        node->layers[i].worst_idx = 0;
        node->layers[i].links = hmalloc(sizeof(hnswNode*) * max_links);
        if (!node->layers[i].links) {
            for (uint32_t j = 0; j < i; j++) hfree(node->layers[j].links);
            hfree(node->vector);
            hfree(node);
            return NULL;
        }
    }

    return node;
}

/* Free a node. */
void hnsw_node_free(hnswNode *node) {
    if (!node) return;

    for (uint32_t i = 0; i <= node->level; i++)
        hfree(node->layers[i].links);

    hfree(node->vector);
    hfree(node);
}

/* Free the entire index. */
void hnsw_free(HNSW *index,void(*free_value)(void*value)) {
    if (!index) return;

    hnswNode *current = index->head;
    while (current) {
        hnswNode *next = current->next;
        if (free_value) free_value(current->value);
        hnsw_node_free(current);
        current = next;
    }

    /* Destroy locks */
    pthread_rwlock_destroy(&index->global_lock);
    for (int i = 0; i < HNSW_MAX_THREADS; i++) {
        pthread_mutex_destroy(&index->slot_locks[i]);
    }

    hfree(index);
}

/* Add node to linked list of nodes. We may need to scan the whole
 * HNSW graph for several reasons. The list is doubly linked since we
 * also need the ability to remove a node without scanning the whole thing. */
void hnsw_add_node(HNSW *index, hnswNode *node) {
    node->next = index->head;
    node->prev = NULL;
    if (index->head)
        index->head->prev = node;
    index->head = node;
    index->node_count++;
}

/* Search the specified layer starting from the specified entry point
 * to collect 'ef' nodes that are near to 'query'.
 *
 * This function implements optional hybrid search, so that each node
 * can be accepted or not based on its associated value. In this case
 * a callback 'filter_callback' should be passed, together with a maximum
 * effort for the search (number of candidates to evaluate), since even
 * with a a low "EF" value we risk that there are too few nodes that satisfy
 * the provided filter, and we could trigger a full scan. */
pqueue *search_layer_with_filter(
                    HNSW *index, hnswNode *query, hnswNode *entry_point,
                    uint32_t ef, uint32_t layer, uint32_t slot,
                    int (*filter_callback)(void *value, void *privdata),
                    void *filter_privdata, uint32_t max_candidates)
{
    // Mark visited nodes with a never seen epoch.
    index->current_epoch[slot]++;

    pqueue *candidates = pq_new(HNSW_MAX_CANDIDATES);
    pqueue *results = pq_new(ef);
    if (!candidates || !results) {
        if (candidates) pq_free(candidates);
        if (results) pq_free(results);
        return NULL;
    }

    // Take track of the total effort: only used when filtering via
    // a callback to have a bound effort.
    uint32_t evaluated_candidates = 1;

    // Add entry point.
    float dist = hnsw_distance(index, query, entry_point);
    pq_push(candidates, entry_point, dist);
    if (filter_callback == NULL ||
        filter_callback(entry_point->value, filter_privdata))
    {
        pq_push(results, entry_point, dist);
    }
    entry_point->visited_epoch[slot] = index->current_epoch[slot];

    // Process candidates.
    while (candidates->count > 0) {
        // Max effort. If zero, we keep scanning.
        if (filter_callback &&
            max_candidates &&
            evaluated_candidates >= max_candidates) break;

        float cur_dist;
        hnswNode *current = pq_pop(candidates, &cur_dist);
        evaluated_candidates++;

        float furthest = pq_max_distance(results);
        if (results->count >= ef && cur_dist > furthest) break;

        /* Check neighbors. */
        for (uint32_t i = 0; i < current->layers[layer].num_links; i++) {
            hnswNode *neighbor = current->layers[layer].links[i];

            if (neighbor->visited_epoch[slot] == index->current_epoch[slot])
                continue; // Already visited during this scan.

            neighbor->visited_epoch[slot] = index->current_epoch[slot];
            float neighbor_dist = hnsw_distance(index, query, neighbor);

            furthest = pq_max_distance(results);
            if (filter_callback == NULL) {
                /* Original HNSW logic when no filtering:
                 * Add to results if better than current max or
                 * results not full. */
                if (neighbor_dist < furthest || results->count < ef) {
                    pq_push(candidates, neighbor, neighbor_dist);
                    pq_push(results, neighbor, neighbor_dist);
                }
            } else {
                /* With filtering: we add candidates even if doesn't match
                 * the filter, in order to continue to explore the graph. */
                if (neighbor_dist < furthest || candidates->count < ef) {
                    pq_push(candidates, neighbor, neighbor_dist);
                }

                /* Add results only if passes filter. */
                if (filter_callback(neighbor->value, filter_privdata)) {
                    if (neighbor_dist < furthest || results->count < ef) {
                        pq_push(results, neighbor, neighbor_dist);
                    }
                }
            }
        }
    }

    pq_free(candidates);
    return results;
}

/* Just a wrapper without hybrid search callback. */
pqueue *search_layer(HNSW *index, hnswNode *query, hnswNode *entry_point,
                     uint32_t ef, uint32_t layer, uint32_t slot)
{
    return search_layer_with_filter(index, query, entry_point, ef, layer, slot,
                                    NULL, NULL, 0);
}

/* This function is used in order to initialize a node allocated in the
 * function stack with the specified vector. The idea is that we can
 * easily use hnsw_distance() from a vector and the HNSW nodes this way:
 *
 * hnswNode myQuery;
 * hnsw_init_tmp_node(myIndex,&myQuery,0,some_vector);
 * hnsw_distance(&myQuery, some_hnsw_node);
 *
 * Make sure to later free the node with:
 *
 * hnsw_free_tmp_node(&myQuery,some_vector);
 * You have to pass the vector to the free function, because sometimes
 * hnsw_init_tmp_node() may just avoid allocating a vector at all,
 * just reusing 'some_vector' pointer.
 *
 * Return 0 on out of memory, 1 on success.
 */
int hnsw_init_tmp_node(HNSW *index, hnswNode *node, int is_normalized, const float *vector) {
    node->vector = NULL;

    /* Work on a normalized query vector if the input vector is
     * not normalized. */
    if (!is_normalized) {
        node->vector = hmalloc(sizeof(float)*index->vector_dim);
        if (node->vector == NULL) return 0;
        memcpy(node->vector,vector,sizeof(float)*index->vector_dim);
        hnsw_normalize_vector(node->vector,NULL,index->vector_dim);
    } else {
        node->vector = (float*)vector;
    }

    /* If quantization is enabled, our query fake node should be
     * quantized as well. */
    if (index->quant_type != HNSW_QUANT_NONE) {
        void *quants = hmalloc(hnsw_quants_bytes(index));
        if (quants == NULL) {
            if (node->vector != vector) hfree(node->vector);
            return 0;
        }
        switch(index->quant_type) {
        case HNSW_QUANT_Q8:
            quantize_to_q8(node->vector, quants, index->vector_dim, &node->quants_range);
            break;
        case HNSW_QUANT_BIN:
            quantize_to_bin(node->vector, quants, index->vector_dim);
        }
        if (node->vector != vector) hfree(node->vector);
        node->vector = quants;
    }
    return 1;
}

/* Free the stack allocated node initialized by hnsw_init_tmp_node(). */
void hnsw_free_tmp_node(hnswNode *node, const float *vector) {
    if (node->vector != vector) hfree(node->vector);
}

/* Return approximated K-NN items. Note that neighbors and distances
 * arrays must have space for at least 'k' items.
 * norm_query should be set to 1 if the query vector is already
 * normalized, otherwise, if 0, the function will copy the vector,
 * L2-normalize the copy and search using the normalized version.
 *
 * If the filter_privdata callback is passed, only elements passing the
 * specified filter (invoked with privdata and the value associated
 * to the node as arguments) are returned. In such case, if max_candidates
 * is not NULL, it represents the maximum number of nodes to explore, since
 * the search may be otherwise unbound if few or no elements pass the
 * filter. */
int hnsw_search_with_filter
               (HNSW *index, const float *query_vector, uint32_t k,
                hnswNode **neighbors, float *distances, uint32_t slot,
                int query_vector_is_normalized,
                int (*filter_callback)(void *value, void *privdata),
                void *filter_privdata, uint32_t max_candidates)

{
    if (!index || !query_vector || !neighbors || k == 0) return -1;
    if (!index->enter_point) return 0; // Empty index.

    /* Use a fake node that holds the query vector, this way we can
     * use our normal node to node distance functions when checking
     * the distance between query and graph nodes. */
    hnswNode query;
    if (hnsw_init_tmp_node(index,&query,query_vector_is_normalized,query_vector) == 0) return -1;

    // Start searching from the entry point.
    hnswNode *curr_ep = index->enter_point;

    /* Start from higher layer to layer 1 (layer 0 is handled later)
     * in the next section. Descend to the most similar node found
     * so far. */
    for (int lc = index->max_level; lc > 0; lc--) {
        pqueue *results = search_layer(index, &query, curr_ep, 1, lc, slot);
        if (!results) continue;

        if (results->count > 0) {
            curr_ep = pq_get_node(results,0);
        }
        pq_free(results);
    }

    /* Search bottom layer (the most densely populated) with ef = k */
    pqueue *results = search_layer_with_filter(
                        index, &query, curr_ep, k, 0, slot, filter_callback,
                        filter_privdata, max_candidates);
    if (!results) {
        hnsw_free_tmp_node(&query, query_vector);
        return -1;
    }

    /* Copy results. */
    uint32_t found = MIN(k, results->count);
    for (uint32_t i = 0; i < found; i++) {
        neighbors[i] = pq_get_node(results,i);
        if (distances) {
            distances[i] = pq_get_distance(results,i);
        }
    }

    pq_free(results);
    hnsw_free_tmp_node(&query, query_vector);
    return found;
}

/* Wrapper to hnsw_search_with_filter() when no filter is needed. */
int hnsw_search(HNSW *index, const float *query_vector, uint32_t k,
                hnswNode **neighbors, float *distances, uint32_t slot,
                int query_vector_is_normalized)
{
    return hnsw_search_with_filter(index,query_vector,k,neighbors,
                                   distances,slot,query_vector_is_normalized,
                                   NULL,NULL,0);
}

/* Rescan a node and update the wortst neighbor index.
 * The followinng two functions are variants of this function to be used
 * when links are added or removed: they may do less work than a full scan. */
void hnsw_update_worst_neighbor(HNSW *index, hnswNode *node, uint32_t layer) {
    float worst_dist = 0;
    uint32_t worst_idx = 0;
    for (uint32_t i = 0; i < node->layers[layer].num_links; i++) {
        float dist = hnsw_distance(index, node, node->layers[layer].links[i]);
        if (dist > worst_dist) {
            worst_dist = dist;
            worst_idx = i;
        }
    }
    node->layers[layer].worst_distance = worst_dist;
    node->layers[layer].worst_idx = worst_idx;
}

/* Update node worst neighbor distance information when a new neighbor
 * is added. */
void hnsw_update_worst_neighbor_on_add(HNSW *index, hnswNode *node, uint32_t layer, uint32_t added_index, float distance) {
    (void) index; // Unused but here for API symmetry.
    if (node->layers[layer].num_links == 1 ||           // First neighbor?
        distance > node->layers[layer].worst_distance)  // New worst?
    {
        node->layers[layer].worst_distance = distance;
        node->layers[layer].worst_idx = added_index;
    }
}

/* Update node worst neighbor distance information when a linked neighbor
 * is removed. */
void hnsw_update_worst_neighbor_on_remove(HNSW *index, hnswNode *node, uint32_t layer, uint32_t removed_idx)
{
    if (node->layers[layer].num_links == 0) {
        node->layers[layer].worst_distance = 0;
        node->layers[layer].worst_idx = 0;
    } else if (removed_idx == node->layers[layer].worst_idx) {
        hnsw_update_worst_neighbor(index,node,layer);
    } else if (removed_idx < node->layers[layer].worst_idx) {
        // Just update index if we removed element before worst.
        node->layers[layer].worst_idx--;
    }
}

/* We have a list of candidate nodes to link to the new node, when inserting
 * one. This function selects which nodes to link and performs the linking.
 *
 * Parameters:
 *
 * - 'candidates' is the priority queue of potential good nodes to link to the
 *   new node 'new_node'.
 * - 'required_links' is as many links we would like our new_node to get
 *   at the specified layer.
 * - 'aggressive' changes the strategy used to find good neighbors as follows:
 *
 * This function is called with aggressive=0 for all the layers, including
 * layer 0. When called like that, it will use the diversity of links and
 * quality of links checks before linking our new node with some candidate.
 *
 * However if the insert function finds that at layer 0, with aggressive=0,
 * few connections were made, it calls this function again with aggressiveness
 * levels greater up to 2.
 *
 * At aggressive=1, the diversity checks are disabled, and the candidate
 * node for linking is accepted even if it is nearest to an already accepted
 * neighbor than it is to the new node.
 *
 * When we link our new node by replacing the link of a candidate neighbor
 * that already has the max number of links, inevitably some other node loses
 * a connection (to make space for our new node link). In this case:
 *
 * 1. If such "dropped" node would remain with too little links, we try with
 *    some different neighbor instead, however as the 'aggressive' parameter
 *    has incremental values (0, 1, 2) we are more and more willing to leave
 *    the dropped node with fever connections.
 * 2. If aggressive=2, we will scan the candidate neighbor node links to
 *    find a different linked-node to replace, one better connected even if
 *    its distance is not the worse.
 *
 * Note: this function is also called during deletion of nodes in order to
 * provide certain nodes with additional links.
 */
void select_neighbors(HNSW *index, pqueue *candidates, hnswNode *new_node,
                      uint32_t layer, uint32_t required_links, int aggressive)
{
    for (uint32_t i = 0; i < candidates->count; i++) {
        hnswNode *neighbor = pq_get_node(candidates,i);
        if (neighbor == new_node) continue; // Don't link node with itself.

        /* Use our cached distance among the new node and the candidate. */
        float dist = pq_get_distance(candidates,i);

        /* First of all, since our links are all bidirectional, if the
         * new node for any reason has no longer room, or if it accumulated
         * the required number of links, return ASAP. */
        if (new_node->layers[layer].num_links >= new_node->layers[layer].max_links ||
            new_node->layers[layer].num_links >= required_links) return;

        /* If aggressive is true, it is possible that the new node
         * already got some link among the candidates (see the top comment,
         * this function gets re-called in case of too few links).
         * So we need to check if this candidate is already linked to
         * the new node. */
        if (aggressive) {
            int duplicated = 0;
            for (uint32_t j = 0; j < new_node->layers[layer].num_links; j++) {
                if (new_node->layers[layer].links[j] == neighbor) {
                    duplicated = 1;
                    break;
                }
            }
            if (duplicated) continue;
        }

        /* Diversity check. We accept new candidates
         * only if there is no element already accepted that is nearest
         * to the candidate than the new element itself.
         * However this check is disabled if we have pressure to find
         * new links (aggressive != 0) */
        if (!aggressive) {
            int diversity_failed = 0;
            for (uint32_t j = 0; j < new_node->layers[layer].num_links; j++) {
                float link_dist = hnsw_distance(index, neighbor,
                    new_node->layers[layer].links[j]);
                if (link_dist < dist) {
                    diversity_failed = 1;
                    break;
                }
            }
            if (diversity_failed) continue;
        }

        /* If potential neighbor node has space, simply add the new link.
         * We will have space as well. */
        uint32_t n = neighbor->layers[layer].num_links;
        if (n < neighbor->layers[layer].max_links) {
            /* Link candidate to new node. */
            neighbor->layers[layer].links[n] = new_node;
            neighbor->layers[layer].num_links++;

            /* Update candidate worst link info. */
            hnsw_update_worst_neighbor_on_add(index,neighbor,layer,n,dist);

            /* Link new node to candidate. */
            uint32_t new_links = new_node->layers[layer].num_links;
            new_node->layers[layer].links[new_links] = neighbor;
            new_node->layers[layer].num_links++;

            /* Update new node worst link info. */
            hnsw_update_worst_neighbor_on_add(index,new_node,layer,new_links,dist);
            continue;
        }

        /* ====================================================================
         * Replacing existing candidate neighbor link step.
         * ================================================================== */

        /* If we are here, our accepted candidate for linking is full.
         *
         * If new node is more distant to candidate than its current worst link
         * then we skip it: we would not be able to establish a bidirectional
         * connection without compromising link quality of candidate.
         *
         * At aggressiveness > 0 we don't care about this check. */
        if (!aggressive && dist >= neighbor->layers[layer].worst_distance)
            continue;

        /* We can add it: we are ready to replace the candidate neighbor worst
         * link with the new node, assuming certain conditions are met. */
        hnswNode *worst_node = neighbor->layers[layer].links[neighbor->layers[layer].worst_idx];

        /* The worst node linked to our candidate may remain too disconnected
         * if we remove the candidate node as its link. Let's check if
         * this is the case: */
        if (aggressive == 0 &&
            worst_node->layers[layer].num_links <= index->M/2)
            continue;

        /* Aggressive level = 1. It's ok if the node remains with just
         * HNSW_M/4 links. */
        else if (aggressive == 1 &&
                 worst_node->layers[layer].num_links <= index->M/4)
            continue;

        /* If aggressive is set to 2, then the new node we are adding failed
         * to find enough neighbors. We can't insert an almost orphaned new
         * node, so let's see if the target node has some other link
         * that is well connected in the graph: we could drop it instead
         * of the worst link. */
        if (aggressive == 2 && worst_node->layers[layer].num_links <=
            index->M/4)
        {
            /* Let's see if we can find at least a candidate link that
             * would remain with a few connections. Track the one
             * that is the farthest away (worst distance) from our candidate
             * neighbor (in order to remove the less interesting link). */
            worst_node = NULL;
            uint32_t worst_idx = 0;
            float max_dist = 0;
            for (uint32_t j = 0; j < neighbor->layers[layer].num_links; j++) {
                hnswNode *to_drop = neighbor->layers[layer].links[j];

                /* Skip this if it would remain too disconnected as well.
                 *
                 * NOTE about index->M/4 min connections requirement:
                 *
                 * It is not too strict, since leaving a node with just a
                 * single link does not just leave it too weakly connected, but
                 * also sometimes creates cycles with few disconnected
                 * nodes linked among them. */
                if (to_drop->layers[layer].num_links <= index->M/4) continue;

                float link_dist = hnsw_distance(index, neighbor, to_drop);
                if (worst_node == NULL || link_dist > max_dist) {
                    worst_node = to_drop;
                    max_dist = link_dist;
                    worst_idx = j;
                }
            }

            if (worst_node != NULL) {
                /* We found a node that we can drop. Let's pretend this is
                 * the worst node of the candidate to unify the following
                 * code path. Later we will fix the worst node info anyway. */
                neighbor->layers[layer].worst_distance = max_dist;
                neighbor->layers[layer].worst_idx = worst_idx;
            } else {
                /* Otherwise we have no other option than reallocating
                 * the max number of links for this target node, and
                 * ensure at least a few connections for our new node. */
                uint32_t reallocation_limit = layer == 0 ?
                    index->M * 3 : index->M *2;
                if (neighbor->layers[layer].max_links >= reallocation_limit)
                    continue;

                uint32_t new_max_links = neighbor->layers[layer].max_links+1;
                hnswNode **new_links = hrealloc(neighbor->layers[layer].links,
                                        sizeof(hnswNode*) * new_max_links);
                if (new_links == NULL) continue; // Non critical.

                /* Update neighbor's link capacity. */
                neighbor->layers[layer].links = new_links;
                neighbor->layers[layer].max_links = new_max_links;

                /* Establish bidirectional link. */
                uint32_t n = neighbor->layers[layer].num_links;
                neighbor->layers[layer].links[n] = new_node;
                neighbor->layers[layer].num_links++;
                hnsw_update_worst_neighbor_on_add(index, neighbor, layer,
                                                  n, dist);

                n = new_node->layers[layer].num_links;
                new_node->layers[layer].links[n] = neighbor;
                new_node->layers[layer].num_links++;
                hnsw_update_worst_neighbor_on_add(index, new_node, layer,
                                                  n, dist);
                continue;
            }
        }

        // Remove backlink from the worst node of our candidate.
        for (uint64_t j = 0; j < worst_node->layers[layer].num_links; j++) {
            if (worst_node->layers[layer].links[j] == neighbor) {
                memmove(&worst_node->layers[layer].links[j],
                        &worst_node->layers[layer].links[j+1],
                        (worst_node->layers[layer].num_links - j - 1) * sizeof(hnswNode*));
                worst_node->layers[layer].num_links--;
                hnsw_update_worst_neighbor_on_remove(index,worst_node,layer,j);
                break;
            }
        }

        /* Replace worst link with the new node. */
        neighbor->layers[layer].links[neighbor->layers[layer].worst_idx] = new_node;

        /* Update the worst link in the target node, at this point
         * the link that we replaced may no longer be the worst. */
        hnsw_update_worst_neighbor(index,neighbor,layer);

        // Add new node -> candidate link.
        uint32_t new_links = new_node->layers[layer].num_links;
        new_node->layers[layer].links[new_links] = neighbor;
        new_node->layers[layer].num_links++;

        // Update new node worst link.
        hnsw_update_worst_neighbor_on_add(index,new_node,layer,new_links,dist);
    }
}

/* This function implements node reconnection after a node deletion in HNSW.
 * When a node is deleted, other nodes at the specified layer lose one
 * connection (all the neighbors of the deleted node). This function attempts
 * to pair such nodes together in a way that maximizes connection quality
 * among the M nodes that were former neighbors of our deleted node.
 *
 * The algorithm works by first building a distance matrix among the nodes:
 *
 *     N0   N1   N2   N3
 * N0  0    1.2  0.4  0.9
 * N1  1.2  0    0.8  0.5
 * N2  0.4  0.8  0    1.1
 * N3  0.9  0.5  1.1  0
 *
 * For each potential pairing (i,j) we compute a score that combines:
 * 1. The direct cosine distance between the two nodes
 * 2. The average distance to other nodes that would no longer be
 *    available for pairing if we select this pair
 *
 * We want to balance local node-to-node requirements and global requirements.
 * For instance sometimes connecting A with B, while optimal, would leave
 * C and D to be connected without other choices, and this could be a very
 * bad connection. Maybe instead A and C and B and D are both relatively high
 * quality connections.
 *
 * The formula used to calculate the score of each connection is:
 *
 * score[i,j] = W1*(2-distance[i,j]) + W2*((new_avg_i + new_avg_j)/2)
 * where new_avg_x is the average of distances in row x excluding distance[i,j]
 *
 * So the score is directly proportional to the SIMILARITY of the two nodes
 * and also directly proportional to the DISTANCE of the potential other
 * connections that we lost by pairign i,j. So we have a cost for missed
 * opportunities, or better, in this case, a reward if the missing
 * opportunities are not so good (big average distance).
 *
 * W1 and W2 are weights (defaults: 0.7 and 0.3) that determine the relative
 * importance of immediate connection quality vs future pairing potential.
 *
 * After the initial pairing phase, any nodes that couldn't be paired
 * (due to odd count or existing connections) are handled by searching
 * the broader graph using the standard HNSW neighbor selection logic.
 */
void hnsw_reconnect_nodes(HNSW *index, hnswNode **nodes, int count, uint32_t layer) {
    if (count <= 0) return;
    debugmsg("Reconnecting %d nodes\n", count);

    /* Step 1: Build the distance matrix between all nodes.
     * Since distance(i,j) = distance(j,i), we only compute the upper triangle
     * and mirror it to the lower triangle. */
    float *distances = hmalloc((unsigned long) count * count * sizeof(float));
    if (!distances) return;

    for (int i = 0; i < count; i++) {
        distances[i*count + i] = 0;  // Distance to self is 0
        for (int j = i+1; j < count; j++) {
            float dist = hnsw_distance(index, nodes[i], nodes[j]);
            distances[i*count + j] = dist;     // Upper triangle.
            distances[j*count + i] = dist;     // Lower triangle.
        }
    }

    /* Step 2: Calculate row averages (will be used in scoring):
     * please note that we just calculate row averages and not
     * columns averages since the matrix is symmetrical, so those
     * are the same: check the image in the top comment if you have any
     * doubt about this. */
    float *row_avgs = hmalloc(count * sizeof(float));
    if (!row_avgs) {
        hfree(distances);
        return;
    }

    for (int i = 0; i < count; i++) {
        float sum = 0;
        int valid_count = 0;
        for (int j = 0; j < count; j++) {
            if (i != j) {
                sum += distances[i*count + j];
                valid_count++;
            }
        }
        row_avgs[i] = valid_count ? sum / valid_count : 0;
    }

    /* Step 3: Build scoring matrix. What we do here is to combine how
     * good is a given i,j nodes connection, with how badly connecting
     * i,j will affect the remaining quality of connections left to
     * pair the other nodes. */
    float *scores = hmalloc((unsigned long) count * count * sizeof(float));
    if (!scores) {
        hfree(distances);
        hfree(row_avgs);
        return;
    }

    /* Those weights were obtained manually... No guarantee that they
     * are optimal. However with these values the algorithm is certain
     * better than its greedy version that just attempts to pick the
     * best pair each time (verified experimentally). */
    const float W1 = 0.7;  // Weight for immediate distance.
    const float W2 = 0.3;  // Weight for future potential.

    for (int i = 0; i < count; i++) {
        for (int j = 0; j < count; j++) {
            if (i == j) {
                scores[i*count + j] = -1;  // Invalid pairing.
                continue;
            }

            // Check for existing connection between i and j.
            int already_linked = 0;
            for (uint32_t k = 0; k < nodes[i]->layers[layer].num_links; k++)
            {
                if (nodes[i]->layers[layer].links[k] == nodes[j]) {
                    scores[i*count + j] = -1;  // Already linked.
                    already_linked = 1;
                    break;
                }
            }
            if (already_linked) continue;

            float dist = distances[i*count + j];

            /* Calculate new averages excluding this pair.
             * Handle edge case where we might have too few elements.
             * Note that it would be not very smart to recompute the average
             * each time scanning the row, we can remove the element
             * and adjust the average without it. */
            float new_avg_i = 0, new_avg_j = 0;
            if (count > 2) {
                new_avg_i = (row_avgs[i] * (count-1) - dist) / (count-2);
                new_avg_j = (row_avgs[j] * (count-1) - dist) / (count-2);
            }

            /* Final weighted score: the more similar i,j, the better
             * the score. The more distant are the pairs we lose by
             * connecting i,j, the better the score. */
            scores[i*count + j] = W1*(2-dist) + W2*((new_avg_i + new_avg_j)/2);
        }
    }

    // Step 5: Pair nodes greedily based on scores.
    int *used = hmalloc(count*sizeof(int));
    memset(used,0,count*sizeof(int));
    if (!used) {
        hfree(distances);
        hfree(row_avgs);
        hfree(scores);
        return;
    }

    /* Scan the matrix looking each time for the potential
     * link with the best score. */
    while(1) {
        float max_score = -1;
        int best_j = -1, best_i = -1;

        // Seek best score i,j values.
        for (int i = 0; i < count; i++) {
            if (used[i]) continue;  // Already connected.

            /* No space left? Not possible after a node deletion but makes
             * this function more future-proof. */
            if (nodes[i]->layers[layer].num_links >=
                nodes[i]->layers[layer].max_links) continue;

            for (int j = 0; j < count; j++) {
                if (i == j) continue; // Same node, skip.
                if (used[j]) continue; // Already connected.
                float score = scores[i*count + j];
                if (score < 0) continue; // Invalid link.

                /* If the target node has space, and its score is better
                 * than any other seen so far... remember it is the best. */
                if (score > max_score &&
                    nodes[j]->layers[layer].num_links <
                    nodes[j]->layers[layer].max_links)
                {
                    // Track the best connection found so far.
                    max_score = score;
                    best_j = j;
                    best_i = i;
                }
            }
        }

        // Possible link found? Connect i and j.
        if (best_j != -1) {
            debugmsg("[%d] linking %d with %d: %f\n", layer, (int)best_i, (int)best_j, max_score);
            // Link i -> j.
            int link_idx = nodes[best_i]->layers[layer].num_links;
            nodes[best_i]->layers[layer].links[link_idx] = nodes[best_j];
            nodes[best_i]->layers[layer].num_links++;

            // Update worst distance if needed.
            float dist = distances[best_i*count + best_j];
            hnsw_update_worst_neighbor_on_add(index,nodes[best_i],layer,link_idx,dist);

            // Link j -> i.
            link_idx = nodes[best_j]->layers[layer].num_links;
            nodes[best_j]->layers[layer].links[link_idx] = nodes[best_i];
            nodes[best_j]->layers[layer].num_links++;

            // Update worst distance if needed.
            hnsw_update_worst_neighbor_on_add(index,nodes[best_j],layer,link_idx,dist);

            // Mark connection as used.
            used[best_i] = used[best_j] = 1;
        } else {
            break; // No more valid connections available.
        }
    }

    /* Step 6: Handle remaining unpaired nodes using the standard HNSW
     * neighbor selection. */
    for (int i = 0; i < count; i++) {
        if (used[i]) continue;

        // Skip if node is already at max connections.
        if (nodes[i]->layers[layer].num_links >=
            nodes[i]->layers[layer].max_links)
            continue;

        debugmsg("[%d] Force linking %d\n", layer, i);

        /* First, try with local nodes as candidates.
         * Some candidate may have space. */
        pqueue *candidates = pq_new(count);
        if (!candidates) continue;

        /* Add all the local nodes having some space as candidates
         * to be linked with this node. */
        for (int j = 0; j < count; j++) {
            if (i != j &&       // Must not be itself.
            nodes[j]->layers[layer].num_links <     // Must not be full.
            nodes[j]->layers[layer].max_links)
            {
                float dist = distances[i*count + j];
                pq_push(candidates, nodes[j], dist);
            }
        }

        /* Try local candidates first with aggressive = 1.
         * So we will link only if there is space.
         * We want one link more than the links we already have. */
        uint32_t wanted_links = nodes[i]->layers[layer].num_links+1;
        if (candidates->count > 0) {
            select_neighbors(index, candidates, nodes[i], layer,
                wanted_links, 1);
            debugmsg("Final links after attempt with local nodes: %d (wanted: %d)\n", (int)nodes[i]->layers[layer].num_links, wanted_links);
        }

        // If still no connection, search the broader graph.
        if (nodes[i]->layers[layer].num_links != wanted_links) {
            debugmsg("No force linking possible with local candidates\n");
            pq_free(candidates);

            // Find entry point for target layer by descending through levels.
            hnswNode *curr_ep = index->enter_point;
            for (uint32_t lc = index->max_level; lc > layer; lc--) {
                pqueue *results = search_layer(index, nodes[i], curr_ep, 1, lc, 0);
                if (results) {
                    if (results->count > 0) {
                        curr_ep = pq_get_node(results,0);
                    }
                    pq_free(results);
                }
            }

            if (curr_ep) {
                /* Search this layer for candidates.
                 * Use the default EF_C in this case, since it's not an
                 * "insert" operation, and we don't know the user
                 * specified "EF". */
                candidates = search_layer(index, nodes[i], curr_ep, HNSW_EF_C, layer, 0);
                if (candidates) {
                    /* Try to connect with aggressiveness proportional to the
                     * node linking condition. */
                    int aggressiveness =
                        (nodes[i]->layers[layer].num_links > index->M / 2)
                            ? 1 : 2;
                    select_neighbors(index, candidates, nodes[i], layer,
                                     wanted_links, aggressiveness);
                    debugmsg("Final links with broader search: %d (wanted: %d)\n", (int)nodes[i]->layers[layer].num_links, wanted_links);
                    pq_free(candidates);
                }
            }
        } else {
            pq_free(candidates);
        }
    }

    // Cleanup.
    hfree(distances);
    hfree(row_avgs);
    hfree(scores);
    hfree(used);
}

/* This is an helper function in order to support node deletion.
 * It's goal is just to:
 *
 * 1. Remove the node from the bidirectional links of neighbors in the graph.
 * 2. Remove the node from the linked list of nodes.
 * 3. Fix the entry point in the graph. We just select one of the neighbors
 *    of the deleted node at a lower level. If none is found, we do
 *    a full scan.
 * 4. The node itself amd its aux value field are NOT freed. It's up to the
 *    caller to do it, by using hnsw_node_free().
 * 5. The node associated value (node->value) is NOT freed.
 *
 * Why this function will not free the node? Because in node updates it
 * could be a good idea to reuse the node allocation for different reasons
 * (currently not implemented).
 * In general it is more future-proof to be able to reuse the node if
 * needed. Right now this library reuses the node only when links are
 * not touched (see hnsw_update() for more information). */
void hnsw_unlink_node(HNSW *index, hnswNode *node) {
    if (!index || !node) return;

    index->version++; // This node may be missing in an already compiled list
                      // of neighbors. Make optimistic concurrent inserts fail.

    /* Remove all bidirectional links at each level.
     * Note that in this implementation all the
     * links are guaranteed to be bedirectional. */

    /* For each level of the deleted node... */
    for (uint32_t level = 0; level <= node->level; level++) {
        /* For each linked node of the deleted node... */
        for (uint32_t i = 0; i < node->layers[level].num_links; i++) {
            hnswNode *linked = node->layers[level].links[i];
            /* Find and remove the backlink in the linked node */
            for (uint32_t j = 0; j < linked->layers[level].num_links; j++) {
                if (linked->layers[level].links[j] == node) {
                    /* Remove by shifting remaining links left */
                    memmove(&linked->layers[level].links[j],
                           &linked->layers[level].links[j + 1],
                           (linked->layers[level].num_links - j - 1) * sizeof(hnswNode*));
                    linked->layers[level].num_links--;
                    hnsw_update_worst_neighbor_on_remove(index,linked,level,j);
                    break;
                }
            }
        }
    }

    /* Update cursors pointing at this element. */
    if (index->cursors) hnsw_cursor_element_deleted(index,node);

    /* Update the previous node's next pointer. */
    if (node->prev) {
        node->prev->next = node->next;
    } else {
        /* If there's no previous node, this is the head. */
        index->head = node->next;
    }

    /* Update the next node's prev pointer. */
    if (node->next) node->next->prev = node->prev;

    /* Update node count. */
    index->node_count--;

    /* If this node was the enter_point, we need to update it. */
    if (node == index->enter_point) {
        /* Reset entry point - we'll find a new one (unless the HNSW is
         * now empty) */
        index->enter_point = NULL;
        index->max_level = 0;

        /* Step 1: Try to find a replacement by scanning levels
         * from top to bottom. Under normal conditions, if there is
         * any other node at the same level, we have a link. Anyway
         * we descend levels to find any neighbor at the higher level
         * possible. */
        for (int level = node->level; level >= 0; level--) {
            if (node->layers[level].num_links > 0) {
                index->enter_point = node->layers[level].links[0];
                break;
            }
        }

        /* Step 2: If no links were found at any level, do a full scan.
         * This should never happen in practice if the HNSW is not
         * empty. */
        if (!index->enter_point) {
            uint32_t new_max_level = 0;
            hnswNode *current = index->head;

            while (current) {
                if (current != node && current->level >= new_max_level) {
                    new_max_level = current->level;
                    index->enter_point = current;
                }
                current = current->next;
            }
        }

        /* Update max_level. */
        if (index->enter_point)
            index->max_level = index->enter_point->level;
    }

    /* Clear the node's links but don't free the node itself */
    node->prev = node->next = NULL;
}

/* Higher level API for hnsw_unlink_node() + hnsw_reconnect_nodes() actual work.
 * This will get the write lock, will delete the node, free it,
 * reconnect the node neighbors among themselves, and unlock again.
 * If free_value function pointer is not NULL, then the function provided is
 * used to free node->value.
 *
 * The function returns 0 on error (inability to acquire the lock), otherwise
 * 1 is returned. */
int hnsw_delete_node(HNSW *index, hnswNode *node, void(*free_value)(void*value)) {
    if (pthread_rwlock_wrlock(&index->global_lock) != 0) return 0;
    hnsw_unlink_node(index,node);
    if (free_value && node->value) free_value(node->value);

    /* Relink all the nodes orphaned of this node link.
     * Do it for all the levels. */
    for (unsigned int j = 0; j <= node->level; j++) {
        hnsw_reconnect_nodes(index, node->layers[j].links,
            node->layers[j].num_links, j);
    }
    hnsw_node_free(node);
    pthread_rwlock_unlock(&index->global_lock);
    return 1;
}

/* ============================ Threaded API ================================
 * Concurrent readers should use the following API to get a slot assigned
 * (and a lock, too), do their read-only call, and unlock the slot.
 *
 * There is a reason why read operations don't implement opaque transparent
 * locking directly on behalf of the user: when we return a result set
 * with hnsw_search(), we report a set of nodes. The caller will do something
 * with the nodes and the associated values, so the unlocking of the
 * slot should happen AFTER the result was already used, otherwise we may
 * have changes to the HNSW nodes as the result is being accessed. */

/* Try to acquire a read slot. Returns the slot number (0 to HNSW_MAX_THREADS-1)
 * on success, -1 on error (pthread mutex errors). */
int hnsw_acquire_read_slot(HNSW *index) {
    /* First try a non-blocking approach on all slots. */
    for (uint32_t i = 0; i < HNSW_MAX_THREADS; i++) {
        if (pthread_mutex_trylock(&index->slot_locks[i]) == 0) {
            if (pthread_rwlock_rdlock(&index->global_lock) != 0) {
                pthread_mutex_unlock(&index->slot_locks[i]);
                return -1;
            }
            return i;
        }
    }

    /* All trylock attempts failed, use atomic increment to select slot. */
    uint32_t slot = index->next_slot++ % HNSW_MAX_THREADS;

    /* Try to lock the selected slot. */
    if (pthread_mutex_lock(&index->slot_locks[slot]) != 0) return -1;

    /* Get read lock. */
    if (pthread_rwlock_rdlock(&index->global_lock) != 0) {
        pthread_mutex_unlock(&index->slot_locks[slot]);
        return -1;
    }

    return slot;
}

/* Release a previously acquired read slot: note that it is important that
 * nodes returned by hnsw_search() are accessed while the read lock is
 * still active, to be sure that nodes are not freed. */
void hnsw_release_read_slot(HNSW *index, int slot) {
    if (slot < 0 || slot >= HNSW_MAX_THREADS) return;
    pthread_rwlock_unlock(&index->global_lock);
    pthread_mutex_unlock(&index->slot_locks[slot]);
}

/* ============================ Nodes insertion =============================
 * We have an optimistic API separating the read-only candidates search
 * and the write side (actual node insertion). We internally also use
 * this API to provide the plain hnsw_insert() function for code unification. */

struct InsertContext {
    pqueue *level_queues[HNSW_MAX_LEVEL]; /* Candidates for each level. */
    hnswNode *node;         /* Pre-allocated node ready for insertion */
    uint64_t version;       /* Index version at preparation time. This is used
                             * for CAS-like locking during change commit. */
};

/* Optimistic insertion API.
 *
 * WARNING: Note that this is an internal function: users should call
 * hnsw_prepare_insert() instead.
 *
 * This is how it works: you use hnsw_prepare_insert() and it will return
 * a context where good candidate neighbors are already pre-selected.
 * This step only uses read locks.
 *
 * Then finally you try to actually commit the new node with
 * hnsw_try_commit_insert(): this time we will require a write lock, but
 * for less time than it would be otherwise needed if using directly
 * hnsw_insert(). When you try to commit the write, if no node was deleted in
 * the meantime, your operation will succeed, otherwise it will fail, and
 * you should try to just use the hnsw_insert() API, since there is
 * contention.
 *
 * See hnsw_node_new() for information about 'vector' and 'qvector'
 * arguments, and which one to pass. */
InsertContext *hnsw_prepare_insert_nolock(HNSW *index, const float *vector,
                const int8_t *qvector, float qrange, uint64_t id,
                int slot, int ef)
{
    InsertContext *ctx = hmalloc(sizeof(*ctx));
    if (!ctx) return NULL;

    memset(ctx, 0, sizeof(*ctx));
    ctx->version = index->version;

    /* Crete a new node that we may be able to insert into the
     * graph later, when calling the commit function. */
    uint32_t level = random_level();
    ctx->node = hnsw_node_new(index, id, vector, qvector, qrange, level, 1);
    if (!ctx->node) {
        hfree(ctx);
        return NULL;
    }

    hnswNode *curr_ep = index->enter_point;

    /* Empty graph, no need to collect candidates. */
    if (curr_ep == NULL) return ctx;

    /* Phase 1: Find good entry point on the highest level of the new
     * node we are going to insert. */
    for (unsigned int lc = index->max_level; lc > level; lc--) {
        pqueue *results = search_layer(index, ctx->node, curr_ep, 1, lc, slot);

        if (results) {
            if (results->count > 0) curr_ep = pq_get_node(results,0);
            pq_free(results);
        }
    }

    /* Phase 2: Collect a set of potential connections for each layer of
     * the new node. */
    for (int lc = MIN(level, index->max_level); lc >= 0; lc--) {
        pqueue *candidates =
            search_layer(index, ctx->node, curr_ep, ef, lc, slot);

        if (!candidates) continue;
        curr_ep = (candidates->count > 0) ? pq_get_node(candidates,0) : curr_ep;
        ctx->level_queues[lc] = candidates;
    }

    return ctx;
}

/* External API for hnsw_prepare_insert_nolock(), handling locking. */
InsertContext *hnsw_prepare_insert(HNSW *index, const float *vector,
                const int8_t *qvector, float qrange, uint64_t id,
                int ef)
{
    InsertContext *ctx;
    int slot = hnsw_acquire_read_slot(index);
    ctx = hnsw_prepare_insert_nolock(index,vector,qvector,qrange,id,slot,ef);
    hnsw_release_read_slot(index,slot);
    return ctx;
}

/* Free an insert context and all its resources. */
void hnsw_free_insert_context(InsertContext *ctx) {
    if (!ctx) return;
    for (uint32_t i = 0; i < HNSW_MAX_LEVEL; i++) {
        if (ctx->level_queues[i]) pq_free(ctx->level_queues[i]);
    }
    if (ctx->node) hnsw_node_free(ctx->node);
    hfree(ctx);
}

/* Commit a prepared insert operation. This function is a low level API that
 * should not be called by the user. See instead hnsw_try_commit_insert(), that
 * will perform the CAS check and acquire the write lock.
 *
 * See the top comment in hnsw_prepare_insert() for more information
 * on the optimistic insertion API.
 *
 * This function can't fail and always returns the pointer to the
 * just inserted node. Out of memory is not possible since no critical
 * allocation is never performed in this code path: we populate links
 * on already allocated nodes. */
hnswNode *hnsw_commit_insert_nolock(HNSW *index, InsertContext *ctx, void *value) {
    hnswNode *node = ctx->node;
    node->value = value;

    /* Handle first node case. */
    if (index->enter_point == NULL) {
        index->version++; // First node, make concurrent inserts fail.
        index->enter_point = node;
        index->max_level = node->level;
        hnsw_add_node(index, node);
        ctx->node = NULL; // So hnsw_free_insert_context() will not free it.
        hnsw_free_insert_context(ctx);
        return node;
    }

    /* Connect the node with near neighbors at each level. */
    for (int lc = MIN(node->level,index->max_level); lc >= 0; lc--) {
        if (ctx->level_queues[lc] == NULL) continue;

        /* Try to provide index->M connections to our node. The call
         * is not guaranteed to be able to provide all the links we would
         * like to have for the new node: they must be bi-directional, obey
         * certain quality checks, and so forth, so later there are further
         * calls to force the hand a bit if needed.
         *
         * Let's start with aggressiveness = 0. */
        select_neighbors(index, ctx->level_queues[lc], node, lc, index->M, 0);

        /* Layer 0 and too few connections? Let's be more aggressive. */
        if (lc == 0 && node->layers[0].num_links < index->M/2) {
            select_neighbors(index, ctx->level_queues[lc], node, lc,
                             index->M, 1);

            /* Still too few connections? Let's go to
             * aggressiveness level '2' in linking strategy. */
            if (node->layers[0].num_links < index->M/4) {
                select_neighbors(index, ctx->level_queues[lc], node, lc,
                                 index->M/4, 2);
            }
        }
    }

    /* If new node level is higher than current max, update entry point. */
    if (node->level > index->max_level) {
        index->version++; // Entry point changed, make concurrent inserts fail.
        index->enter_point = node;
        index->max_level = node->level;
    }

    /* Add node to the linked list. */
    hnsw_add_node(index, node);
    ctx->node = NULL; // So hnsw_free_insert_context() will not free the node.
    hnsw_free_insert_context(ctx);
    return node;
}

/* If the context obtained with hnsw_prepare_insert() is still valid
 * (nodes not deleted in the meantime) then add the new node to the HNSW
 * index and return its pointer. Otherwise NULL is returned and the operation
 * should be either performed with the blocking API hnsw_insert() or attempted
 * again. */
hnswNode *hnsw_try_commit_insert(HNSW *index, InsertContext *ctx, void *value) {
    /* Check if the version changed since preparation. Note that we
     * should access index->version under the write lock in order to
     * be sure we can safely commit the write: this is just a fast-path
     * in order to return ASAP without acquiring the write lock in case
     * the version changed. */
    if (ctx->version != index->version) {
        hnsw_free_insert_context(ctx);
        return NULL;
    }

    /* Try to acquire write lock. */
    if (pthread_rwlock_wrlock(&index->global_lock) != 0) {
        hnsw_free_insert_context(ctx);
        return NULL;
    }

    /* Check version again under write lock. */
    if (ctx->version != index->version) {
        pthread_rwlock_unlock(&index->global_lock);
        hnsw_free_insert_context(ctx);
        return NULL;
    }

    /* Commit the change: note that it's up to hnsw_commit_insert_nolock()
     * to free the insertion context. */
    hnswNode *node = hnsw_commit_insert_nolock(index, ctx, value);

    /* Release the write lock. */
    pthread_rwlock_unlock(&index->global_lock);
    return node;
}

/* Insert a new element into the graph.
 * See hnsw_node_new() for information about 'vector' and 'qvector'
 * arguments, and which one to pass.
 *
 * Return NULL on out of memory during insert. Otherwise the newly
 * inserted node pointer is returned. */
hnswNode *hnsw_insert(HNSW *index, const float *vector, const int8_t *qvector, float qrange, uint64_t id, void *value, int ef) {
    /* Write lock. We acquire the write lock even for the prepare()
     * operation (that is a read-only operation) since we want this function
     * to don't fail in the check-and-set stage of commit().
     *
     * Basically here we are using the optimistic API in a non-optimistinc
     * way in order to have a single insertion code in the implementation. */
    if (pthread_rwlock_wrlock(&index->global_lock) != 0) return NULL;

    // Prepare the insertion - note we pass slot 0 since we're single threaded.
    InsertContext *ctx = hnsw_prepare_insert_nolock(index, vector, qvector,
                                                   qrange, id, 0, ef);
    if (!ctx) {
        pthread_rwlock_unlock(&index->global_lock);
        return NULL;
    }

    // Commit the prepared insertion without version checking.
    hnswNode *node = hnsw_commit_insert_nolock(index, ctx, value);

    // Release write lock and return our node pointer.
    pthread_rwlock_unlock(&index->global_lock);
    return node;
}

/* Helper function for qsort call in hnsw_should_reuse_node(). */
static int compare_floats(const float *a, const float *b) {
    if (*a < *b) return 1;
    if (*a > *b) return -1;
    return 0;
}

/* This function determines if a node can be reused with a new vector by:
 *
 * 1. Computing average of worst 25% of current distances.
 * 2. Checking if at least 50% of new distances stay below this threshold.
 * 3. Requiring a minimum number of links for the check to be meaningful.
 *
 * This check is useful when we want to just update a node that already
 * exists in the graph. Often the new vector is a learned embedding generated
 * by some model, and the embedding represents some document that perhaps
 * changed just slightly compared to the past, so the new embedding will
 * be very nearby. We need to find a way do determine if the current node
 * neighbors (practically speaking its location in the grapb) are good
 * enough even with the new vector.
 *
 * XXX: this function needs improvements: successive updates to the same
 * node with more and more distant vectors will make the node drift away
 * from its neighbors. One of the additional metrics used could be
 * neighbor-to-neighbor distance, that represents a more absolute check
 * of fit for the new vector. */
int hnsw_should_reuse_node(HNSW *index, hnswNode *node, int is_normalized, const float *new_vector) {
    /* Step 1: Not enough links? Advice to avoid reuse. */
    const uint32_t min_links_for_reuse = 4;
    uint32_t layer0_connections = node->layers[0].num_links;
    if (layer0_connections < min_links_for_reuse) return 0;

    /* Step2: get all current distances and run our heuristic. */
    float *old_distances = hmalloc(sizeof(float) * layer0_connections);
    if (!old_distances) return 0;

    // Temporary node with the new vector, to simplify the next logic.
    hnswNode tmp_node;
    if (hnsw_init_tmp_node(index,&tmp_node,is_normalized,new_vector) == 0) {
        hfree(old_distances);
        return 0;
    }

    /* Get old dinstances and sort them to access the 25% worst
     * (bigger) ones. */
    for (uint32_t i = 0; i < layer0_connections; i++) {
        old_distances[i] = hnsw_distance(index, node, node->layers[0].links[i]);
    }
    qsort(old_distances, layer0_connections, sizeof(float),
          (int (*)(const void*, const void*))(&compare_floats));

    uint32_t count = (layer0_connections+3)/4; // 25% approx to larger int.
    if (count > layer0_connections) count = layer0_connections; // Futureproof.
    float worst_avg = 0;

    // Compute average of 25% worst dinstances.
    for (uint32_t i = 0; i < count; i++) worst_avg += old_distances[i];
    worst_avg /= count;
    hfree(old_distances);

    // Count how many new distances stay below the threshold.
    uint32_t good_distances = 0;
    for (uint32_t i = 0; i < layer0_connections; i++) {
        float new_dist = hnsw_distance(index, &tmp_node, node->layers[0].links[i]);
        if (new_dist <= worst_avg) good_distances++;
    }
    hnsw_free_tmp_node(&tmp_node,new_vector);

    /* At least 50% of the nodes should pass our quality test, for the
     * node to be reused. */
    return good_distances >= layer0_connections/2;
}

/**
 * Return a random node from the HNSW graph.
 *
 * This function performs a random walk starting from the entry point,
 * using only level 0 connections for navigation. It uses log^2(N) steps
 * to ensure proper mixing time.
 */

hnswNode *hnsw_random_node(HNSW *index, int slot) {
    if (index->node_count == 0 || index->enter_point == NULL)
        return NULL;

    (void)slot; // Unused, but we need the caller to acquire the lock.

    /* First phase: descend from max level to level 0 taking random paths.
     * Note that we don't need a more conservative log^2(N) steps for
     * proper mixing, since we already descend to a random cluster here. */
    hnswNode *current = index->enter_point;
    for (uint32_t level = index->max_level; level > 0; level--) {
        /* If current node doesn't have this level or no links, continue
	 * to lower level. */
        if (current->level < level || current->layers[level].num_links == 0)
            continue;

        /* Choose random neighbor at this level. */
        uint32_t rand_neighbor = rand() % current->layers[level].num_links;
        current = current->layers[level].links[rand_neighbor];
    }

    /* Second phase: at level 0, take log(N) * c random steps. */
    const int c = 3; // Multiplier for more thorough exploration.
    double logN = log2(index->node_count + 1);
    uint32_t num_walks = (uint32_t)(logN * c);

    /* Avoid the ping-pong effect: imagine there are just two nodes and
     * the number of walks selected is even. We will select always the
     * first element of the graph; conversely, if it is odd, we will always
     * select the other element. One way to add more selection randomness is
     * to randomly add '1' or '0' to the number of walks to perform. */
    num_walks += rand() & 1;

    // Perform random walk at level 0.
    for (uint32_t i = 0; i < num_walks; i++) {
        if (current->layers[0].num_links == 0) return current;

        // Choose random neighbor.
        uint32_t rand_neighbor = rand() % current->layers[0].num_links;
        current = current->layers[0].links[rand_neighbor];
    }
    return current;
}

/* ============================= Serialization ==============================
 *
 * TO SERIALIZE
 * ============
 *
 * To serialize on disk, you need to persist the vector dimension, number
 * of elements, and the quantization type index->quant_type. These are
 * global values for the whole index.
 *
 * Then, to serialize each node:
 *
 * call hnsw_serialize_node() with each node you find in the linked list
 * of nodes, starting at index->head (each node has a next pointer).
 * The function will return an hnswSerNode structure, you will need
 * to store the following on disk (for each node):
 *
 * - The sernode->vector data, that is sernode->vector_size bytes.
 * - The sernode->params array, that points to an array of uint64_t
 *   integers. There are sernode->params_count total items. These
 *   parameters contain everything there is to need about your node: how
 *   many levels it has, its ID, the list of neighbors for each level (as node
 *   IDs), and so forth.
 *
 * You need to to save your own node->value in some way as well, but it already
 * belongs to the user of the API, since, for this library, it's just a pointer,
 * so the user should know how to serialized its private data.
 *
 * RELOADING FROM DISK / NET
 * =========================
 *
 * When reloading nodes, you first load the index vector dimension and
 * quantization type, and create the index with:
 *
 * HNSW *hnsw_new(uint32_t vector_dim, uint32_t quant_type);
 *
 * Then you load back, for each node (you stored how many nodes you had)
 * the vector and the params array / count.
 * You also load the value associated with your node.
 *
 * At this point you add back the loaded elements into the index with:
 *
 * hnsw_insert_serialized(HNSW *index, void *vector, uint64_t params,
 *                        uint32_t params_len, void *value);
 *
 * Once you added all the nodes back, you need to resolve the pointers
 * (since so far they are added just with the node IDs as reference), so
 * you call:
 *
 * hnsw_deserialize_index(index);
 *
 * The index is now ready to be used like if it has been always in memory.
 *
 * DESIGN NOTES
 * ============
 *
 * Why this API does not just give you a binary blob to save? Because in
 * many systems (and in Redis itself) to save integers / floats can have
 * more interesting encodings that just storing a 64 bit value. Many vector
 * indexes will be small, and their IDs will be small numbers, so the storage
 * system can exploit that and use less disk space, less network bandwidth
 * and so forth.
 *
 * How is the data stored in these arrays of numbers? Oh well, we have
 * things that are obviously numbers like node ID, number of levels for the
 * node and so forth. Also each of our nodes have an unique incremental ID,
 * so we can store a node set of links in terms of linked node IDs. This
 * data is put directly in the loaded node pointer space! We just cast the
 * integer to the pointer (so THIS IS NOT SAFE for 32 bit systems). Then
 * we want to translate such IDs into pointers. To do that, we build an
 * hash table, then scan all the nodes again and fix all the links converting
 * the ID to the pointer. */

/* History of serialization versions:
 * version 0: the first implementation, lacking worst node id/info.
 * version 1: includes worst link id/info. */
#define HNSW_SERIALIZATION_VERSION 1

/* This is a special worst link index that is set when loading a serialized
 * node with version 0 (this version of the serialization lacked explicit
 * information about the worst link index/distance). This way, later, the
 * function that fixes a deserialized index will know to compute the worst
 * index info at runtime. */
#define HNSW_SER_WORSTLINK_MISSING UINT32_MAX

/* Return the serialized node information as specified in the top comment
 * above. Note that the returned information is true as long as the node
 * provided is not deleted or modified, so this function should be called
 * when there are no concurrent writes.
 *
 * The function hnsw_serialize_node() should be called in order to
 * free the result of this function. */
hnswSerNode *hnsw_serialize_node(HNSW *index, hnswNode *node) {
    /* The first step is calculating the number of uint64_t parameters
     * that we need in order to serialize the node. */
    uint32_t num_params = 0;
    num_params += 2;    // node ID, number of layers.
    for (uint32_t i = 0; i <= node->level; i++) {
        num_params += 2; // max_links and num_links info for this layer.
        num_params += node->layers[i].num_links; // The IDs of linked nodes.
        num_params += 1; // worst link id/distance parameter.
    }

    /* We use another 64bit value to store two floats that are about
     * the vector: l2 and quantization range (that is only used if the
     * vector is quantized). */
    num_params++;

    /* Allocate the return object and the parameters array. */
    hnswSerNode *sn = hmalloc(sizeof(hnswSerNode));
    if (sn == NULL) return NULL;
    sn->params = hmalloc(sizeof(uint64_t)*num_params);
    if (sn->params == NULL) {
        hfree(sn);
        return NULL;
    }

    /* Fill data. */
    sn->params_count = num_params;
    sn->vector = node->vector;
    sn->vector_size = hnsw_quants_bytes(index);

    uint32_t param_idx = 0;
    sn->params[param_idx++] = node->id;
    /* The second parameter contains information about the serialization
     * version of this node, the node level and some unused field:
     *
     * +--------+--------+--------+--------+
     * |VVVVVVVV|........|........|LLLLLLLL|
     * +--------+--------+--------+--------+
     *
     * V is the version, 8 bits.
     * L is the node level, 8 bits (but actually 16 is the max so far).
     * The middle two bytes are reserved for future uses. */
    sn->params[param_idx] = node->level & 0xff;
    sn->params[param_idx] |= HNSW_SERIALIZATION_VERSION << 24;
    param_idx++;
    for (uint32_t i = 0; i <= node->level; i++) {
        sn->params[param_idx++] = node->layers[i].num_links;
        sn->params[param_idx++] = node->layers[i].max_links;
        for (uint32_t j = 0; j < node->layers[i].num_links; j++) {
            sn->params[param_idx++] = node->layers[i].links[j]->id;
        }
        /* Since version 1: pack and store worst_idx and worst_distance. */
        uint32_t worst_distance_bits;
        memcpy(&worst_distance_bits, &node->layers[i].worst_distance,
               sizeof(float));
        uint64_t wi =
            (((uint64_t)worst_distance_bits) << 32) | node->layers[i].worst_idx;
        sn->params[param_idx++] = wi;
    }

    /* Store l2 and range as uint32_t, in a way that is endian-safe.
     * Note that in big endian archs both are reversed: integers and
     * also the bytes of floats, so they will match. */
    uint64_t l2_and_range;
    uint32_t l2_bits, range_bits;
    memcpy(&l2_bits,&node->l2,sizeof(float));
    memcpy(&range_bits,&node->quants_range,sizeof(float));
    l2_and_range = ((uint64_t)range_bits<<32) | l2_bits;

    sn->params[param_idx++] = l2_and_range;

    /* Better safe than sorry: */
    assert(param_idx == num_params);
    return sn;
}

/* This is needed in order to free hnsw_serialize_node() returned
 * structure. */
void hnsw_free_serialized_node(hnswSerNode *sn) {
    hfree(sn->params);
    hfree(sn);
}

/* Load a serialized node. See the top comment in this section of code
 * for the documentation about how to use this.
 *
 * The function returns NULL both on out of memory and if the remaining
 * parameters length does not match the number of links or other items
 * to load. */
hnswNode *hnsw_insert_serialized(HNSW *index, void *vector, uint64_t *params, uint32_t params_len, void *value)
{
    if (params_len < 2) return NULL;

    uint64_t id = params[0];
    /* Check the node serialization function for the specific layout
     * of param[1] fields. */
    uint32_t level = params[1] & 0xff;                  // Node level.
    uint32_t version = (params[1] & 0xff000000) >> 24;  // Format version.

    if (version > HNSW_SERIALIZATION_VERSION) return NULL;
    int has_worst_link_info = version > 0;

    /* Keep track of maximum ID seen while loading. */
    if (id >= index->last_id) index->last_id = id;

    /* Create node, passing vector data directly based on quantization type. */
    hnswNode *node;
    if (index->quant_type != HNSW_QUANT_NONE) {
        node = hnsw_node_new(index, id, NULL, vector, 0, level, 0);
    } else {
        node = hnsw_node_new(index, id, vector, NULL, 0, level, 0);
    }
    if (!node) return NULL;

    /* Load params array into the node. */
    uint32_t param_idx = 2;
    for (uint32_t i = 0; i <= level; i++) {
        /* Sanity check. */
        if (param_idx + 2 + has_worst_link_info > params_len) {
            hnsw_node_free(node);
            return NULL;
        }

        uint32_t num_links = params[param_idx++];
        uint32_t max_links = params[param_idx++];

        /* Sanity check: links should be less than max links and
         * in general a reasonable amount. */
        if (num_links > max_links || max_links > HNSW_MAX_M*4) {
            hnsw_node_free(node);
            return NULL;
        }

        /* If max_links is larger than current allocation, reallocate.
         * It could happen in select_neighbors() that we over-allocate the
         * node under very unlikely to happen conditions. */
        if (max_links > node->layers[i].max_links) {
            hnswNode **new_links = hrealloc(node->layers[i].links, 
                                         sizeof(hnswNode*) * max_links);
            if (!new_links) {
                hnsw_node_free(node);
                return NULL;
            }
            node->layers[i].links = new_links;
            node->layers[i].max_links = max_links;
        }
        node->layers[i].num_links = num_links;

        /* Sanity check. */
        if (param_idx + num_links + has_worst_link_info > params_len) {
            hnsw_node_free(node);
            return NULL;
        }

        /* Fill links for this layer with the IDs. Note that this
         * is going to not work in 32 bit systems. Deleting / adding-back
         * nodes can produce IDs larger than 2^32-1 even if we can't never
         * fit more than 2^32 nodes in a 32 bit system. */
        for (uint32_t j = 0; j < num_links; j++)
            node->layers[i].links[j] = (hnswNode*)params[param_idx++];

        if (has_worst_link_info) {
            uint64_t wi = params[param_idx++];
            uint32_t worst_idx = wi & 0xffffffff;
            uint32_t worst_distance_bits = wi >> 32;
            float worst_distance;
            memcpy(&worst_distance,&worst_distance_bits,sizeof(float));
            node->layers[i].worst_idx = worst_idx;
            node->layers[i].worst_distance = worst_distance;

            // Sanity check the worst ID range.
            if (node->layers[i].num_links > 0 &&
                node->layers[i].worst_idx >= node->layers[i].num_links)
            {
                hnsw_node_free(node);
                return NULL;
            }
        } else {
            node->layers[i].worst_idx = HNSW_SER_WORSTLINK_MISSING;
            node->layers[i].worst_distance = 0;
        }
    }

    /* Get l2 and quantization range. */
    if (param_idx >= params_len) {
        hnsw_node_free(node);
        return NULL;
    }

    /* Load l2 and range packed into an uint64_t in an endian safe way. */
    uint64_t l2_and_range = params[param_idx];
    uint32_t l2_bits, range_bits;
    l2_bits = l2_and_range & 0xffffffff;
    range_bits = l2_and_range >> 32;
    memcpy(&node->l2, &l2_bits, sizeof(float));
    memcpy(&node->quants_range, &range_bits, sizeof(float));

    node->value = value;
    hnsw_add_node(index, node);

    /* Keep track of higher node level and set the entry point to the
     * greatest level node seen so far: thanks to this check we don't
     * need to remember what our entry point was during serialization. */
    if (index->enter_point == NULL || level > index->max_level) {
        index->max_level = level;
        index->enter_point = node;
    }
    return node;
}

/* Integer hashing, used by hnsw_deserialize_index().
 * MurmurHash3's 64-bit finalizer function. */
uint64_t hnsw_hash_node_id(uint64_t id) {
    id ^= id >> 33;
    id *= 0xff51afd7ed558ccd;
    id ^= id >> 33;
    id *= 0xc4ceb9fe1a85ec53;
    id ^= id >> 33;
    return id;
}

/* Helper for duplicated link detection in hnsw_deserialize_index(). */
static int qsort_compare_pointers(const void *aptr, const void *bptr) {
    uintptr_t a = *((uintptr_t*)aptr);
    uintptr_t b = *((uintptr_t*)bptr);
    if (a > b) return 1;
    if (a < b) return -1;
    return 0;
}

/* Fix pointers of neighbors nodes: after loading the serialized nodes, the
 * neighbors links are just IDs (casted to pointers), instead of the actual
 * pointers. We need to resolve IDs into pointers.
 *
 * The two integers salt0 and salt1 are used to make the internal state
 * of the function unguessable to an external attacker, in order to protect
 * from corruptions. Show be two random numbers from /dev/urandom if possible
 * otherwise can be just 0,0 if the application is not security critical and
 * never processes untrusted inputs.
 *
 * Return 0 on error (out of memory or some ID that can't be resolved), 1 on
 * success. */
int hnsw_deserialize_index(HNSW *index, uint64_t salt0, uint64_t salt1) {
    /* We will use simple linear probing, so over-allocating is a good
     * idea: anyway this flat array of pointers will consume a fraction
     * of the memory of the loaded index. */
    uint64_t min_size = index->node_count*2;
    uint64_t table_size = 1;
    while(table_size < min_size) table_size <<= 1;

    hnswNode **table = hmalloc(sizeof(hnswNode*) * table_size);
    if (table == NULL) return 0;
    memset(table,0,sizeof(hnswNode*) * table_size);

    /* First pass: populate the ID -> pointer hash table. */
    hnswNode *node = index->head;
    while(node) {
        uint64_t bucket = hnsw_hash_node_id(node->id) & (table_size-1);
        for (uint64_t j = 0; j < table_size; j++) {
            if (table[bucket] == NULL) {
                table[bucket] = node;
                break;
            }
            bucket = (bucket+1) & (table_size-1);
        }
        node = node->next;
    }

    /* Second pass: fix pointers of all the neighbors links.
     * As we scan and fix the links, we also compute the accumulator
     * register "reciprocal", that is used in order to guarantee that all
     * the links are reciprocal.
     *
     * This is how it works, we hash (using a strong hash function) the
     * following key for each link that we see from A to B (or vice versa):
     *
     *      hash(salt || A || B || link-level)
     *
     * We always sort A and B, so the same link from A to B and from B to A
     * will hash the same. The we xor the result into the 128 bit accumulator.
     * If each link has its own backlink, the accumulator is guaranteed to
     * be zero at the end.
     *
     * Collisions are extremely unlikely to happen, and an external attacker
     * can't easily control the hash function output, since the salt is
     * unknown, and also there would be to control the pointers.
     *
     * This algorithm is O(1) for each node so it is basically free for
     * us, as we scan the list of nodes, and runs on constant and very
     * small memory. */
    uint64_t accumulator[2] = {0,0};

    node = index->head; // Rewind.
    while(node) {
        uint64_t this_node_id = node->id;
        for (uint32_t i = 0; i <= node->level; i++) {
            // Check if there are duplicated links: those are
            // also corruptions of the on-disk serialization format.
            if (node->layers[i].num_links > 0) {
                qsort(node->layers[i].links, node->layers[i].num_links,
                        sizeof(void*), qsort_compare_pointers);
                for (uint32_t j = 0; j < node->layers[i].num_links-1; j++) {
                    if (node->layers[i].links[j] == node->layers[i].links[j+1])
                        goto corrupted;
                }
            }

            // Resolve pointers.
            for (uint32_t j = 0; j < node->layers[i].num_links; j++) {
                uint64_t linked_id = (uint64_t) node->layers[i].links[j];

                // We can't link to our own node.
                if (linked_id == this_node_id) goto corrupted;

                // Compute accumulator for reciprocal links check.
                uint64_t mixed_h1, mixed_h2;
                secure_pair_mixer_128(salt0, salt1, this_node_id, linked_id, (uint64_t)i, &mixed_h1, &mixed_h2);

                accumulator[0] ^= mixed_h1;
                accumulator[1] ^= mixed_h2;

                // Fix links.
                uint64_t bucket = hnsw_hash_node_id(linked_id) & (table_size-1);
                hnswNode *neighbor = NULL;
                for (uint64_t k = 0; k < table_size; k++) {
                    if (table[bucket] && table[bucket]->id == linked_id) {
                        neighbor = table[bucket];
                        break;
                    }
                    bucket = (bucket+1) & (table_size-1);
                }

                /* The neighbor must exist and also exist at the right
                 * level. */
                if (neighbor == NULL || neighbor->level < i) {
                    /* Unresolved link! Either a bug in this code
                     * or broken serialization data. */
                    goto corrupted;
                }
                node->layers[i].links[j] = neighbor;
            }

            /* The worst link information was missing from older
             * serialization formats. Compute it on the fly if needed. */
            if (node->layers[i].worst_idx == HNSW_SER_WORSTLINK_MISSING) {
                hnsw_update_worst_neighbor(index,node,i);
            }
        }
        node = node->next;
    }

    /* Check that links are reciprocal, otherwise fail. */
    if (accumulator[0] || accumulator[1]) goto corrupted;

    /* Everything fine. Return success. */
    hfree(table);
    return 1;

corrupted:
    /* Some corruption error detected. */
    hfree(table);
    return 0;
}

/* ================================ Iterator ================================ */

/* Get a cursor that can be used as argument of hnsw_cursor_next() to iterate
 * all the elements that remain there from the start to the end of the
 * iteration, excluding newly added elements.
 *
 * The function returns NULL on out of memory. */
hnswCursor *hnsw_cursor_init(HNSW *index) {
    if (pthread_rwlock_wrlock(&index->global_lock) != 0) return NULL;
    hnswCursor *cursor = hmalloc(sizeof(*cursor));
    if (cursor == NULL) {
        pthread_rwlock_unlock(&index->global_lock);
        return NULL;
    }
    cursor->index = index;
    cursor->next = index->cursors;
    cursor->current = index->head;
    index->cursors = cursor;
    pthread_rwlock_unlock(&index->global_lock);
    return cursor;
}

/* Free the cursor. Can be called both at the end of the iteration, when
 * hnsw_cursor_next() returned NULL, or before. */
void hnsw_cursor_free(hnswCursor *cursor) {
    if (pthread_rwlock_wrlock(&cursor->index->global_lock) != 0) {
        // No easy way to recover from that. We will leak memory.
        return;
    }

    hnswCursor *x = cursor->index->cursors;
    hnswCursor *prev = NULL;
    while(x) {
        if (x == cursor) {
            if (prev)
                prev->next = cursor->next;
            else
                cursor->index->cursors = cursor->next;
            hfree(cursor);
            break;
        }
        x = x->next;
    }
    pthread_rwlock_unlock(&cursor->index->global_lock);
}

/* Acquire a lock to use the cursor. Returns 1 if the lock was acquired
 * with success, otherwise zero is returned. The returned element is
 * protected after calling hnsw_cursor_next() for all the time required to
 * access it, then hnsw_cursor_release_lock() should be called in order
 * to unlock the HNSW index. */
int hnsw_cursor_acquire_lock(hnswCursor *cursor) {
    return pthread_rwlock_rdlock(&cursor->index->global_lock) == 0;
}

/* Release the cursor lock, see hnsw_cursor_acquire_lock() top comment
 * for more information. */
void hnsw_cursor_release_lock(hnswCursor *cursor) {
    pthread_rwlock_unlock(&cursor->index->global_lock);
}

/* Return the next element of the HNSW. See hnsw_cursor_init() for
 * the guarantees of the function. */
hnswNode *hnsw_cursor_next(hnswCursor *cursor) {
    hnswNode *ret = cursor->current;
    if (ret) cursor->current = ret->next;
    return ret;
}

/* Called by hnsw_unlink_node() if there is at least an active cursor.
 * Will scan the cursors to see if any cursor is going to yield this
 * one, and in this case, updates the current element to the next. */
void hnsw_cursor_element_deleted(HNSW *index, hnswNode *deleted) {
    hnswCursor *x = index->cursors;
    while(x) {
        if (x->current == deleted) x->current = deleted->next;
        x = x->next;
    }
}

/* ============================ Debugging stuff ============================= */

/* Show stats about nodes connections. */
void hnsw_print_stats(HNSW *index) {
    if (!index || !index->head) {
        printf("Empty index or NULL pointer passed\n");
        return;
    }

    long long total_links = 0;
    int min_links = -1;         // We'll set this to first node's count.
    int isolated_nodes = 0;
    uint32_t node_count = 0;

    // Iterate through all nodes using the linked list.
    hnswNode *current = index->head;
    while (current) {
        // Count total links for this node across all layers.
        int node_total_links = 0;
        for (uint32_t layer = 0; layer <= current->level; layer++)
            node_total_links += current->layers[layer].num_links;

        // Update statistics.
        total_links += node_total_links;

        // Initialize or update minimum links.
        if (min_links == -1 || node_total_links < min_links) {
            min_links = node_total_links;
        }

        // Check if node is isolated (no links at all).
        if (node_total_links == 0) isolated_nodes++;

        node_count++;
        current = current->next;
    }

    // Print statistics
    printf("HNSW Graph Statistics:\n");
    printf("----------------------\n");
    printf("Total nodes: %u\n", node_count);
    if (node_count > 0) {
        printf("Average links per node: %.2f\n",
		(float)total_links / node_count);
        printf("Minimum links in a single node: %d\n", min_links);
        printf("Number of isolated nodes: %d (%.1f%%)\n",
               isolated_nodes,
               (float)isolated_nodes * 100 / node_count);
    }
}

/* Validate graph connectivity and link reciprocity. Takes pointers to store results:
 * - connected_nodes: will contain number of reachable nodes from entry point.
 * - reciprocal_links: will contain 1 if all links are reciprocal, 0 otherwise.
 * Returns 0 on success, -1 on error (NULL parameters and such).
 */
int hnsw_validate_graph(HNSW *index, uint64_t *connected_nodes, int *reciprocal_links) {
    if (!index || !connected_nodes || !reciprocal_links) return -1;
    if (!index->enter_point) {
        *connected_nodes = 0;
        *reciprocal_links = 1;  // Empty graph is valid.
        return 0;
    }

    // Initialize connectivity check.
    index->current_epoch[0]++;
    *connected_nodes = 0;
    *reciprocal_links = 1;

    // Initialize node stack.
    uint64_t stack_size = index->node_count;
    hnswNode **stack = hmalloc(sizeof(hnswNode*) * stack_size);
    if (!stack) return -1;
    uint64_t stack_top = 0;

    // Start from entry point.
    index->enter_point->visited_epoch[0] = index->current_epoch[0];
    (*connected_nodes)++;
    stack[stack_top++] = index->enter_point;

    // Process all reachable nodes.
    while (stack_top > 0) {
        hnswNode *current = stack[--stack_top];

        // Explore all neighbors at each level.
        for (uint32_t level = 0; level <= current->level; level++) {
            for (uint64_t i = 0; i < current->layers[level].num_links; i++) {
                hnswNode *neighbor = current->layers[level].links[i];

                // Check reciprocity.
                int found_backlink = 0;
                for (uint64_t j = 0; j < neighbor->layers[level].num_links; j++) {
                    if (neighbor->layers[level].links[j] == current) {
                        found_backlink = 1;
                        break;
                    }
                }
                if (!found_backlink) {
                    *reciprocal_links = 0;
                }

                // If we haven't visited this neighbor yet.
                if (neighbor->visited_epoch[0] != index->current_epoch[0]) {
                    neighbor->visited_epoch[0] = index->current_epoch[0];
                    (*connected_nodes)++;
                    if (stack_top < stack_size) {
                        stack[stack_top++] = neighbor;
                    } else {
                        // This should never happen in a valid graph.
                        hfree(stack);
                        return -1;
                    }
                }
            }
        }
    }

    hfree(stack);

    // Now scan for unreachable nodes and print debug info.
    printf("\nUnreachable nodes debug information:\n");
    printf("=====================================\n");

    hnswNode *current = index->head;
    while (current) {
        if (current->visited_epoch[0] != index->current_epoch[0]) {
            printf("\nUnreachable node found:\n");
            printf("- Node pointer: %p\n", (void*)current);
            printf("- Node ID: %llu\n", (unsigned long long)current->id);
            printf("- Node level: %u\n", current->level);

            // Print info about all its links at each level.
            for (uint32_t level = 0; level <= current->level; level++) {
                printf("  Level %u links (%u):\n", level,
                       current->layers[level].num_links);
                for (uint64_t i = 0; i < current->layers[level].num_links; i++) {
                    hnswNode *neighbor = current->layers[level].links[i];
                    // Check reciprocity for this specific link
                    int found_backlink = 0;
                    for (uint64_t j = 0; j < neighbor->layers[level].num_links; j++) {
                        if (neighbor->layers[level].links[j] == current) {
                            found_backlink = 1;
                            break;
                        }
                    }
                    printf("    - Link %llu: pointer=%p, id=%llu, visited=%s,recpr=%s\n",
                           (unsigned long long)i, (void*)neighbor,
                           (unsigned long long)neighbor->id,
                           neighbor->visited_epoch[0] == index->current_epoch[0] ?
                           "yes" : "no",
                           found_backlink ? "yes" : "no");
                }
            }
        }
        current = current->next;
    }

    printf("Total connected nodes: %llu\n", (unsigned long long)*connected_nodes);
    printf("All links are bi-directiona? %s\n", (*reciprocal_links)?"yes":"no");
    return 0;
}

/* Test graph recall ability by verifying each node can be found searching
 * for its own vector. This helps validate that the majority of nodes are
 * properly connected and easily reachable in the graph structure. Every
 * unreachable node is reported.
 *
 * Normally only a small percentage of nodes will be not reachable when
 * visited. This is expected and part of the statistical properties
 * of HNSW. This happens especially with entries that have an ambiguous
 * meaning in the represented space, and are across two or multiple clusters
 * of items.
 *
 * The function works by:
 * 1. Iterating through all nodes in the linked list
 * 2. Using each node's vector to perform a search with specified EF
 * 3. Verifying the node can find itself as nearest neighbor
 * 4. Collecting and reporting statistics about reachability
 *
 * This is just a debugging function that reports stuff in the standard
 * output, part of the implementation because this kind of functions
 * provide some visibility on what happens inside the HNSW.
 */
void hnsw_test_graph_recall(HNSW *index, int test_ef, int verbose) {
    // Stats
    uint32_t total_nodes = 0;
    uint32_t unreachable_nodes = 0;
    uint32_t perfectly_reachable = 0;  // Node finds itself as first result

    // For storing search results
    hnswNode **neighbors = hmalloc(sizeof(hnswNode*) * test_ef);
    float *distances = hmalloc(sizeof(float) * test_ef);
    float *test_vector = hmalloc(sizeof(float) * index->vector_dim);
    if (!neighbors || !distances || !test_vector) {
        hfree(neighbors);
        hfree(distances);
        hfree(test_vector);
        return;
    }

    // Get a read slot for searching (even if it's highly unlikely that
    // this test will be run threaded...).
    int slot = hnsw_acquire_read_slot(index);
    if (slot < 0) {
        hfree(neighbors);
        hfree(distances);
        return;
    }

    printf("\nTesting graph recall\n");
    printf("====================\n");

    // Process one node at a time using the linked list
    hnswNode *current = index->head;
    while (current) {
        total_nodes++;

        // If using quantization, we need to reconstruct the normalized vector
        if (index->quant_type == HNSW_QUANT_Q8) {
            int8_t *quants = current->vector;
            // Reconstruct normalized vector from quantized data
            for (uint32_t j = 0; j < index->vector_dim; j++) {
                test_vector[j] = (quants[j] * current->quants_range) / 127;
            }
        } else if (index->quant_type == HNSW_QUANT_NONE) {
            memcpy(test_vector,current->vector,sizeof(float)*index->vector_dim);
        } else {
            assert(0 && "Quantization type not supported.");
        }

        // Search using the node's own vector with high ef
        int found = hnsw_search(index, test_vector, test_ef, neighbors,
                              distances, slot, 1);

        if (found == 0) continue; // Empty HNSW?

        // Look for the node itself in the results
        int found_self = 0;
        int self_position = -1;
        for (int i = 0; i < found; i++) {
            if (neighbors[i] == current) {
                found_self = 1;
                self_position = i;
                break;
            }
        }

        if (!found_self || self_position != 0) {
            unreachable_nodes++;
            if (verbose) {
                if (!found_self)
                    printf("\nNode %s cannot find itself:\n", (char*)current->value);
                else
                    printf("\nNode %s is not top result:\n", (char*)current->value);
                printf("- Node ID: %llu\n", (unsigned long long)current->id);
                printf("- Node level: %u\n", current->level);
                printf("- Found %d neighbors but self not among them\n", found);
                printf("- Closest neighbor distance: %f\n", distances[0]);
                printf("- Neighbors: ");
                for (uint32_t i = 0; i < current->layers[0].num_links; i++) {
                    printf("%s ", (char*)current->layers[0].links[i]->value);
                }
                printf("\n");
                printf("\nFound instead: ");
                for (int j = 0; j < found && j < 10; j++) {
                    printf("%s ", (char*)neighbors[j]->value);
                }
                printf("\n");
            }
        } else {
            perfectly_reachable++;
        }
        current = current->next;
    }

    // Release read slot
    hnsw_release_read_slot(index, slot);

    // Free resources
    hfree(neighbors);
    hfree(distances);
    hfree(test_vector);

    // Print final statistics
    printf("Total nodes tested: %u\n", total_nodes);
    printf("Perfectly reachable nodes: %u (%.1f%%)\n",
           perfectly_reachable,
           total_nodes ? (float)perfectly_reachable * 100 / total_nodes : 0);
    printf("Unreachable/suboptimal nodes: %u (%.1f%%)\n",
           unreachable_nodes,
           total_nodes ? (float)unreachable_nodes * 100 / total_nodes : 0);
}

/* Return exact K-NN items by performing a linear scan of all nodes.
 * This function has the same signature as hnsw_search_with_filter() but
 * instead of using the graph structure, it scans all nodes to find the
 * true nearest neighbors.
 *
 * Note that neighbors and distances arrays must have space for at least 'k' items.
 * norm_query should be set to 1 if the query vector is already normalized.
 *
 * If the filter_callback is passed, only elements passing the specified filter
 * are returned. The slot parameter is ignored but kept for API consistency. */
int hnsw_ground_truth_with_filter
               (HNSW *index, const float *query_vector, uint32_t k,
                hnswNode **neighbors, float *distances, uint32_t slot,
                int query_vector_is_normalized,
                int (*filter_callback)(void *value, void *privdata),
                void *filter_privdata)
{
    /* Note that we don't really use the slot here: it's a linear scan.
     * Yet we want the user to acquire the slot as this will hold the
     * global lock in read only mode. */
    (void) slot;

    /* Take our query vector into a temporary node. */
    hnswNode query;
    if (hnsw_init_tmp_node(index, &query, query_vector_is_normalized, query_vector) == 0) return -1;

    /* Accumulate best results into a priority queue. */
    pqueue *results = pq_new(k);
    if (!results) {
        hnsw_free_tmp_node(&query, query_vector);
        return -1;
    }

    /* Scan all nodes linearly. */
    hnswNode *current = index->head;
    while (current) {
        /* Apply filter if needed. */
        if (filter_callback &&
            !filter_callback(current->value, filter_privdata))
        {
            current = current->next;
            continue;
        }

        /* Calculate distance to query. */
        float dist = hnsw_distance(index, &query, current);

        /* Add to results to pqueue. Will be accepted only if better than
         * the current worse or pqueue not full. */
        pq_push(results, current, dist);
        current = current->next;
    }

    /* Copy results to output arrays. */
    uint32_t found = MIN(k, results->count);
    for (uint32_t i = 0; i < found; i++) {
        neighbors[i] = pq_get_node(results, i);
        if (distances) distances[i] = pq_get_distance(results, i);
    }

    /* Clean up. */
    pq_free(results);
    hnsw_free_tmp_node(&query, query_vector);
    return found;
}
