/* Redis implementation for vector sets. The data structure itself
 * is implemented in hnsw.c.
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 * Originally authored by: Salvatore Sanfilippo.
 *
 * ======================== Understand threading model =========================
 * This code implements threaded operarations for two of the commands:
 *
 * 1. VSIM, by default.
 * 2. VADD, if the CAS option is specified.
 *
 * Note that even if the second operation, VADD, is a write operation, only
 * the neighbors collection for the new node is performed in a thread: then,
 * the actual insert is performed in the reply callback VADD_CASReply(),
 * which is executed in the main thread.
 *
 * Threaded operations need us to protect various operations with mutexes,
 * even if a certain degree of protection is already provided by the HNSW
 * library. Here are a few very important things about this implementation
 * and the way locking is performed.
 *
 * 1. All the write operations are performed in the main Redis thread:
 *    this also include VADD_CASReply() callback, that is called by Redis
 *    internals only in the context of the main thread. However the HNSW
 *    library allows background threads in hnsw_search() (VSIM) to modify
 *    nodes metadata to speedup search (to understand if a node was already
 *    visited), but this only happens after acquiring a specific lock
 *    for a given "read slot".
 *
 * 2. We use a global lock for each Vector Set object, called "in_use". This
 *    lock is a read-write lock, and is acquired in read mode by all the
 *    threads that perform reads in the background. It is only acquired in
 *    write mode by vectorSetWaitAllBackgroundClients(): the function acquires
 *    the lock and immediately releases it, with the effect of waiting all the
 *    background threads still running from ending their execution.
 *
 *    Note that no thread can be spawned, since we only call
 *    vectorSetWaitAllBackgroundClients() from the main Redis thread, that
 *    is also the only thread spawning other threads.
 *
 *    vectorSetWaitAllBackgroundClients() is used in two ways:
 *    A) When we need to delete a vector set because of (DEL) or other
 *       operations destroying the object, we need to wait that all the
 *       background threads working with this object finished their work.
 *    B) When we modify the HNSW nodes bypassing the normal locking
 *       provided by the HNSW library. This only happens when we update
 *       an existing node attribute so far, in VSETATTR and when we call
 *       VADD to update a node with the SETATTR option.
 *
 *  3. Often during read operations performed by Redis commands in the
 *     main thread (VCARD, VEMB, VRANDMEMBER, ...) we don't acquire any
 *     lock at all. The commands run in the main Redis thread, we can only
 *     have, at the same time, background reads against the same data
 *     structure. Note that VSIM_thread() and VADD_thread() still modify the
 *     read slot metadata, that is node->visited_epoch[slot], but as long as
 *     our read commands running in the main thread don't need to use
 *     hnsw_search() or other HNSW functions using the visited epochs slots
 *     we are safe.
 *
 * 4. There is a race from the moment we create a thread, passing the
 *    vector set object, to the moment the thread can actually lock the
 *    result win the in_use_lock mutex: as the thread starts, in the meanwhile
 *    a DEL/expire could trigger and remove the object. For this reason
 *    we use an atomic counter that protects our object for this small
 *    time in vectorSetWaitAllBackgroundClients(). This prevents removal
 *    of objects that are about to be taken by threads.
 *
 *    Note that other competing solutions could be used to fix the problem
 *    but have their set of issues, however they are worth documenting here
 *    and evaluating in the future:
 *
 *      A. Using a conditional variable we could "wait" for the thread to
 *         acquire the lock. However this means waiting before returning
 *         to the event loop, and would make the command execution slower.
 *      B. We could use again an atomic variable, like we did, but this time
 *         as a refcount for the object, with a vsetAcquire() vsetRelease().
 *         In this case, the command could retain the object in the main thread
 *         before starting the thread, and the thread, after the work is done,
 *         could release it. This way sometimes the object would be freed by
 *         the thread, and it's while now can be safe to do the kind of resource
 *         deallocation that vectorSetReleaseObject() does, given that the
 *         Redis Modules API is not always thread safe this solution may not
 *         be future-proof. However there is to evaluate it better in the
 *         future.
 *      C. We could use the "B" solution but instead of freeing the object
 *         in the thread, in this specific case we could just put it into a
 *         list and defer it for later freeing (for instance in the reply
 *         callback), so that the object is always freed in the main thread.
 *         This would require a list of objects to free.
 *
 *    However the current solution only disadvantage is the potential busy
 *    loop, but this busy loop in practical terms will almost never do
 *    much: to trigger it, a number of circumnstances must happen: deleting
 *    Vector Set keys while using them, hitting the small window needed to
 *    start the thread and read-lock the mutex.
 */

#define _DEFAULT_SOURCE
#define _USE_MATH_DEFINES
#define _POSIX_C_SOURCE 200809L

#include "../../src/redismodule.h"
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include "hnsw.h"
#include "vset_config.h"

// We inline directly the expression implementation here so that building
// the module is trivial.
#include "expr.c"

static RedisModuleType *VectorSetType;
static uint64_t VectorSetTypeNextId = 0;

// Default EF value if not specified during creation.
#define VSET_DEFAULT_C_EF 200

// Default EF value if not specified during search.
#define VSET_DEFAULT_SEARCH_EF 100

// Default num elements returned by VSIM.
#define VSET_DEFAULT_COUNT 10

/* ========================== Internal data structure  ====================== */

/* Our abstract data type needs a dual representation similar to Redis
 * sorted set: the proximity graph, and also a element -> graph-node map
 * that will allow us to perform deletions and other operations that have
 * as input the element itself. */
struct vsetObject {
    HNSW *hnsw;                 // Proximity graph.
    RedisModuleDict *dict;      // Element -> node mapping.
    float *proj_matrix;         // Random projection matrix, NULL if no projection
    uint32_t proj_input_size;     // Input dimension after projection.
                                  // Output dimension is implicit in
                                  // hnsw->vector_dim.
    pthread_rwlock_t in_use_lock; // Lock needed to destroy the object safely.
    uint64_t id;                // Unique ID used by threaded VADD to know the
                                // object is still the same.
    uint64_t numattribs;        // Number of nodes associated with an attribute.
    atomic_int thread_creation_pending; // Number of threads that are currently
                                        // pending to lock the object.
};

/* Each node has two associated values: the associated string (the item
 * in the set) and potentially a JSON string, that is, the attributes, used
 * for hybrid search with the VSIM FILTER option. */
struct vsetNodeVal {
    RedisModuleString *item;
    RedisModuleString *attrib;
};

/* Count the number of set bits in an integer (population count/Hamming weight).
 * This is a portable implementation that doesn't rely on compiler
 * extensions. */
static inline uint32_t bit_count(uint32_t n) {
    uint32_t count = 0;
    while (n) {
        count += n & 1;
        n >>= 1;
    }
    return count;
}

/* Create a Hadamard-based projection matrix for dimensionality reduction.
 * Uses {-1, +1} entries with a pattern based on bit operations.
 * The pattern is matrix[i][j] = (i & j) % 2 == 0 ? 1 : -1
 * Matrix is scaled by 1/sqrt(input_dim) for normalization.
 * Returns NULL on allocation failure.
 *
 * Note that compared to other approaches (random gaussian weights), what
 * we have here is deterministic, it means that our replicas will have
 * the same set of weights. Also this approach seems to work much better
 * in practice, and the distances between elements are better guaranteed.
 *
 * Note that we still save the projection matrix in the RDB file, because
 * in the future we may change the weights generation, and we want everything
 * to be backward compatible. */
float *createProjectionMatrix(uint32_t input_dim, uint32_t output_dim) {
    float *matrix = RedisModule_Alloc(sizeof(float) * input_dim * output_dim);

    /* Scale factor to normalize the projection. */
    const float scale = 1.0f / sqrt(input_dim);

    /* Fill the matrix using Hadamard pattern. */
    for (uint32_t i = 0; i < output_dim; i++) {
        for (uint32_t j = 0; j < input_dim; j++) {
            /* Calculate position in the flattened matrix. */
            uint32_t pos = i * input_dim + j;

            /* Hadamard pattern: use bit operations to determine sign
             * If the count of 1-bits in the bitwise AND of i and j is even,
             * the value is 1, otherwise -1. */
            int value = (bit_count(i & j) % 2 == 0) ? 1 : -1;

            /* Store the scaled value. */
            matrix[pos] = value * scale;
        }
    }
    return matrix;
}

/* Apply random projection to input vector. Returns new allocated vector. */
float *applyProjection(const float *input, const float *proj_matrix,
                      uint32_t input_dim, uint32_t output_dim)
{
    float *output = RedisModule_Alloc(sizeof(float) * output_dim);

    for (uint32_t i = 0; i < output_dim; i++) {
        const float *row = &proj_matrix[i * input_dim];
        float sum = 0.0f;
        for (uint32_t j = 0; j < input_dim; j++) {
            sum += row[j] * input[j];
        }
        output[i] = sum;
    }
    return output;
}

/* Create the vector as HNSW+Dictionary combined data structure. */
struct vsetObject *createVectorSetObject(unsigned int dim, uint32_t quant_type, uint32_t hnsw_M) {
    struct vsetObject *o;
    o = RedisModule_Alloc(sizeof(*o));

    o->id = VectorSetTypeNextId++;
    o->hnsw = hnsw_new(dim,quant_type,hnsw_M);
    if (!o->hnsw) { // May fail because of mutex creation.
        RedisModule_Free(o);
        return NULL;
    }

    o->dict = RedisModule_CreateDict(NULL);
    o->proj_matrix = NULL;
    o->proj_input_size = 0;
    o->numattribs = 0;
    o->thread_creation_pending = 0;
    RedisModule_Assert(pthread_rwlock_init(&o->in_use_lock,NULL) == 0);
    return o;
}

void vectorSetReleaseNodeValue(void *v) {
    struct vsetNodeVal *nv = v;
    RedisModule_FreeString(NULL,nv->item);
    if (nv->attrib) RedisModule_FreeString(NULL,nv->attrib);
    RedisModule_Free(nv);
}

/* Free the vector set object. */
void vectorSetReleaseObject(struct vsetObject *o) {
    if (!o) return;
    if (o->hnsw) hnsw_free(o->hnsw,vectorSetReleaseNodeValue);
    if (o->dict) RedisModule_FreeDict(NULL,o->dict);
    if (o->proj_matrix) RedisModule_Free(o->proj_matrix);
    pthread_rwlock_destroy(&o->in_use_lock);
    RedisModule_Free(o);
}

/* Wait for all the threads performing operations on this
 * index to terminate their work (locking for write will
 * wait for all the other threads).
 *
 * if 'for_del' is set to 1, we also wait for all the pending threads
 * that still didn't acquire the lock to finish their work. This
 * is useful only if we are going to call this function to delete
 * the object, and not if we want to just to modify it. */
void vectorSetWaitAllBackgroundClients(struct vsetObject *vset, int for_del) {
    if (for_del) {
        // If we are going to destroy the object, after this call, let's
        // wait for threads that are being created and still didn't had
        // a chance to acquire the lock.
        while (vset->thread_creation_pending > 0);
    }
    RedisModule_Assert(pthread_rwlock_wrlock(&vset->in_use_lock) == 0);
    pthread_rwlock_unlock(&vset->in_use_lock);
}

/* Return a string representing the quantization type name of a vector set. */
const char *vectorSetGetQuantName(struct vsetObject *o) {
    switch(o->hnsw->quant_type) {
    case HNSW_QUANT_NONE: return "f32";
    case HNSW_QUANT_Q8: return "int8";
    case HNSW_QUANT_BIN: return "bin";
    default: return "unknown";
    }
}

/* Insert the specified element into the Vector Set.
 * If update is '1', the existing node will be updated.
 *
 * Returns 1 if the element was added, or 0 if the element was already there
 * and was just updated. */
int vectorSetInsert(struct vsetObject *o, float *vec, int8_t *qvec, float qrange, RedisModuleString *val, RedisModuleString *attrib, int update, int ef)
{
    hnswNode *node = RedisModule_DictGet(o->dict,val,NULL);
    if (node != NULL) {
        if (update) {
            /* Wait for clients in the background: background VSIM
             * operations touch the nodes attributes we are going
             * to touch. */
            vectorSetWaitAllBackgroundClients(o,0);

            struct vsetNodeVal *nv = node->value;
            /* Pass NULL as value-free function. We want to reuse
             * the old value. */
            hnsw_delete_node(o->hnsw, node, NULL);
            node = hnsw_insert(o->hnsw,vec,qvec,qrange,0,nv,ef);
            RedisModule_Assert(node != NULL);
            RedisModule_DictReplace(o->dict,val,node);

            /* If attrib != NULL, the user wants that in case of an update we
             * update the attribute as well (otherwise it remains as it was).
             * Note that the order of operations is conceinved so that it
             * works in case the old attrib and the new attrib pointer is the
             * same. */
            if (attrib) {
                // Empty attribute string means: unset the attribute during
                // the update.
                size_t attrlen;
                RedisModule_StringPtrLen(attrib,&attrlen);
                if (attrlen != 0) {
                    RedisModule_RetainString(NULL,attrib);
                    o->numattribs++;
                } else {
                    attrib = NULL;
                }

                if (nv->attrib) {
                    o->numattribs--;
                    RedisModule_FreeString(NULL,nv->attrib);
                }
                nv->attrib = attrib;
            }
        }
        return 0;
    }

    struct vsetNodeVal *nv = RedisModule_Alloc(sizeof(*nv));
    nv->item = val;
    nv->attrib = attrib;
    node = hnsw_insert(o->hnsw,vec,qvec,qrange,0,nv,ef);
    if (node == NULL) {
        // XXX Technically in Redis-land we don't have out of memory, as we
        // crash on OOM. However the HNSW library may fail for error in the
        // locking libc call. Probably impossible in practical terms.
        RedisModule_Free(nv);
        return 0;
    }
    if (attrib != NULL) o->numattribs++;
    RedisModule_DictSet(o->dict,val,node);
    RedisModule_RetainString(NULL,val);
    if (attrib) RedisModule_RetainString(NULL,attrib);
    return 1;
}

/* Parse vector from FP32 blob or VALUES format, with optional REDUCE.
 * Format: [REDUCE dim] FP32|VALUES ...
 * Returns allocated vector and sets dimension in *dim.
 * If reduce_dim is not NULL, sets it to the requested reduction dimension.
 * Returns NULL on parsing error.
 *
 * The function sets as a reference *consumed_args, so that the caller
 * knows how many arguments we consumed in order to parse the input
 * vector. Remaining arguments are often command options. */
float *parseVector(RedisModuleString **argv, int argc, int start_idx,
                  size_t *dim, uint32_t *reduce_dim, int *consumed_args)
{
    int consumed = 0; // Arguments consumed

    /* Check for REDUCE option first. */
    if (reduce_dim) *reduce_dim = 0;
    if (reduce_dim && argc > start_idx + 2 &&
        !strcasecmp(RedisModule_StringPtrLen(argv[start_idx],NULL),"REDUCE"))
    {
        long long rdim;
        if (RedisModule_StringToLongLong(argv[start_idx+1],&rdim)
            != REDISMODULE_OK || rdim <= 0)
        {
            return NULL;
        }
        if (reduce_dim) *reduce_dim = rdim;
        start_idx += 2;  // Skip REDUCE and its argument.
        consumed += 2;
    }

    /* Now parse the vector format as before. */
    float *vec = NULL;
    const char *vec_format = RedisModule_StringPtrLen(argv[start_idx],NULL);

    if (!strcasecmp(vec_format,"FP32")) {
        if (argc < start_idx + 2) return NULL;  // Need FP32 + vector + value.
        size_t vec_raw_len;
        const char *blob =
            RedisModule_StringPtrLen(argv[start_idx+1],&vec_raw_len);

        // Must be 4 bytes per component.
        if (vec_raw_len % 4 || vec_raw_len < 4) return NULL;
        *dim = vec_raw_len/4;

        vec = RedisModule_Alloc(vec_raw_len);
        if (!vec) return NULL;
        memcpy(vec,blob,vec_raw_len);
        consumed += 2;
    } else if (!strcasecmp(vec_format,"VALUES")) {
        if (argc < start_idx + 2) return NULL;  // Need at least the dimension.
        long long vdim; // Vector dimension passed by the user.
        if (RedisModule_StringToLongLong(argv[start_idx+1],&vdim)
            != REDISMODULE_OK || vdim < 1) return NULL;

        // Check that all the arguments are available.
        if (argc < start_idx + 2 + vdim) return NULL;

        *dim = vdim;
        vec = RedisModule_Alloc(sizeof(float) * vdim);
        if (!vec) return NULL;

        for (int j = 0; j < vdim; j++) {
            double val;
            if (RedisModule_StringToDouble(argv[start_idx+2+j],&val)
                != REDISMODULE_OK)
            {
                RedisModule_Free(vec);
                return NULL;
            }
            vec[j] = val;
        }
        consumed += vdim + 2;
    } else {
        return NULL;  // Unknown format.
    }

    if (consumed_args) *consumed_args = consumed;
    return vec;
}

/* ========================== Commands implementation ======================= */

/* VADD thread handling the "CAS" version of the command, that is
 * performed blocking the client, accumulating here, in the thread, the
 * set of potential candidates, and later inserting the element in the
 * key (if it still exists, and if it is still the *same* vector set)
 * in the Reply callback. */
void *VADD_thread(void *arg) {
    pthread_detach(pthread_self());

    void **targ = (void**)arg;
    RedisModuleBlockedClient *bc = targ[0];
    struct vsetObject *vset = targ[1];
    float *vec = targ[3];
    int ef = (uint64_t)targ[6];

    /* Lock the object and signal that we are no longer pending
     * the lock acquisition. */
    RedisModule_Assert(pthread_rwlock_rdlock(&vset->in_use_lock) == 0);
    vset->thread_creation_pending--;

    /* Look for candidates... */
    InsertContext *ic = hnsw_prepare_insert(vset->hnsw, vec, NULL, 0, 0, ef);
    targ[5] = ic; // Pass the context to the reply callback.

    /* Unblock the client so that our read reply will be invoked. */
    pthread_rwlock_unlock(&vset->in_use_lock);
    RedisModule_BlockedClientMeasureTimeEnd(bc);
    RedisModule_UnblockClient(bc,targ); // Use targ as privdata.
    return NULL;
}

/* Reply callback for CAS variant of VADD.
 * Note: this is called in the main thread, in the background thread
 * we just do the read operation of gathering the neighbors. */
int VADD_CASReply(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    (void)argc;
    RedisModule_AutoMemory(ctx); /* Use automatic memory management. */

    int retval = REDISMODULE_OK;
    void **targ = (void**)RedisModule_GetBlockedClientPrivateData(ctx);
    uint64_t vset_id = (unsigned long) targ[2];
    float *vec = targ[3];
    RedisModuleString *val = targ[4];
    InsertContext *ic = targ[5];
    int ef = (uint64_t)targ[6];
    RedisModuleString *attrib = targ[7];
    RedisModule_Free(targ);

    /* Open the key: there are no guarantees it still exists, or contains
     * a vector set, or even the SAME vector set. */
    RedisModuleKey *key = RedisModule_OpenKey(ctx,argv[1],
        REDISMODULE_READ|REDISMODULE_WRITE);
    int type = RedisModule_KeyType(key);
    struct vsetObject *vset = NULL;

    if (type != REDISMODULE_KEYTYPE_EMPTY &&
        RedisModule_ModuleTypeGetType(key) == VectorSetType)
    {
        vset = RedisModule_ModuleTypeGetValue(key);
        // Same vector set?
        if (vset->id != vset_id) vset = NULL;

        /* Also, if the element was already inserted, we just pretend
         * the other insert won. We don't even start a threaded VADD
         * if this was an update, since the deletion of the element itself
         * in order to perform the update would invalidate the CAS state. */
        if (vset && RedisModule_DictGet(vset->dict,val,NULL) != NULL)
            vset = NULL;
    }

    if (vset == NULL) {
        /* If the object does not match the start of the operation, we
         * just pretend the VADD was performed BEFORE the key was deleted
         * or replaced. We return success but don't do anything. */
        hnsw_free_insert_context(ic);
    } else {
        /* Otherwise try to insert the new element with the neighbors
         * collected in background. If we fail, do it synchronously again
         * from scratch. */

        // First: allocate the dual-ported value for the node.
        struct vsetNodeVal *nv = RedisModule_Alloc(sizeof(*nv));
        nv->item = val;
        nv->attrib = attrib;

        /* Then: insert the node in the HNSW data structure. Note that
         * 'ic' could be NULL in case hnsw_prepare_insert() failed because of
         * locking failure (likely impossible in practical terms). */
        hnswNode *newnode;
        if (ic == NULL ||
            (newnode = hnsw_try_commit_insert(vset->hnsw, ic, nv)) == NULL)
        {
            /* If we are here, the CAS insert failed. We need to insert
             * again with full locking for neighbors selection and
             * actual insertion. This time we can't fail: */
            newnode = hnsw_insert(vset->hnsw, vec, NULL, 0, 0, nv, ef);
            RedisModule_Assert(newnode != NULL);
        }
        RedisModule_DictSet(vset->dict,val,newnode);
        val = NULL; // Don't free it later.
        attrib = NULL; // Don't free it later.

        RedisModule_ReplicateVerbatim(ctx);
    }

    // Whatever happens is a success... :D
    RedisModule_ReplyWithBool(ctx,1);
    if (val) RedisModule_FreeString(ctx,val); // Not added? Free it.
    if (attrib) RedisModule_FreeString(ctx,attrib); // Not added? Free it.
    RedisModule_Free(vec);
    return retval;
}

/* VADD key [REDUCE dim] FP32|VALUES vector value [CAS] [NOQUANT] [BIN] [Q8]
 *      [M count] */
int VADD_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx); /* Use automatic memory management. */

    if (argc < 5) return RedisModule_WrongArity(ctx);

    /* Parse vector with optional REDUCE */
    size_t dim = 0;
    uint32_t reduce_dim = 0;
    int consumed_args;
    int cas = 0; // Threaded check-and-set style insert.
    long long ef = VSET_DEFAULT_C_EF; // HNSW creation time EF for new nodes.
    long long hnsw_create_M = HNSW_DEFAULT_M; // HNSW creation default M value.
    float *vec = parseVector(argv, argc, 2, &dim, &reduce_dim, &consumed_args);
    RedisModuleString *attrib = NULL; // Attributes if passed via ATTRIB.
    if (!vec)
        return RedisModule_ReplyWithError(ctx,"ERR invalid vector specification");

    /* Missing element string at the end? */
    if (argc-2-consumed_args < 1) {
        RedisModule_Free(vec);
        return RedisModule_WrongArity(ctx);
    }

    /* Parse options after the element string. */
    uint32_t quant_type = HNSW_QUANT_Q8; // Default quantization type.

    for (int j = 2 + consumed_args + 1; j < argc; j++) {
        const char *opt = RedisModule_StringPtrLen(argv[j], NULL);
        if (!strcasecmp(opt, "CAS")) {
            cas = 1;
        } else if (!strcasecmp(opt, "EF") && j+1 < argc) {
            if (RedisModule_StringToLongLong(argv[j+1], &ef)
                != REDISMODULE_OK || ef <= 0 || ef > 1000000)
            {
                RedisModule_Free(vec);
                return RedisModule_ReplyWithError(ctx, "ERR invalid EF");
            }
            j++; // skip argument.
        } else if (!strcasecmp(opt, "M") && j+1 < argc) {
            if (RedisModule_StringToLongLong(argv[j+1], &hnsw_create_M)
                != REDISMODULE_OK || hnsw_create_M < HNSW_MIN_M ||
                hnsw_create_M > HNSW_MAX_M)
            {
                RedisModule_Free(vec);
                return RedisModule_ReplyWithError(ctx, "ERR invalid M");
            }
            j++; // skip argument.
        } else if (!strcasecmp(opt, "SETATTR") && j+1 < argc) {
            attrib = argv[j+1];
            j++; // skip argument.
        } else if (!strcasecmp(opt, "NOQUANT")) {
            quant_type = HNSW_QUANT_NONE;
        } else if (!strcasecmp(opt, "BIN")) {
            quant_type = HNSW_QUANT_BIN;
        } else if (!strcasecmp(opt, "Q8")) {
            quant_type = HNSW_QUANT_Q8;
        } else {
            RedisModule_Free(vec);
            return RedisModule_ReplyWithError(ctx,"ERR invalid option after element");
        }
    }

    /* Drop CAS if this is a replica and we are getting the command from the
     * replication link: we want to add/delete items in the same order as
     * the master, while with CAS the timing would be different.
     *
     * Also for Lua scripts and MULTI/EXEC, we want to run the command
     * on the main thread. */
    if (RedisModule_GetContextFlags(ctx) &
            (REDISMODULE_CTX_FLAGS_REPLICATED|
             REDISMODULE_CTX_FLAGS_LUA|
             REDISMODULE_CTX_FLAGS_MULTI))
    {
        cas = 0;
    }

    if (VSGlobalConfig.forceSingleThreadExec) {
        cas = 0;
    }

    /* Open/create key */
    RedisModuleKey *key = RedisModule_OpenKey(ctx,argv[1],
        REDISMODULE_READ|REDISMODULE_WRITE);
    int type = RedisModule_KeyType(key);
    if (type != REDISMODULE_KEYTYPE_EMPTY &&
        RedisModule_ModuleTypeGetType(key) != VectorSetType)
    {
        RedisModule_Free(vec);
        return RedisModule_ReplyWithError(ctx,REDISMODULE_ERRORMSG_WRONGTYPE);
    }

    /* Get the correct value argument based on format and REDUCE */
    RedisModuleString *val = argv[2 + consumed_args];

    /* Create or get existing vector set */
    struct vsetObject *vset;
    if (type == REDISMODULE_KEYTYPE_EMPTY) {
        cas = 0; /* Do synchronous insert at creation, otherwise the
                  * key would be left empty until the threaded part
                  * does not return. It's also pointless to try try
                  * doing threaded first element insertion. */
        vset = createVectorSetObject(reduce_dim ? reduce_dim : dim, quant_type, hnsw_create_M);
        if (vset == NULL) {
            // We can't fail for OOM in Redis, but the mutex initialization
            // at least theoretically COULD fail. Likely this code path
            // is not reachable in practical terms.
            RedisModule_Free(vec);
            return RedisModule_ReplyWithError(ctx,
                "ERR unable to create a Vector Set: system resources issue?");
        }

        /* Initialize projection if requested */
        if (reduce_dim) {
            vset->proj_matrix = createProjectionMatrix(dim, reduce_dim);
            vset->proj_input_size = dim;

            /* Project the vector */
            float *projected = applyProjection(vec, vset->proj_matrix,
                                            dim, reduce_dim);
            RedisModule_Free(vec);
            vec = projected;
        }
        RedisModule_ModuleTypeSetValue(key,VectorSetType,vset);
    } else {
        vset = RedisModule_ModuleTypeGetValue(key);

        if (vset->hnsw->quant_type != quant_type) {
            RedisModule_Free(vec);
            return RedisModule_ReplyWithError(ctx,
                "ERR asked quantization mismatch with existing vector set");
        }

        if (vset->hnsw->M != hnsw_create_M) {
            RedisModule_Free(vec);
            return RedisModule_ReplyWithError(ctx,
                "ERR asked M value mismatch with existing vector set");
        }

        if ((vset->proj_matrix == NULL && vset->hnsw->vector_dim != dim) ||
            (vset->proj_matrix && vset->hnsw->vector_dim != reduce_dim))
        {
            RedisModule_Free(vec);
            return RedisModule_ReplyWithErrorFormat(ctx,
                "ERR Vector dimension mismatch - got %d but set has %d",
                (int)dim, (int)vset->hnsw->vector_dim);
        }

        /* Check REDUCE compatibility */
        if (reduce_dim) {
            if (!vset->proj_matrix) {
                RedisModule_Free(vec);
                return RedisModule_ReplyWithError(ctx,
                    "ERR cannot add projection to existing set without projection");
            }
            if (reduce_dim != vset->hnsw->vector_dim) {
                RedisModule_Free(vec);
                return RedisModule_ReplyWithError(ctx,
                    "ERR projection dimension mismatch with existing set");
            }
        }

        /* Apply projection if needed */
        if (vset->proj_matrix) {
            /* Ensure input dimension matches the projection matrix's expected input dimension */
            if (dim != vset->proj_input_size) {
                RedisModule_Free(vec);
                return RedisModule_ReplyWithErrorFormat(ctx,
                    "ERR Input dimension mismatch for projection - got %d but projection expects %d",
                    (int)dim, (int)vset->proj_input_size);
            }

            float *projected = applyProjection(vec, vset->proj_matrix,
                                             vset->proj_input_size,
                                             vset->hnsw->vector_dim);
            RedisModule_Free(vec);
            vec = projected;
            dim = vset->hnsw->vector_dim;
        }
    }

    /* For existing keys don't do CAS updates. For how things work now, the
     * CAS state would be invalidated by the deletion before adding back. */
    if (cas && RedisModule_DictGet(vset->dict,val,NULL) != NULL)
        cas = 0;

    /* Here depending on the CAS option we directly insert in a blocking
     * way, or use a thread to do candidate neighbors selection and only
     * later, in the reply callback, actually add the element. */
    if (cas) {
        RedisModuleBlockedClient *bc = RedisModule_BlockClient(ctx,VADD_CASReply,NULL,NULL,0);
        pthread_t tid;
        void **targ = RedisModule_Alloc(sizeof(void*)*8);
        targ[0] = bc;
        targ[1] = vset;
        targ[2] = (void*)(unsigned long)vset->id;
        targ[3] = vec;
        targ[4] = val;
        targ[5] = NULL; // Used later for insertion context.
        targ[6] = (void*)(unsigned long)ef;
        targ[7] = attrib;
        RedisModule_RetainString(ctx,val);
        if (attrib) RedisModule_RetainString(ctx,attrib);
        RedisModule_BlockedClientMeasureTimeStart(bc);
        vset->thread_creation_pending++;
        if (pthread_create(&tid,NULL,VADD_thread,targ) != 0) {
            vset->thread_creation_pending--;
            RedisModule_AbortBlock(bc);
            RedisModule_Free(targ);
            RedisModule_FreeString(ctx,val);
            if (attrib) RedisModule_FreeString(ctx,attrib);

            // Fall back to synchronous insert, see later in the code.
        } else {
            return REDISMODULE_OK;
        }
    }

    /* Insert vector synchronously: we reach this place even
     * if cas was true but thread creation failed. */
    int added = vectorSetInsert(vset,vec,NULL,0,val,attrib,1,ef);
    RedisModule_Free(vec);

    RedisModule_ReplyWithBool(ctx,added);
    if (added) RedisModule_ReplicateVerbatim(ctx);
    return REDISMODULE_OK;
}

/* HNSW callback to filter items according to a predicate function
 * (our FILTER expression in this case). */
int vectorSetFilterCallback(void *value, void *privdata) {
    exprstate *expr = privdata;
    struct vsetNodeVal *nv = value;
    if (nv->attrib == NULL) return 0; // No attributes? No match.
    size_t json_len;
    char *json = (char*)RedisModule_StringPtrLen(nv->attrib,&json_len);
    return exprRun(expr,json,json_len);
}

/* Common path for the execution of the VSIM command both threaded and
 * not threaded. Note that 'ctx' may be normal context of a thread safe
 * context obtained from a blocked client. The locking that is specific
 * to the vset object is handled by the caller, however the function
 * handles the HNSW locking explicitly. */
void VSIM_execute(RedisModuleCtx *ctx, struct vsetObject *vset,
    float *vec, unsigned long count, float epsilon, unsigned long withscores,
    unsigned long withattribs, unsigned long ef, exprstate *filter_expr,
    unsigned long filter_ef, int ground_truth)
{
    /* In our scan, we can't just collect 'count' elements as
     * if count is small we would explore the graph in an insufficient
     * way to provide enough recall.
     *
     * If the user didn't asked for a specific exploration, we use
     * VSET_DEFAULT_SEARCH_EF as minimum, or we match count if count
     * is greater than that. Otherwise the minumim will be the specified
     * EF argument. */
    if (ef == 0) ef = VSET_DEFAULT_SEARCH_EF;
    if (count > ef) ef = count;

    /* Perform search */
    hnswNode **neighbors = RedisModule_Alloc(sizeof(hnswNode*)*ef);
    float *distances = RedisModule_Alloc(sizeof(float)*ef);
    int slot = hnsw_acquire_read_slot(vset->hnsw);
    unsigned int found;
    if (ground_truth) {
        found = hnsw_ground_truth_with_filter(vset->hnsw, vec, ef, neighbors,
                    distances, slot, 0,
                    filter_expr ? vectorSetFilterCallback : NULL,
                    filter_expr);
    } else {
        if (filter_expr == NULL) {
            found = hnsw_search(vset->hnsw, vec, ef, neighbors,
                                distances, slot, 0);
        } else {
            found = hnsw_search_with_filter(vset->hnsw, vec, ef, neighbors,
                        distances, slot, 0, vectorSetFilterCallback,
                        filter_expr, filter_ef);
        }
    }

    /* Return results */
    int resp3 = RedisModule_GetContextFlags(ctx) & REDISMODULE_CTX_FLAGS_RESP3;
    int reply_with_map = resp3 && (withscores || withattribs);

    if (reply_with_map)
        RedisModule_ReplyWithMap(ctx, REDISMODULE_POSTPONED_LEN);
    else
        RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_LEN);

    long long arraylen = 0;
    for (unsigned int i = 0; i < found && i < count; i++) {
        if (distances[i]/2 > epsilon) break;
        struct vsetNodeVal *nv = neighbors[i]->value;
        RedisModule_ReplyWithString(ctx, nv->item);
        arraylen++;

        /* If the user asked for multiple properties at the same time using
         * the RESP3 protocol, we wrap the value of the map into an N-items
         * array. Two for now, since we have just two properties that can be
         * requested.
         *
         * So in the case of RESP2 we will just have the flat reply:
         * item, score, attribute. For RESP3 instead item -> [score, attribute]
         */
        if (resp3 && withscores && withattribs)
            RedisModule_ReplyWithArray(ctx,2);

        if (withscores) {
            /* The similarity score is provided in a 0-1 range. */
            RedisModule_ReplyWithDouble(ctx, 1.0 - distances[i]/2.0);
        }
        if (withattribs) {
            /* Return the attributes as well, if any. */
            if (nv->attrib)
                RedisModule_ReplyWithString(ctx, nv->attrib);
            else
                RedisModule_ReplyWithNull(ctx);
        }
    }
    hnsw_release_read_slot(vset->hnsw,slot);

    if (reply_with_map) {
        RedisModule_ReplySetMapLength(ctx, arraylen);
    } else {
        int items_per_ele = 1+withattribs+withscores;
        RedisModule_ReplySetArrayLength(ctx, arraylen * items_per_ele);
    }

    RedisModule_Free(vec);
    RedisModule_Free(neighbors);
    RedisModule_Free(distances);
    if (filter_expr) exprFree(filter_expr);
}

/* VSIM thread handling the blocked client request. */
void *VSIM_thread(void *arg) {
    pthread_detach(pthread_self());

    // Extract arguments.
    void **targ = (void**)arg;
    RedisModuleBlockedClient *bc = targ[0];
    struct vsetObject *vset = targ[1];
    float *vec = targ[2];
    unsigned long count = (unsigned long)targ[3];
    float epsilon = *((float*)targ[4]);
    unsigned long withscores = (unsigned long)targ[5];
    unsigned long withattribs = (unsigned long)targ[6];
    unsigned long ef = (unsigned long)targ[7];
    exprstate *filter_expr = targ[8];
    unsigned long filter_ef = (unsigned long)targ[9];
    unsigned long ground_truth = (unsigned long)targ[10];
    RedisModule_Free(targ[4]);
    RedisModule_Free(targ);

    /* Lock the object and signal that we are no longer pending
     * the lock acquisition. */
    RedisModule_Assert(pthread_rwlock_rdlock(&vset->in_use_lock) == 0);
    vset->thread_creation_pending--;

    // Accumulate reply in a thread safe context: no contention.
    RedisModuleCtx *ctx = RedisModule_GetThreadSafeContext(bc);

    // Run the query.
    VSIM_execute(ctx, vset, vec, count, epsilon, withscores, withattribs, ef, filter_expr, filter_ef, ground_truth);
    pthread_rwlock_unlock(&vset->in_use_lock);

    // Cleanup.
    RedisModule_FreeThreadSafeContext(ctx);
    RedisModule_BlockedClientMeasureTimeEnd(bc);
    RedisModule_UnblockClient(bc,NULL);
    return NULL;
}

/* VSIM key [ELE|FP32|VALUES] <vector or ele> [WITHSCORES] [WITHATTRIBS] [COUNT num] [EPSILON eps] [EF exploration-factor] [FILTER expression] [FILTER-EF exploration-factor] */
int VSIM_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);

    /* Basic argument check: need at least key and vector specification
     * method. */
    if (argc < 4) return RedisModule_WrongArity(ctx);

    /* Defaults */
    int withscores = 0;
    int withattribs = 0;
    long long count = VSET_DEFAULT_COUNT;   /* New default value */
    long long ef = 0;       /* Exploration factor (see HNSW paper) */
    double epsilon = 2.0;   /* Max cosine distance */
    long long ground_truth = 0; /* Linear scan instead of HNSW search? */
    int no_thread = 0;       /* NOTHREAD option: exec on main thread. */

    /* Things computed later. */
    long long filter_ef = 0;
    exprstate *filter_expr = NULL;

    /* Get key and vector type */
    RedisModuleString *key = argv[1];
    const char *vectorType = RedisModule_StringPtrLen(argv[2], NULL);

    /* Get vector set */
    RedisModuleKey *keyptr = RedisModule_OpenKey(ctx, key, REDISMODULE_READ);
    int type = RedisModule_KeyType(keyptr);
    if (type == REDISMODULE_KEYTYPE_EMPTY)
        return RedisModule_ReplyWithEmptyArray(ctx);

    if (RedisModule_ModuleTypeGetType(keyptr) != VectorSetType)
        return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);

    struct vsetObject *vset = RedisModule_ModuleTypeGetValue(keyptr);

    /* Vector parsing stage */
    float *vec = NULL;
    size_t dim = 0;
    int vector_args = 0;  /* Number of args consumed by vector specification */

    if (!strcasecmp(vectorType, "ELE")) {
        /* Get vector from existing element */
        RedisModuleString *ele = argv[3];
        hnswNode *node = RedisModule_DictGet(vset->dict, ele, NULL);
        if (!node) {
            return RedisModule_ReplyWithError(ctx, "ERR element not found in set");
        }
        vec = RedisModule_Alloc(sizeof(float) * vset->hnsw->vector_dim);
        hnsw_get_node_vector(vset->hnsw,node,vec);
        dim = vset->hnsw->vector_dim;
        vector_args = 2;  /* ELE + element name */
    } else {
        /* Parse vector. */
        int consumed_args;

        vec = parseVector(argv, argc, 2, &dim, NULL, &consumed_args);
        if (!vec) {
            return RedisModule_ReplyWithError(ctx,
                "ERR invalid vector specification");
        }
        vector_args = consumed_args;

        /* Apply projection if the set uses it, with the exception
         * of ELE type, that will already have the right dimension. */
        if (vset->proj_matrix && dim != vset->hnsw->vector_dim) {
            /* Ensure input dimension matches the projection matrix's expected input dimension */
            if (dim != vset->proj_input_size) {
                RedisModule_Free(vec);
                return RedisModule_ReplyWithErrorFormat(ctx,
                    "ERR Input dimension mismatch for projection - got %d but projection expects %d",
                    (int)dim, (int)vset->proj_input_size);
            }

            float *projected = applyProjection(vec, vset->proj_matrix,
                                             vset->proj_input_size,
                                             vset->hnsw->vector_dim);
            RedisModule_Free(vec);
            vec = projected;
            dim = vset->hnsw->vector_dim;
        }

        /* Count consumed arguments */
        if (!strcasecmp(vectorType, "FP32")) {
            vector_args = 2;  /* FP32 + vector blob */
        } else if (!strcasecmp(vectorType, "VALUES")) {
            long long vdim;
            if (RedisModule_StringToLongLong(argv[3], &vdim) != REDISMODULE_OK) {
                RedisModule_Free(vec);
                return RedisModule_ReplyWithError(ctx, "ERR invalid vector dimension");
            }
            vector_args = 2 + vdim;  /* VALUES + dim + values */
        } else {
            RedisModule_Free(vec);
            return RedisModule_ReplyWithError(ctx,
                "ERR vector type must be ELE, FP32 or VALUES");
        }
    }

    /* Check vector dimension matches set */
    if (dim != vset->hnsw->vector_dim) {
        RedisModule_Free(vec);
        return RedisModule_ReplyWithErrorFormat(ctx,
            "ERR Vector dimension mismatch - got %d but set has %d",
            (int)dim, (int)vset->hnsw->vector_dim);
    }

    /* Parse optional arguments - start after vector specification */
    int j = 2 + vector_args;
    while (j < argc) {
        const char *opt = RedisModule_StringPtrLen(argv[j], NULL);
        if (!strcasecmp(opt, "WITHSCORES")) {
            withscores = 1;
            j++;
        } else if (!strcasecmp(opt, "WITHATTRIBS")) {
            withattribs = 1;
            j++;
        } else if (!strcasecmp(opt, "TRUTH")) {
            ground_truth = 1;
            j++;
        } else if (!strcasecmp(opt, "NOTHREAD")) {
            no_thread = 1;
            j++;
        } else if (!strcasecmp(opt, "COUNT") && j+1 < argc) {
            if (RedisModule_StringToLongLong(argv[j+1], &count)
                != REDISMODULE_OK || count <= 0)
            {
                RedisModule_Free(vec);
                return RedisModule_ReplyWithError(ctx, "ERR invalid COUNT");
            }
            j += 2;
        } else if (!strcasecmp(opt, "EPSILON") && j+1 < argc) {
            if (RedisModule_StringToDouble(argv[j+1], &epsilon) !=
                REDISMODULE_OK || epsilon <= 0)
            {
                RedisModule_Free(vec);
                return RedisModule_ReplyWithError(ctx, "ERR invalid EPSILON");
            }
            j += 2;
        } else if (!strcasecmp(opt, "EF") && j+1 < argc) {
            if (RedisModule_StringToLongLong(argv[j+1], &ef) !=
                REDISMODULE_OK || ef <= 0)
            {
                RedisModule_Free(vec);
                return RedisModule_ReplyWithError(ctx, "ERR invalid EF");
            }
            j += 2;
        } else if (!strcasecmp(opt, "FILTER-EF") && j+1 < argc) {
            if (RedisModule_StringToLongLong(argv[j+1], &filter_ef) !=
                REDISMODULE_OK || filter_ef <= 0)
            {
                RedisModule_Free(vec);
                return RedisModule_ReplyWithError(ctx, "ERR invalid FILTER-EF");
            }
            j += 2;
        } else if (!strcasecmp(opt, "FILTER") && j+1 < argc) {
            RedisModuleString *exprarg = argv[j+1];
            size_t exprlen;
            char *exprstr = (char*)RedisModule_StringPtrLen(exprarg,&exprlen);
            int errpos;
            filter_expr = exprCompile(exprstr,&errpos);
            if (filter_expr == NULL) {
                if ((size_t)errpos >= exprlen) errpos = 0;
                RedisModule_Free(vec);
                return RedisModule_ReplyWithErrorFormat(ctx,
                    "ERR syntax error in FILTER expression near: %s",
                        exprstr+errpos);
            }
            j += 2;
        } else {
            RedisModule_Free(vec);
            return RedisModule_ReplyWithError(ctx,
                "ERR syntax error in VSIM command");
        }
    }

    int threaded_request = 1; // Run on a thread, by default.
    if (filter_ef == 0) filter_ef = count * 100; // Max filter visited nodes.

    /* Disable threaded for MULTI/EXEC and Lua, or if explicitly
     * requested by the user via the NOTHREAD option. */
    if (no_thread || VSGlobalConfig.forceSingleThreadExec ||
        (RedisModule_GetContextFlags(ctx) &
        (REDISMODULE_CTX_FLAGS_LUA | REDISMODULE_CTX_FLAGS_MULTI)))
    {
        threaded_request = 0;
    }

    if (threaded_request) {
        /* Note: even if we create one thread per request, the underlying
         * HNSW library has a fixed number of slots for the threads, as it's
         * defined in HNSW_MAX_THREADS (beware that if you increase it,
         * every node will use more memory). This means that while this request
         * is threaded, and will NOT block Redis, it may end waiting for a
         * free slot if all the HNSW_MAX_THREADS slots are used. */
        RedisModuleBlockedClient *bc = RedisModule_BlockClient(ctx,NULL,NULL,NULL,0);
        pthread_t tid;
        void **targ = RedisModule_Alloc(sizeof(void*)*11);
        targ[0] = bc;
        targ[1] = vset;
        targ[2] = vec;
        targ[3] = (void*)count;
        targ[4] = RedisModule_Alloc(sizeof(float));
        *((float*)targ[4]) = epsilon;
        targ[5] = (void*)(unsigned long)withscores;
        targ[6] = (void*)(unsigned long)withattribs;
        targ[7] = (void*)(unsigned long)ef;
        targ[8] = (void*)filter_expr;
        targ[9] = (void*)(unsigned long)filter_ef;
        targ[10] = (void*)(unsigned long)ground_truth;
        RedisModule_BlockedClientMeasureTimeStart(bc);
        vset->thread_creation_pending++;
        if (pthread_create(&tid,NULL,VSIM_thread,targ) != 0) {
            vset->thread_creation_pending--;
            RedisModule_AbortBlock(bc);
            RedisModule_Free(targ[4]);
            RedisModule_Free(targ);
            VSIM_execute(ctx, vset, vec, count, epsilon, withscores, withattribs, ef, filter_expr, filter_ef, ground_truth);
        }
    } else {
        VSIM_execute(ctx, vset, vec, count, epsilon, withscores, withattribs, ef, filter_expr, filter_ef, ground_truth);
    }

    return REDISMODULE_OK;
}

/* VDIM <key>: return the dimension of vectors in the vector set. */
int VDIM_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);

    if (argc != 2) return RedisModule_WrongArity(ctx);

    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ);
    int type = RedisModule_KeyType(key);

    if (type == REDISMODULE_KEYTYPE_EMPTY)
        return RedisModule_ReplyWithError(ctx, "ERR key does not exist");

    if (RedisModule_ModuleTypeGetType(key) != VectorSetType)
        return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);

    struct vsetObject *vset = RedisModule_ModuleTypeGetValue(key);
    return RedisModule_ReplyWithLongLong(ctx, vset->hnsw->vector_dim);
}

/* VCARD <key>: return cardinality (num of elements) of the vector set. */
int VCARD_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);

    if (argc != 2) return RedisModule_WrongArity(ctx);

    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ);
    int type = RedisModule_KeyType(key);

    if (type == REDISMODULE_KEYTYPE_EMPTY)
        return RedisModule_ReplyWithLongLong(ctx, 0);

    if (RedisModule_ModuleTypeGetType(key) != VectorSetType)
        return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);

    struct vsetObject *vset = RedisModule_ModuleTypeGetValue(key);
    return RedisModule_ReplyWithLongLong(ctx, vset->hnsw->node_count);
}

/* VREM key element
 * Remove an element from a vector set.
 * Returns 1 if the element was found and removed, 0 if not found. */
int VREM_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx); /* Use automatic memory management. */

    if (argc != 3) return RedisModule_WrongArity(ctx);

    /* Get key and value */
    RedisModuleString *key = argv[1];
    RedisModuleString *element = argv[2];

    /* Open key */
    RedisModuleKey *keyptr = RedisModule_OpenKey(ctx, key,
        REDISMODULE_READ|REDISMODULE_WRITE);
    int type = RedisModule_KeyType(keyptr);

    /* Handle non-existing key or wrong type */
    if (type == REDISMODULE_KEYTYPE_EMPTY) {
        return RedisModule_ReplyWithBool(ctx, 0);
    }
    if (RedisModule_ModuleTypeGetType(keyptr) != VectorSetType) {
        return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
    }

    /* Get vector set from key */
    struct vsetObject *vset = RedisModule_ModuleTypeGetValue(keyptr);

    /* Find the node for this element */
    hnswNode *node = RedisModule_DictGet(vset->dict, element, NULL);
    if (!node) {
        return RedisModule_ReplyWithBool(ctx, 0);
    }

    /* Remove from dictionary */
    RedisModule_DictDel(vset->dict, element, NULL);

    /* Remove from HNSW graph using the high-level API that handles
     * locking and cleanup. We pass RedisModule_FreeString as the value
     * free function since the strings were retained at insertion time. */
    struct vsetNodeVal *nv = node->value;
    if (nv->attrib != NULL) vset->numattribs--;
    RedisModule_Assert(hnsw_delete_node(vset->hnsw, node, vectorSetReleaseNodeValue) == 1);

    /* Destroy empty vector set. */
    if (RedisModule_DictSize(vset->dict) == 0) {
        RedisModule_DeleteKey(keyptr);
    }

    /* Reply and propagate the command */
    RedisModule_ReplyWithBool(ctx, 1);
    RedisModule_ReplicateVerbatim(ctx);
    return REDISMODULE_OK;
}

/* VEMB key element
 * Returns the embedding vector associated with an element, or NIL if not
 * found. The vector is returned in the same format it was added, but the
 * return value will have some lack of precision due to quantization and
 * normalization of vectors. Also, if items were added using REDUCE, the
 * reduced vector is returned instead. */
int VEMB_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);
    int raw_output = 0; // RAW option.

    if (argc < 3) return RedisModule_WrongArity(ctx);

    /* Parse arguments. */
    for (int j = 3; j < argc; j++) {
        const char *opt = RedisModule_StringPtrLen(argv[j], NULL);
        if (!strcasecmp(opt,"raw")) {
            raw_output = 1;
        } else {
            return RedisModule_ReplyWithError(ctx,"ERR invalid option");
        }
    }

    /* Get key and element. */
    RedisModuleString *key = argv[1];
    RedisModuleString *element = argv[2];

    /* Open key. */
    RedisModuleKey *keyptr = RedisModule_OpenKey(ctx, key, REDISMODULE_READ);
    int type = RedisModule_KeyType(keyptr);

    /* Handle non-existing key and key of wrong type. */
    if (type == REDISMODULE_KEYTYPE_EMPTY) {
        return RedisModule_ReplyWithNull(ctx);
    } else if (RedisModule_ModuleTypeGetType(keyptr) != VectorSetType) {
        return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
    }

    /* Lookup the node about the specified element. */
    struct vsetObject *vset = RedisModule_ModuleTypeGetValue(keyptr);
    hnswNode *node = RedisModule_DictGet(vset->dict, element, NULL);
    if (!node) {
        return RedisModule_ReplyWithNull(ctx);
    }

    if (raw_output) {
        int output_qrange = vset->hnsw->quant_type == HNSW_QUANT_Q8;
        RedisModule_ReplyWithArray(ctx, 3+output_qrange);
        RedisModule_ReplyWithSimpleString(ctx, vectorSetGetQuantName(vset));
        RedisModule_ReplyWithStringBuffer(ctx, node->vector, hnsw_quants_bytes(vset->hnsw));
        RedisModule_ReplyWithDouble(ctx, node->l2);
        if (output_qrange) RedisModule_ReplyWithDouble(ctx, node->quants_range);
    } else {
        /* Get the vector associated with the node. */
        float *vec = RedisModule_Alloc(sizeof(float) * vset->hnsw->vector_dim);
        hnsw_get_node_vector(vset->hnsw, node, vec); // May dequantize/denorm.

        /* Return as array of doubles. */
        RedisModule_ReplyWithArray(ctx, vset->hnsw->vector_dim);
        for (uint32_t i = 0; i < vset->hnsw->vector_dim; i++)
            RedisModule_ReplyWithDouble(ctx, vec[i]);
        RedisModule_Free(vec);
    }
    return REDISMODULE_OK;
}

/* VSETATTR key element json
 * Set or remove the JSON attribute associated with an element.
 * Setting an empty string removes the attribute.
 * The command returns one if the attribute was actually updated or
 * zero if there is no key or element. */
int VSETATTR_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);

    if (argc != 4) return RedisModule_WrongArity(ctx);

    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1],
        REDISMODULE_READ|REDISMODULE_WRITE);
    int type = RedisModule_KeyType(key);

    if (type == REDISMODULE_KEYTYPE_EMPTY)
        return RedisModule_ReplyWithBool(ctx, 0);

    if (RedisModule_ModuleTypeGetType(key) != VectorSetType)
        return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);

    struct vsetObject *vset = RedisModule_ModuleTypeGetValue(key);
    hnswNode *node = RedisModule_DictGet(vset->dict, argv[2], NULL);
    if (!node)
        return RedisModule_ReplyWithBool(ctx, 0);

    struct vsetNodeVal *nv = node->value;
    RedisModuleString *new_attr = argv[3];

    /* Background VSIM operations use the node attributes, so
     * wait for background operations before messing with them. */
    vectorSetWaitAllBackgroundClients(vset,0);

    /* Set or delete the attribute based on the fact it's an empty
     * string or not. */
    size_t attrlen;
    RedisModule_StringPtrLen(new_attr, &attrlen);
    if (attrlen == 0) {
        // If we had an attribute before, decrease the count and free it.
        if (nv->attrib) {
            vset->numattribs--;
            RedisModule_FreeString(NULL, nv->attrib);
            nv->attrib = NULL;
        }
    } else {
        // If we didn't have an attribute before, increase the count.
        // Otherwise free the old one.
        if (nv->attrib) {
            RedisModule_FreeString(NULL, nv->attrib);
        } else {
            vset->numattribs++;
        }
        // Set new attribute.
        RedisModule_RetainString(NULL, new_attr);
        nv->attrib = new_attr;
    }

    RedisModule_ReplyWithBool(ctx, 1);
    RedisModule_ReplicateVerbatim(ctx);
    return REDISMODULE_OK;
}

/* VGETATTR key element
 * Get the JSON attribute associated with an element.
 * Returns NIL if the element has no attribute or doesn't exist. */
int VGETATTR_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);

    if (argc != 3) return RedisModule_WrongArity(ctx);

    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ);
    int type = RedisModule_KeyType(key);

    if (type == REDISMODULE_KEYTYPE_EMPTY)
        return RedisModule_ReplyWithNull(ctx);

    if (RedisModule_ModuleTypeGetType(key) != VectorSetType)
        return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);

    struct vsetObject *vset = RedisModule_ModuleTypeGetValue(key);
    hnswNode *node = RedisModule_DictGet(vset->dict, argv[2], NULL);
    if (!node)
        return RedisModule_ReplyWithNull(ctx);

    struct vsetNodeVal *nv = node->value;
    if (!nv->attrib)
        return RedisModule_ReplyWithNull(ctx);

    return RedisModule_ReplyWithString(ctx, nv->attrib);
}

/* ============================== Reflection ================================ */

/* VLINKS key element [WITHSCORES]
 * Returns the neighbors of an element at each layer in the HNSW graph.
 * Reply is an array of arrays, where each nested array represents one level
 * of neighbors, from highest level to level 0. If WITHSCORES is specified,
 * each neighbor is followed by its distance from the element. */
int VLINKS_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);

    if (argc < 3 || argc > 4) return RedisModule_WrongArity(ctx);

    RedisModuleString *key = argv[1];
    RedisModuleString *element = argv[2];

    /* Parse WITHSCORES option. */
    int withscores = 0;
    if (argc == 4) {
        const char *opt = RedisModule_StringPtrLen(argv[3], NULL);
        if (strcasecmp(opt, "WITHSCORES") != 0) {
            return RedisModule_WrongArity(ctx);
        }
        withscores = 1;
    }

    RedisModuleKey *keyptr = RedisModule_OpenKey(ctx, key, REDISMODULE_READ);
    int type = RedisModule_KeyType(keyptr);

    /* Handle non-existing key or wrong type. */
    if (type == REDISMODULE_KEYTYPE_EMPTY)
        return RedisModule_ReplyWithNull(ctx);

    if (RedisModule_ModuleTypeGetType(keyptr) != VectorSetType)
        return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);

    /* Find the node for this element. */
    struct vsetObject *vset = RedisModule_ModuleTypeGetValue(keyptr);
    hnswNode *node = RedisModule_DictGet(vset->dict, element, NULL);
    if (!node)
        return RedisModule_ReplyWithNull(ctx);

    /* Reply with array of arrays, one per level. */
    RedisModule_ReplyWithArray(ctx, node->level + 1);

    /* For each level, from highest to lowest: */
    for (int i = node->level; i >= 0; i--) {
        /* Reply with array of neighbors at this level. */
        if (withscores)
            RedisModule_ReplyWithMap(ctx,node->layers[i].num_links);
        else
            RedisModule_ReplyWithArray(ctx,node->layers[i].num_links);

        /* Add each neighbor's element value to the array. */
        for (uint32_t j = 0; j < node->layers[i].num_links; j++) {
            struct vsetNodeVal *nv = node->layers[i].links[j]->value;
            RedisModule_ReplyWithString(ctx, nv->item);
            if (withscores) {
                float distance = hnsw_distance(vset->hnsw, node, node->layers[i].links[j]);
                /* Convert distance to similarity score to match
                 * VSIM behavior.*/
                float similarity = 1.0 - distance/2.0;
                RedisModule_ReplyWithDouble(ctx, similarity);
            }
        }
    }
    return REDISMODULE_OK;
}

/* VINFO key
 * Returns information about a vector set, both visible and hidden
 * features of the HNSW data structure. */
int VINFO_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);

    if (argc != 2) return RedisModule_WrongArity(ctx);

    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ);
    int type = RedisModule_KeyType(key);

    if (type == REDISMODULE_KEYTYPE_EMPTY)
        return RedisModule_ReplyWithNullArray(ctx);

    if (RedisModule_ModuleTypeGetType(key) != VectorSetType)
        return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);

    struct vsetObject *vset = RedisModule_ModuleTypeGetValue(key);

    /* Reply with hash */
    RedisModule_ReplyWithMap(ctx, 9);

    /* Quantization type */
    RedisModule_ReplyWithSimpleString(ctx, "quant-type");
    RedisModule_ReplyWithSimpleString(ctx, vectorSetGetQuantName(vset));

    /* HNSW M value */
    RedisModule_ReplyWithSimpleString(ctx, "hnsw-m");
    RedisModule_ReplyWithLongLong(ctx, vset->hnsw->M);

    /* Vector dimensionality. */
    RedisModule_ReplyWithSimpleString(ctx, "vector-dim");
    RedisModule_ReplyWithLongLong(ctx, vset->hnsw->vector_dim);

    /* Original input dimension before projection.
     * This is zero for vector sets without a random projection matrix. */
    RedisModule_ReplyWithSimpleString(ctx, "projection-input-dim");
    RedisModule_ReplyWithLongLong(ctx, vset->proj_input_size);

    /* Number of elements. */
    RedisModule_ReplyWithSimpleString(ctx, "size");
    RedisModule_ReplyWithLongLong(ctx, vset->hnsw->node_count);

    /* Max level of HNSW. */
    RedisModule_ReplyWithSimpleString(ctx, "max-level");
    RedisModule_ReplyWithLongLong(ctx, vset->hnsw->max_level);

    /* Number of nodes with attributes. */
    RedisModule_ReplyWithSimpleString(ctx, "attributes-count");
    RedisModule_ReplyWithLongLong(ctx, vset->numattribs);

    /* Vector set ID. */
    RedisModule_ReplyWithSimpleString(ctx, "vset-uid");
    RedisModule_ReplyWithLongLong(ctx, vset->id);

    /* HNSW max node ID. */
    RedisModule_ReplyWithSimpleString(ctx, "hnsw-max-node-uid");
    RedisModule_ReplyWithLongLong(ctx, vset->hnsw->last_id);

    return REDISMODULE_OK;
}

/* VRANDMEMBER key [count]
 * Return random members from a vector set.
 *
 * Without count: returns a single random member.
 * With positive count: N unique random members (no duplicates).
 * With negative count: N random members (with possible duplicates).
 *
 * If the key doesn't exist, returns NULL if count is not given, or
 * an empty array if a count was given. */
int VRANDMEMBER_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx); /* Use automatic memory management. */

    /* Check arguments. */
    if (argc != 2 && argc != 3) return RedisModule_WrongArity(ctx);

    /* Parse optional count argument. */
    long long count = 1;  /* Default is to return a single element. */
    int with_count = (argc == 3);

    if (with_count) {
        if (RedisModule_StringToLongLong(argv[2], &count) != REDISMODULE_OK) {
            return RedisModule_ReplyWithError(ctx,
                "ERR COUNT value is not an integer");
        }
        /* Count = 0 is a special case, return empty array */
        if (count == 0) {
            return RedisModule_ReplyWithEmptyArray(ctx);
        }
    }

    /* Open key. */
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ);
    int type = RedisModule_KeyType(key);

    /* Handle non-existing key. */
    if (type == REDISMODULE_KEYTYPE_EMPTY) {
        if (!with_count) {
            return RedisModule_ReplyWithNull(ctx);
        } else {
            return RedisModule_ReplyWithEmptyArray(ctx);
        }
    }

    /* Check key type. */
    if (RedisModule_ModuleTypeGetType(key) != VectorSetType) {
        return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
    }

    /* Get vector set from key. */
    struct vsetObject *vset = RedisModule_ModuleTypeGetValue(key);
    uint64_t set_size = vset->hnsw->node_count;

    /* No elements in the set? */
    if (set_size == 0) {
        if (!with_count) {
            return RedisModule_ReplyWithNull(ctx);
        } else {
            return RedisModule_ReplyWithEmptyArray(ctx);
        }
    }

    /* Case 1: No count specified: return a single element. */
    if (!with_count) {
        hnswNode *random_node = hnsw_random_node(vset->hnsw, 0);
        if (random_node) {
            struct vsetNodeVal *nv = random_node->value;
            return RedisModule_ReplyWithString(ctx, nv->item);
        } else {
            return RedisModule_ReplyWithNull(ctx);
        }
    }

    /* Case 2: COUNT option given, return an array of elements. */
    int allow_duplicates = (count < 0);
    long long abs_count = (count < 0) ? -count : count;

    /* Cap the count to the set size if we are not allowing duplicates. */
    if (!allow_duplicates && abs_count > (long long)set_size)
        abs_count = set_size;

    /* Prepare reply. */
    RedisModule_ReplyWithArray(ctx, abs_count);

    if (allow_duplicates) {
        /* Simple case: With duplicates, just pick random nodes
         * abs_count times. */
        for (long long i = 0; i < abs_count; i++) {
            hnswNode *random_node = hnsw_random_node(vset->hnsw,0);
            struct vsetNodeVal *nv = random_node->value;
            RedisModule_ReplyWithString(ctx, nv->item);
        }
    } else {
        /* Case where count is positive: we need unique elements.
         * But, if the user asked for many elements, selecting so
         * many (> 20%) random nodes may be too expansive: we just start
         * from a random element and follow the next link.
         *
         * Otherwisem for the <= 20% case, a dictionary is used to
         * reject duplicates. */
        int use_dict = (abs_count <= set_size * 0.2);

        if (use_dict) {
            RedisModuleDict *returned = RedisModule_CreateDict(ctx);

            long long returned_count = 0;
            while (returned_count < abs_count) {
                hnswNode *random_node = hnsw_random_node(vset->hnsw, 0);
                struct vsetNodeVal *nv = random_node->value;

                /* Check if we've already returned this element. */
                if (RedisModule_DictGet(returned, nv->item, NULL) == NULL) {
                    /* Mark as returned and add to results. */
                    RedisModule_DictSet(returned, nv->item, (void*)1);
                    RedisModule_ReplyWithString(ctx, nv->item);
                    returned_count++;
                }
            }
            RedisModule_FreeDict(ctx, returned);
        } else {
            /* For large samples, get a random starting node and walk
             * the list.
             *
             * IMPORTANT: doing so does not really generate random
             * elements: it's just a linear scan, but we have no choices.
             * If we generate too many random elements, more and more would
             * fail the check of being novel (not yet collected in the set
             * to return) if the % of elements to emit is too large, we would
             * spend too much CPU. */
            hnswNode *start_node = hnsw_random_node(vset->hnsw, 0);
            hnswNode *current = start_node;

            long long returned_count = 0;
            while (returned_count < abs_count) {
                if (current == NULL) {
                    /* Restart from head if we hit the end. */
                    current = vset->hnsw->head;
                }
                struct vsetNodeVal *nv = current->value;
                RedisModule_ReplyWithString(ctx, nv->item);
                returned_count++;
                current = current->next;
            }
        }
    }
    return REDISMODULE_OK;
}

/* VISMEMBER key element
 * Check if an element exists in a vector set.
 * Returns 1 if the element exists, 0 if not. */
int VISMEMBER_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);
    if (argc != 3) return RedisModule_WrongArity(ctx);

    RedisModuleString *key = argv[1];
    RedisModuleString *element = argv[2];

    /* Open key. */
    RedisModuleKey *keyptr = RedisModule_OpenKey(ctx, key, REDISMODULE_READ);
    int type = RedisModule_KeyType(keyptr);

    /* Handle non-existing key or wrong type. */
    if (type == REDISMODULE_KEYTYPE_EMPTY) {
        /* An element of a non existing key does not exist, like
         * SISMEMBER & similar. */
        return RedisModule_ReplyWithBool(ctx, 0);
    }
    if (RedisModule_ModuleTypeGetType(keyptr) != VectorSetType) {
        return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
    }

    /* Get the object and test membership via the dictionary in constant
     * time (assuming a member of average size). */
    struct vsetObject *vset = RedisModule_ModuleTypeGetValue(keyptr);
    hnswNode *node = RedisModule_DictGet(vset->dict, element, NULL);
    return RedisModule_ReplyWithBool(ctx, node != NULL);
}

/* ============================== vset type methods ========================= */

#define SAVE_FLAG_HAS_PROJMATRIX    (1<<0)
#define SAVE_FLAG_HAS_ATTRIBS       (1<<1)

/* Save object to RDB */
void VectorSetRdbSave(RedisModuleIO *rdb, void *value) {
    struct vsetObject *vset = value;
    RedisModule_SaveUnsigned(rdb, vset->hnsw->vector_dim);
    RedisModule_SaveUnsigned(rdb, vset->hnsw->node_count);

    uint32_t hnsw_config = (vset->hnsw->quant_type & 0xff) |
                           ((vset->hnsw->M & 0xffff) << 8);
    RedisModule_SaveUnsigned(rdb, hnsw_config);

    uint32_t save_flags = 0;
    if (vset->proj_matrix) save_flags |= SAVE_FLAG_HAS_PROJMATRIX;
    if (vset->numattribs != 0) save_flags |= SAVE_FLAG_HAS_ATTRIBS;
    RedisModule_SaveUnsigned(rdb, save_flags);

    /* Save projection matrix if present */
    if (vset->proj_matrix) {
        uint32_t input_dim = vset->proj_input_size;
        uint32_t output_dim = vset->hnsw->vector_dim;
        RedisModule_SaveUnsigned(rdb, input_dim);
        // Output dim is the same as the first value saved
        // above, so we don't save it.

        // Save projection matrix as binary blob
        size_t matrix_size = sizeof(float) * input_dim * output_dim;
        RedisModule_SaveStringBuffer(rdb, (const char *)vset->proj_matrix, matrix_size);
    }

    hnswNode *node = vset->hnsw->head;
    while(node) {
        struct vsetNodeVal *nv = node->value;
        RedisModule_SaveString(rdb, nv->item);
        if (vset->numattribs) {
            if (nv->attrib)
                RedisModule_SaveString(rdb, nv->attrib);
            else
                RedisModule_SaveStringBuffer(rdb, "", 0);
        }
        hnswSerNode *sn = hnsw_serialize_node(vset->hnsw,node);
        RedisModule_SaveStringBuffer(rdb, (const char *)sn->vector, sn->vector_size);
        RedisModule_SaveUnsigned(rdb, sn->params_count);
        for (uint32_t j = 0; j < sn->params_count; j++)
            RedisModule_SaveUnsigned(rdb, sn->params[j]);
        hnsw_free_serialized_node(sn);
        node = node->next;
    }
}

/* Load object from RDB. Recover from recoverable errors (read errors)
 * by performing cleanup. */
void *VectorSetRdbLoad(RedisModuleIO *rdb, int encver) {
    if (encver != 0) return NULL;  // Invalid version

    uint32_t dim = RedisModule_LoadUnsigned(rdb);
    uint64_t elements = RedisModule_LoadUnsigned(rdb);
    uint32_t hnsw_config = RedisModule_LoadUnsigned(rdb);
    if (RedisModule_IsIOError(rdb)) return NULL;
    uint32_t quant_type = hnsw_config & 0xff;
    uint32_t hnsw_m = (hnsw_config >> 8) & 0xffff;

    /* Check that the quantization type is correct. Otherwise
     * return ASAP signaling the error. */
    if (quant_type != HNSW_QUANT_NONE &&
        quant_type != HNSW_QUANT_Q8 &&
        quant_type != HNSW_QUANT_BIN) return NULL;

    if (hnsw_m == 0) hnsw_m = 16; // Default, useful for RDB files predating
                                  // this configuration parameter: it was fixed
                                  // to 16.
    struct vsetObject *vset = createVectorSetObject(dim,quant_type,hnsw_m);
    RedisModule_Assert(vset != NULL);

    /* Load projection matrix if present */
    uint32_t save_flags = RedisModule_LoadUnsigned(rdb);
    if (RedisModule_IsIOError(rdb)) goto ioerr;
    int has_projection = save_flags & SAVE_FLAG_HAS_PROJMATRIX;
    int has_attribs = save_flags & SAVE_FLAG_HAS_ATTRIBS;
    if (has_projection) {
        uint32_t input_dim = RedisModule_LoadUnsigned(rdb);
        if (RedisModule_IsIOError(rdb)) goto ioerr;
        uint32_t output_dim = dim;
        size_t matrix_size = sizeof(float) * input_dim * output_dim;

        vset->proj_matrix = RedisModule_Alloc(matrix_size);
        vset->proj_input_size = input_dim;

        // Load projection matrix as a binary blob
        char *matrix_blob = RedisModule_LoadStringBuffer(rdb, NULL);
        if (matrix_blob == NULL) goto ioerr;
        memcpy(vset->proj_matrix, matrix_blob, matrix_size);
        RedisModule_Free(matrix_blob);
    }

    while(elements--) {
        // Load associated string element.
        RedisModuleString *ele = RedisModule_LoadString(rdb);
        if (RedisModule_IsIOError(rdb)) goto ioerr;
        RedisModuleString *attrib = NULL;
        if (has_attribs) {
            attrib = RedisModule_LoadString(rdb);
            if (RedisModule_IsIOError(rdb)) {
                RedisModule_FreeString(NULL,ele);
                goto ioerr;
            }
            size_t attrlen;
            RedisModule_StringPtrLen(attrib,&attrlen);
            if (attrlen == 0) {
                RedisModule_FreeString(NULL,attrib);
                attrib = NULL;
            }
        }
        size_t vector_len;
        void *vector = RedisModule_LoadStringBuffer(rdb, &vector_len);
        if (RedisModule_IsIOError(rdb)) {
            RedisModule_FreeString(NULL,ele);
            if (attrib) RedisModule_FreeString(NULL,attrib);
            goto ioerr;
        }
        uint32_t vector_bytes = hnsw_quants_bytes(vset->hnsw);
        if (vector_len != vector_bytes) {
            RedisModule_LogIOError(rdb,"warning",
                                       "Mismatching vector dimension");
            RedisModule_FreeString(NULL,ele);
            if (attrib) RedisModule_FreeString(NULL,attrib);
            RedisModule_Free(vector);
            goto ioerr;
        }

        // Load node parameters back.
        uint32_t params_count = RedisModule_LoadUnsigned(rdb);
        if (RedisModule_IsIOError(rdb)) {
            RedisModule_FreeString(NULL,ele);
            if (attrib) RedisModule_FreeString(NULL,attrib);
            RedisModule_Free(vector);
            goto ioerr;
        }

        uint64_t *params = RedisModule_Alloc(params_count*sizeof(uint64_t));
        for (uint32_t j = 0; j < params_count; j++) {
            // Ignore loading errors here: handled at the end of the loop.
            params[j] = RedisModule_LoadUnsigned(rdb);
        }
        if (RedisModule_IsIOError(rdb)) {
            RedisModule_FreeString(NULL,ele);
            if (attrib) RedisModule_FreeString(NULL,attrib);
            RedisModule_Free(vector);
            RedisModule_Free(params);
            goto ioerr;
        }

        struct vsetNodeVal *nv = RedisModule_Alloc(sizeof(*nv));
        nv->item = ele;
        nv->attrib = attrib;
        hnswNode *node = hnsw_insert_serialized(vset->hnsw, vector, params, params_count, nv);
        if (node == NULL) {
            RedisModule_LogIOError(rdb,"warning",
                                       "Vector set node index loading error");
            vectorSetReleaseNodeValue(nv);
            RedisModule_Free(vector);
            RedisModule_Free(params);
            goto ioerr;
        }
        if (nv->attrib) vset->numattribs++;
        RedisModule_DictSet(vset->dict,ele,node);
        RedisModule_Free(vector);
        RedisModule_Free(params);
    }

    uint64_t salt[2];
    RedisModule_GetRandomBytes((unsigned char*)salt,sizeof(salt));
    if (!hnsw_deserialize_index(vset->hnsw, salt[0], salt[1])) goto ioerr;

    return vset;

ioerr:
    /* We want to recover from I/O errors and free the partially allocated
     * data structure to support diskless replication. */
    vectorSetReleaseObject(vset);
    return NULL;
}

/* Calculate memory usage */
size_t VectorSetMemUsage(const void *value) {
    const struct vsetObject *vset = value;
    size_t size = sizeof(*vset);

    /* Account for HNSW index base structure */
    size += sizeof(HNSW);

    /* Account for projection matrix if present */
    if (vset->proj_matrix) {
        /* For the matrix size, we need the input dimension. We can get it
         * from the first node if the set is not empty. */
        uint32_t input_dim = vset->proj_input_size;
        uint32_t output_dim = vset->hnsw->vector_dim;
        size += sizeof(float) * input_dim * output_dim;
    }

    /* Account for each node's memory usage. */
    hnswNode *node = vset->hnsw->head;
    if (node == NULL) return size;

    /* Base node structure. */
    size += sizeof(*node) * vset->hnsw->node_count;

    /* Vector storage. */
    uint64_t vec_storage = hnsw_quants_bytes(vset->hnsw);
    size += vec_storage * vset->hnsw->node_count;

    /* Layers array. We use 1.33 as average nodes layers count. */
    uint64_t layers_storage = sizeof(hnswNodeLayer) * vset->hnsw->node_count;
    layers_storage = layers_storage * 4 / 3; // 1.33 times.
    size += layers_storage;

    /* All the nodes have layer 0 links. */
    uint64_t level0_links = node->layers[0].max_links;
    uint64_t other_levels_links = level0_links/2;
    size += sizeof(hnswNode*) * level0_links * vset->hnsw->node_count;

    /* Add the 0.33 remaining part, but upper layers have less links. */
    size += (sizeof(hnswNode*) * other_levels_links * vset->hnsw->node_count)/3;

    /* Associated string value and attributres.
     * Use Redis Module API to get string size, and guess that all the
     * elements have similar size as the first few. */
    size_t items_scanned = 0, items_size = 0;
    size_t attribs_scanned = 0, attribs_size = 0;
    int scan_effort = 20;
    while(scan_effort > 0 && node) {
        struct vsetNodeVal *nv = node->value;
        items_size += RedisModule_MallocSizeString(nv->item);
        items_scanned++;
        if (nv->attrib) {
            attribs_size += RedisModule_MallocSizeString(nv->attrib);
            attribs_scanned++;
        }
        scan_effort--;
        node = node->next;
    }

    /* Add the memory usage due to items. */
    if (items_scanned)
        size += items_size / items_scanned * vset->hnsw->node_count;

    /* Add memory usage due to attributres. */
    if (attribs_scanned == 0) {
        /* We were not lucky enough to find a single attribute in the
         * first few items? Let's use a fixed arbitrary value. */
        attribs_scanned = 1;
        attribs_size = 64;
    }
    size += attribs_size / attribs_scanned * vset->numattribs;

    /* Account for dictionary overhead - this is an approximation. */
    size += RedisModule_DictSize(vset->dict) * (sizeof(void*) * 2);

    return size;
}

/* Free the entire data structure */
void VectorSetFree(void *value) {
    struct vsetObject *vset = value;

    vectorSetWaitAllBackgroundClients(vset,1);
    vectorSetReleaseObject(value);
}

/* Add object digest to the digest context */
void VectorSetDigest(RedisModuleDigest *md, void *value) {
    struct vsetObject *vset = value;

    /* Add consistent order-independent hash of all vectors */
    hnswNode *node = vset->hnsw->head;

    /* Hash the vector dimension and number of nodes. */
    RedisModule_DigestAddLongLong(md, vset->hnsw->node_count);
    RedisModule_DigestAddLongLong(md, vset->hnsw->vector_dim);
    RedisModule_DigestEndSequence(md);

    while(node) {
        struct vsetNodeVal *nv = node->value;
        /* Hash each vector component */
        RedisModule_DigestAddStringBuffer(md, node->vector, hnsw_quants_bytes(vset->hnsw));
        /* Hash the associated value */
        size_t len;
        const char *str = RedisModule_StringPtrLen(nv->item, &len);
        RedisModule_DigestAddStringBuffer(md, (char*)str, len);
        if (nv->attrib) {
            str = RedisModule_StringPtrLen(nv->attrib, &len);
            RedisModule_DigestAddStringBuffer(md, (char*)str, len);
        }
        node = node->next;
        RedisModule_DigestEndSequence(md);
    }
}

// int VectorSets_InitModuleConfig(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
int VectorSets_InitModuleConfig(RedisModuleCtx *ctx) {
    if (RegisterModuleConfig(ctx) == REDISMODULE_ERR) {
        RedisModule_Log(ctx, "warning", "Error registering module configuration");
        return REDISMODULE_ERR;
    }
    // Load default values
    if (RedisModule_LoadDefaultConfigs(ctx) == REDISMODULE_ERR) {
        RedisModule_Log(ctx, "warning", "Error loading default module configuration");
        return REDISMODULE_ERR;
    } else {
        RedisModule_Log(ctx, "verbose", "Successfully loaded default module configuration");
    }
    if (RedisModule_LoadConfigs(ctx) == REDISMODULE_ERR) {
        RedisModule_Log(ctx, "warning", "Error loading user module configuration");
        return REDISMODULE_ERR;
    } else {
        RedisModule_Log(ctx, "verbose", "Successfully loaded user module configuration");
    }
    return REDISMODULE_OK;
}

/* This function must be present on each Redis module. It is used in order to
 * register the commands into the Redis server. */
int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx,"vectorset",1,REDISMODULE_APIVER_1)
        == REDISMODULE_ERR) return REDISMODULE_ERR;

    if (VectorSets_InitModuleConfig(ctx) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    RedisModule_SetModuleOptions(ctx, REDISMODULE_OPTIONS_HANDLE_IO_ERRORS|REDISMODULE_OPTIONS_HANDLE_REPL_ASYNC_LOAD);

    RedisModuleTypeMethods tm = {
        .version = REDISMODULE_TYPE_METHOD_VERSION,
        .rdb_load = VectorSetRdbLoad,
        .rdb_save = VectorSetRdbSave,
        .aof_rewrite = NULL,
        .mem_usage = VectorSetMemUsage,
        .free = VectorSetFree,
        .digest = VectorSetDigest
    };

    VectorSetType = RedisModule_CreateDataType(ctx,"vectorset",0,&tm);
    if (VectorSetType == NULL) return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"VADD",
        VADD_RedisCommand,"write deny-oom",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"VREM",
        VREM_RedisCommand,"write",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"VSIM",
        VSIM_RedisCommand,"readonly",1,1,1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "VDIM",
        VDIM_RedisCommand, "readonly fast", 1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "VCARD",
        VCARD_RedisCommand, "readonly fast", 1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "VEMB",
        VEMB_RedisCommand, "readonly fast", 1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "VLINKS",
        VLINKS_RedisCommand, "readonly fast", 1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "VINFO",
        VINFO_RedisCommand, "readonly fast", 1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "VSETATTR",
        VSETATTR_RedisCommand, "write fast", 1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "VGETATTR",
        VGETATTR_RedisCommand, "readonly fast", 1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "VRANDMEMBER",
        VRANDMEMBER_RedisCommand, "readonly", 1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "VISMEMBER",
        VISMEMBER_RedisCommand, "readonly", 1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    hnsw_set_allocator(RedisModule_Free, RedisModule_Alloc,
                       RedisModule_Realloc);

    return REDISMODULE_OK;
}

int VectorSets_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    return RedisModule_OnLoad(ctx, argv, argc);
}
