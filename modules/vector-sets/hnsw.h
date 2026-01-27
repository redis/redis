/*
 * HNSW (Hierarchical Navigable Small World) Implementation
 * Based on the paper by Yu. A. Malkov, D. A. Yashunin
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 * Originally authored by: Salvatore Sanfilippo.
 */

#ifndef HNSW_H
#define HNSW_H

#include <pthread.h>
#include <stdatomic.h>

#define HNSW_DEFAULT_M  16     /* Used when 0 is given at creation time. */
#define HNSW_MIN_M      4      /* Probably even too low already. */
#define HNSW_MAX_M      4096   /* Safeguard sanity limit. */
#define HNSW_MAX_THREADS 32    /* Maximum number of concurrent threads */

/* Quantization types you can enable at creation time in hnsw_new() */
#define HNSW_QUANT_NONE  0   // No quantization.
#define HNSW_QUANT_Q8    1   // Q8 quantization.
#define HNSW_QUANT_BIN   2   // Binary quantization.

/* Layer structure for HNSW nodes. Each node will have from one to a few
 * of this depending on its level. */
typedef struct {
    struct hnswNode **links;  /* Array of neighbors for this layer */
    uint32_t num_links;       /* Number of used links */
    uint32_t max_links;       /* Maximum links for this layer. We may
                               * reallocate the node in very particular
                               * conditions in order to allow linking of
                               * new inserted nodes, so this may change
                               * dynamically and be > M*2 for a small set of
                               * nodes. */
    float worst_distance;     /* Distance to the worst neighbor */
    uint32_t worst_idx;       /* Index of the worst neighbor */
} hnswNodeLayer;

/* Node structure for HNSW graph */
typedef struct hnswNode {
    uint32_t level;         /* Node's maximum level */
    uint64_t id;            /* Unique identifier, may be useful in order to
                             * have a bitmap of visited notes to use as
                             * alternative to epoch / visited_epoch.
                             * Also used in serialization in order to retain
                             * links specifying IDs. */
    void *vector;           /* The vector, quantized or not. */
    float quants_range;     /* Quantization range for this vector:
                             * min/max values will be in the range
                             * -quants_range, +quants_range */
    float l2;               /* L2 before normalization. */

    /* Last time (epoch) this node was visited. We need one per thread.
     * This avoids having a different data structure where we track
     * visited nodes, but costs memory per node. */
    uint64_t visited_epoch[HNSW_MAX_THREADS];

    void *value;                    /* Associated value */
    struct hnswNode *prev, *next;   /* Prev/Next node in the list starting at
                                     * HNSW->head. */

    /* Links (and links info) per each layer. Note that this is part
     * of the node allocation to be more cache friendly: reliable 3% speedup
     * on Apple silicon, and does not make anything more complex. */
    hnswNodeLayer layers[];
} hnswNode;

struct HNSW;

/* It is possible to navigate an HNSW with a cursor that guarantees
 * visiting all the elements that remain in the HNSW from the start to the
 * end of the process (but not the new ones, so that the process will
 * eventually finish). Check hnsw_cursor_init(), hnsw_cursor_next() and
 * hnsw_cursor_free(). */
typedef struct hnswCursor {
    struct HNSW *index; // Reference to the index of this cursor.
    hnswNode *current;  // Element to report when hnsw_cursor_next() is called.
    struct hnswCursor *next; // Next cursor active.
} hnswCursor;

/* Main HNSW index structure */
typedef struct HNSW {
    hnswNode *enter_point;   /* Entry point for the graph */
    uint32_t M;               /* M as in the paper: layer 0 has M*2 max
                                 neighbors (M populated at insertion time)
                                 while all the other layers have M neighbors. */
    uint32_t max_level;      /* Current maximum level in the graph */
    uint32_t vector_dim;     /* Dimensionality of stored vectors */
    uint64_t node_count;     /* Total number of nodes */
    _Atomic uint64_t last_id; /* Last node ID used */
    uint64_t current_epoch[HNSW_MAX_THREADS];  /* Current epoch for visit tracking */
    hnswNode *head;             /* Linked list of nodes. Last first */

    /* We have two locks here:
     * 1. A global_lock that is used to perform write operations blocking all
     * the readers.
     * 2. One mutex per epoch slot, in order for read operations to acquire
     * a lock on a specific slot to use epochs tracking of visited nodes. */
    pthread_rwlock_t global_lock;  /* Global read-write lock */
    pthread_mutex_t slot_locks[HNSW_MAX_THREADS];  /* Per-slot locks */

    _Atomic uint32_t next_slot; /* Next thread slot to try */
    _Atomic uint64_t version;   /* Version for optimistic concurrency, this is
                                 * incremented on deletions and entry point
                                 * updates. */
    uint32_t quant_type;        /* Quantization used. HNSW_QUANT_... */
    hnswCursor *cursors;
} HNSW;

/* Serialized node. This structure is used as return value of
 * hnsw_serialize_node(). */
typedef struct hnswSerNode {
    void *vector;
    uint32_t vector_size;
    uint64_t *params;
    uint32_t params_count;
} hnswSerNode;

/* Insert preparation context */
typedef struct InsertContext InsertContext;

/* Core HNSW functions */
HNSW *hnsw_new(uint32_t vector_dim, uint32_t quant_type, uint32_t m);
void hnsw_free(HNSW *index,void(*free_value)(void*value));
void hnsw_node_free(hnswNode *node);
void hnsw_print_stats(HNSW *index);
hnswNode *hnsw_insert(HNSW *index, const float *vector, const int8_t *qvector,
                float qrange, uint64_t id, void *value, int ef);
int hnsw_search(HNSW *index, const float *query, uint32_t k,
                hnswNode **neighbors, float *distances, uint32_t slot,
                int query_vector_is_normalized);
int hnsw_search_with_filter
               (HNSW *index, const float *query_vector, uint32_t k,
                hnswNode **neighbors, float *distances, uint32_t slot,
                int query_vector_is_normalized,
                int (*filter_callback)(void *value, void *privdata),
                void *filter_privdata, uint32_t max_candidates);
void hnsw_get_node_vector(HNSW *index, hnswNode *node, float *vec);
int hnsw_delete_node(HNSW *index, hnswNode *node, void(*free_value)(void*value));
hnswNode *hnsw_random_node(HNSW *index, int slot);

/* Thread safety functions. */
int hnsw_acquire_read_slot(HNSW *index);
void hnsw_release_read_slot(HNSW *index, int slot);

/* Optimistic insertion API. */
InsertContext *hnsw_prepare_insert(HNSW *index, const float *vector, const int8_t *qvector, float qrange, uint64_t id, int ef);
hnswNode *hnsw_try_commit_insert(HNSW *index, InsertContext *ctx, void *value);
void hnsw_free_insert_context(InsertContext *ctx);

/* Serialization. */
hnswSerNode *hnsw_serialize_node(HNSW *index, hnswNode *node);
void hnsw_free_serialized_node(hnswSerNode *sn);
hnswNode *hnsw_insert_serialized(HNSW *index, void *vector, uint64_t *params, uint32_t params_len, void *value);
int hnsw_deserialize_index(HNSW *index, uint64_t salt0, uint64_t salt1);

// Helper function in case the user wants to directly copy
// the vector bytes.
uint32_t hnsw_quants_bytes(HNSW *index);

/* Cursors. */
hnswCursor *hnsw_cursor_init(HNSW *index);
void hnsw_cursor_free(hnswCursor *cursor);
hnswNode *hnsw_cursor_next(hnswCursor *cursor);
int hnsw_cursor_acquire_lock(hnswCursor *cursor);
void hnsw_cursor_release_lock(hnswCursor *cursor);

/* Allocator selection. */
void hnsw_set_allocator(void (*free_ptr)(void*), void *(*malloc_ptr)(size_t),
                        void *(*realloc_ptr)(void*, size_t));

/* Testing. */
int hnsw_validate_graph(HNSW *index, uint64_t *connected_nodes, int *reciprocal_links);
void hnsw_test_graph_recall(HNSW *index, int test_ef, int verbose);
float hnsw_distance(HNSW *index, hnswNode *a, hnswNode *b);
int hnsw_ground_truth_with_filter
               (HNSW *index, const float *query_vector, uint32_t k,
                hnswNode **neighbors, float *distances, uint32_t slot,
                int query_vector_is_normalized,
                int (*filter_callback)(void *value, void *privdata),
                void *filter_privdata);

#endif /* HNSW_H */
