#include <assert.h>
#include <stdalign.h>
#include <stdio.h>
#include <string.h>

#include <roaring/art/art.h>
#include <roaring/memory.h>
#include <roaring/portability.h>

#define CROARING_ART_NULL_REF 0

#define CROARING_ART_LEAF_TYPE 1
#define CROARING_ART_NODE4_TYPE 2
#define CROARING_ART_NODE16_TYPE 3
#define CROARING_ART_NODE48_TYPE 4
#define CROARING_ART_NODE256_TYPE 5

#define CROARING_ART_MIN_TYPE CROARING_ART_LEAF_TYPE
#define CROARING_ART_MAX_TYPE CROARING_ART_NODE256_TYPE

// Node48 placeholder value to indicate no child is present at this key index.
#define CROARING_ART_NODE48_EMPTY_VAL 48
#define CROARING_NODE48_AVAILABLE_CHILDREN_MASK ((UINT64_C(1) << 48) - 1)

#define CROARING_ART_ALIGN_BUF(buf, alignment)      \
    (char *)(((uintptr_t)(buf) + ((alignment)-1)) & \
             (ptrdiff_t)(~((alignment)-1)))

// Gives the byte difference needed to align the current buffer to the
// alignment, relative to the start of the buffer.
#define CROARING_ART_ALIGN_SIZE_RELATIVE(buf_cur, buf_start, alignment) \
    ((((ptrdiff_t)((buf_cur) - (buf_start)) + ((alignment)-1)) &        \
      (ptrdiff_t)(~((alignment)-1))) -                                  \
     (ptrdiff_t)((buf_cur) - (buf_start)))

#ifdef __cplusplus
extern "C" {
namespace roaring {
namespace internal {
#endif

typedef uint8_t art_typecode_t;

typedef struct art_leaf_s {
    union {
        struct {
            art_key_chunk_t key[ART_KEY_BYTES];
            art_val_t val;
        };
        uint64_t next_free;
    };
} art_leaf_t;

// Inner node, with prefix.
//
// We use a fixed-length array as a pointer would be larger than the array.
typedef struct art_inner_node_s {
    uint8_t prefix_size;
    uint8_t prefix[ART_KEY_BYTES - 1];
} art_inner_node_t;

// Inner node types.

// Node4: key[i] corresponds with children[i]. Keys are sorted.
typedef struct art_node4_s {
    union {
        struct {
            art_inner_node_t base;
            uint8_t count;
            uint8_t keys[4];
            art_ref_t children[4];
        };
        uint64_t next_free;
    };
} art_node4_t;

// Node16: key[i] corresponds with children[i]. Keys are sorted.
typedef struct art_node16_s {
    union {
        struct {
            art_inner_node_t base;
            uint8_t count;
            uint8_t keys[16];
            art_ref_t children[16];
        };
        uint64_t next_free;
    };
} art_node16_t;

// Node48: key[i] corresponds with children[key[i]] if key[i] !=
// CROARING_ART_NODE48_EMPTY_VAL. Keys are naturally sorted due to direct
// indexing.
typedef struct art_node48_s {
    union {
        struct {
            art_inner_node_t base;
            uint8_t count;
            // Bitset where the ith bit is set if children[i] is available
            // Because there are at most 48 children, only the bottom 48 bits
            // are used.
            uint64_t available_children;
            uint8_t keys[256];
            art_ref_t children[48];
        };
        uint64_t next_free;
    };
} art_node48_t;

// Node256: children[i] is directly indexed by key chunk. A child is present if
// children[i] != NULL.
typedef struct art_node256_s {
    union {
        struct {
            art_inner_node_t base;
            uint16_t count;
            art_ref_t children[256];
        };
        uint64_t next_free;
    };
} art_node256_t;

// Size of each node type, indexed by typecode for convenience.
static const size_t ART_NODE_SIZES[] = {
    0,
    sizeof(art_leaf_t),
    sizeof(art_node4_t),
    sizeof(art_node16_t),
    sizeof(art_node48_t),
    sizeof(art_node256_t),
};

// Helper struct to refer to a child within a node at a specific index.
typedef struct art_indexed_child_s {
    art_ref_t child;
    uint8_t index;
    art_key_chunk_t key_chunk;
} art_indexed_child_t;

typedef struct art_internal_validate_s {
    const char **reason;
    art_validate_cb_t validate_cb;
    void *context;

    int depth;
    art_key_chunk_t current_key[ART_KEY_BYTES];
} art_internal_validate_t;

// Set the reason message, and return false for convenience.
static inline bool art_validate_fail(const art_internal_validate_t *validate,
                                     const char *msg) {
    *validate->reason = msg;
    return false;
}

static inline art_ref_t art_to_ref(uint64_t index, art_typecode_t typecode) {
    return ((art_ref_t)index) << 16 | typecode;
}

static inline uint64_t art_ref_index(art_ref_t ref) {
    return ((uint64_t)ref) >> 16;
}

static inline art_typecode_t art_ref_typecode(art_ref_t ref) {
    return (art_typecode_t)ref;
}

/**
 * Gets a pointer to a node from its reference. The pointer only remains valid
 * under non-mutating operations. If any mutating operations occur, this
 * function should be called again to get a valid pointer to the node.
 */
static art_node_t *art_deref(const art_t *art, art_ref_t ref) {
    assert(ref != CROARING_ART_NULL_REF);
    art_typecode_t typecode = art_ref_typecode(ref);
    return (art_node_t *)((char *)art->nodes[typecode] +
                          art_ref_index(ref) * ART_NODE_SIZES[typecode]);
}

static inline art_node_t *art_get_node(const art_t *art, uint64_t index,
                                       art_typecode_t typecode) {
    return art_deref(art, art_to_ref(index, typecode));
}

static inline uint64_t art_get_index(const art_t *art, const art_node_t *node,
                                     art_typecode_t typecode) {
    art_node_t *nodes = art->nodes[typecode];
    switch (typecode) {
        case CROARING_ART_LEAF_TYPE:
            return (art_leaf_t *)node - (art_leaf_t *)nodes;
        case CROARING_ART_NODE4_TYPE:
            return (art_node4_t *)node - (art_node4_t *)nodes;
        case CROARING_ART_NODE16_TYPE:
            return (art_node16_t *)node - (art_node16_t *)nodes;
        case CROARING_ART_NODE48_TYPE:
            return (art_node48_t *)node - (art_node48_t *)nodes;
        case CROARING_ART_NODE256_TYPE:
            return (art_node256_t *)node - (art_node256_t *)nodes;
        default:
            assert(false);
            return 0;
    }
}

/**
 * Creates a reference from a pointer.
 */
static inline art_ref_t art_get_ref(const art_t *art, const art_node_t *node,
                                    art_typecode_t typecode) {
    return art_to_ref(art_get_index(art, node, typecode), typecode);
}

static inline bool art_is_leaf(art_ref_t ref) {
    return art_ref_typecode(ref) == CROARING_ART_LEAF_TYPE;
}

static inline void art_init_inner_node(art_inner_node_t *node,
                                       const art_key_chunk_t prefix[],
                                       uint8_t prefix_size) {
    node->prefix_size = prefix_size;
    memcpy(node->prefix, prefix, prefix_size * sizeof(art_key_chunk_t));
}

static void art_node_free(art_t *art, art_node_t *node,
                          art_typecode_t typecode);

static uint64_t art_allocate_index(art_t *art, art_typecode_t typecode);

// ===================== Start of node-specific functions ======================

static art_ref_t art_leaf_create(art_t *art, const art_key_chunk_t key[],
                                 art_val_t val) {
    uint64_t index = art_allocate_index(art, CROARING_ART_LEAF_TYPE);
    art_leaf_t *leaf =
        ((art_leaf_t *)art->nodes[CROARING_ART_LEAF_TYPE]) + index;
    memcpy(leaf->key, key, ART_KEY_BYTES);
    leaf->val = val;
    return art_to_ref(index, CROARING_ART_LEAF_TYPE);
}

static art_node4_t *art_node4_create(art_t *art, const art_key_chunk_t prefix[],
                                     uint8_t prefix_size);
static art_node16_t *art_node16_create(art_t *art,
                                       const art_key_chunk_t prefix[],
                                       uint8_t prefix_size);
static art_node48_t *art_node48_create(art_t *art,
                                       const art_key_chunk_t prefix[],
                                       uint8_t prefix_size);
static art_node256_t *art_node256_create(art_t *art,
                                         const art_key_chunk_t prefix[],
                                         uint8_t prefix_size);

static art_ref_t art_node4_insert(art_t *art, art_node4_t *node,
                                  art_ref_t child, uint8_t key);
static art_ref_t art_node16_insert(art_t *art, art_node16_t *node,
                                   art_ref_t child, uint8_t key);
static art_ref_t art_node48_insert(art_t *art, art_node48_t *node,
                                   art_ref_t child, uint8_t key);
static art_ref_t art_node256_insert(art_t *art, art_node256_t *node,
                                    art_ref_t child, uint8_t key);

static art_node4_t *art_node4_create(art_t *art, const art_key_chunk_t prefix[],
                                     uint8_t prefix_size) {
    uint64_t index = art_allocate_index(art, CROARING_ART_NODE4_TYPE);
    art_node4_t *node =
        ((art_node4_t *)art->nodes[CROARING_ART_NODE4_TYPE]) + index;
    art_init_inner_node(&node->base, prefix, prefix_size);
    node->count = 0;
    return node;
}

static inline art_ref_t art_node4_find_child(const art_node4_t *node,
                                             art_key_chunk_t key) {
    for (size_t i = 0; i < node->count; ++i) {
        if (node->keys[i] == key) {
            return node->children[i];
        }
    }
    return CROARING_ART_NULL_REF;
}

static art_ref_t art_node4_insert(art_t *art, art_node4_t *node,
                                  art_ref_t child, uint8_t key) {
    if (node->count < 4) {
        size_t idx = 0;
        for (; idx < node->count; ++idx) {
            if (node->keys[idx] > key) {
                break;
            }
        }
        size_t after = node->count - idx;
        // Shift other keys to maintain sorted order.
        memmove(node->keys + idx + 1, node->keys + idx,
                after * sizeof(art_key_chunk_t));
        memmove(node->children + idx + 1, node->children + idx,
                after * sizeof(art_ref_t));

        node->children[idx] = child;
        node->keys[idx] = key;
        node->count++;
        return art_get_ref(art, (art_node_t *)node, CROARING_ART_NODE4_TYPE);
    }
    art_node16_t *new_node =
        art_node16_create(art, node->base.prefix, node->base.prefix_size);
    // Instead of calling insert, this could be specialized to 2x memcpy and
    // setting the count.
    for (size_t i = 0; i < 4; ++i) {
        art_node16_insert(art, new_node, node->children[i], node->keys[i]);
    }
    art_node_free(art, (art_node_t *)node, CROARING_ART_NODE4_TYPE);
    return art_node16_insert(art, new_node, child, key);
}

static inline art_ref_t art_node4_erase(art_t *art, art_node4_t *node,
                                        art_key_chunk_t key_chunk) {
    int idx = -1;
    for (size_t i = 0; i < node->count; ++i) {
        if (node->keys[i] == key_chunk) {
            idx = i;
        }
    }
    if (idx == -1) {
        return art_get_ref(art, (art_node_t *)node, CROARING_ART_NODE4_TYPE);
    }
    if (node->count == 2) {
        // Only one child remains after erasing, so compress the path by
        // removing this node.
        uint8_t other_idx = idx ^ 1;
        art_ref_t remaining_child = node->children[other_idx];
        art_key_chunk_t remaining_child_key = node->keys[other_idx];
        if (!art_is_leaf(remaining_child)) {
            // Correct the prefix of the child node.
            art_inner_node_t *inner_node =
                (art_inner_node_t *)art_deref(art, remaining_child);
            memmove(inner_node->prefix + node->base.prefix_size + 1,
                    inner_node->prefix, inner_node->prefix_size);
            memcpy(inner_node->prefix, node->base.prefix,
                   node->base.prefix_size);
            inner_node->prefix[node->base.prefix_size] = remaining_child_key;
            inner_node->prefix_size += node->base.prefix_size + 1;
        }
        art_node_free(art, (art_node_t *)node, CROARING_ART_NODE4_TYPE);
        return remaining_child;
    }
    // Shift other keys to maintain sorted order.
    size_t after_next = node->count - idx - 1;
    memmove(node->keys + idx, node->keys + idx + 1,
            after_next * sizeof(art_key_chunk_t));
    memmove(node->children + idx, node->children + idx + 1,
            after_next * sizeof(art_ref_t));
    node->count--;
    return art_get_ref(art, (art_node_t *)node, CROARING_ART_NODE4_TYPE);
}

static inline void art_node4_replace(art_node4_t *node,
                                     art_key_chunk_t key_chunk,
                                     art_ref_t new_child) {
    for (size_t i = 0; i < node->count; ++i) {
        if (node->keys[i] == key_chunk) {
            node->children[i] = new_child;
            return;
        }
    }
}

static inline art_indexed_child_t art_node4_next_child(const art_node4_t *node,
                                                       int index) {
    art_indexed_child_t indexed_child;
    index++;
    if (index >= node->count) {
        indexed_child.child = CROARING_ART_NULL_REF;
        return indexed_child;
    }
    indexed_child.index = index;
    indexed_child.child = node->children[index];
    indexed_child.key_chunk = node->keys[index];
    return indexed_child;
}

static inline art_indexed_child_t art_node4_prev_child(const art_node4_t *node,
                                                       int index) {
    if (index > node->count) {
        index = node->count;
    }
    index--;
    art_indexed_child_t indexed_child;
    if (index < 0) {
        indexed_child.child = CROARING_ART_NULL_REF;
        return indexed_child;
    }
    indexed_child.index = index;
    indexed_child.child = node->children[index];
    indexed_child.key_chunk = node->keys[index];
    return indexed_child;
}

static inline art_indexed_child_t art_node4_child_at(const art_node4_t *node,
                                                     int index) {
    art_indexed_child_t indexed_child;
    if (index < 0 || index >= node->count) {
        indexed_child.child = CROARING_ART_NULL_REF;
        return indexed_child;
    }
    indexed_child.index = index;
    indexed_child.child = node->children[index];
    indexed_child.key_chunk = node->keys[index];
    return indexed_child;
}

static inline art_indexed_child_t art_node4_lower_bound(
    art_node4_t *node, art_key_chunk_t key_chunk) {
    art_indexed_child_t indexed_child;
    for (size_t i = 0; i < node->count; ++i) {
        if (node->keys[i] >= key_chunk) {
            indexed_child.index = i;
            indexed_child.child = node->children[i];
            indexed_child.key_chunk = node->keys[i];
            return indexed_child;
        }
    }
    indexed_child.child = CROARING_ART_NULL_REF;
    return indexed_child;
}

static bool art_internal_validate_at(const art_t *art, art_ref_t ref,
                                     art_internal_validate_t validator);

static bool art_node4_internal_validate(const art_t *art,
                                        const art_node4_t *node,
                                        art_internal_validate_t validator) {
    if (node->count == 0) {
        return art_validate_fail(&validator, "Node4 has no children");
    }
    if (node->count > 4) {
        return art_validate_fail(&validator, "Node4 has too many children");
    }
    if (node->count == 1) {
        return art_validate_fail(
            &validator, "Node4 and child node should have been combined");
    }
    validator.depth++;
    for (int i = 0; i < node->count; ++i) {
        if (i > 0) {
            if (node->keys[i - 1] >= node->keys[i]) {
                return art_validate_fail(
                    &validator, "Node4 keys are not strictly increasing");
            }
        }
        for (int j = i + 1; j < node->count; ++j) {
            if (node->children[i] == node->children[j]) {
                return art_validate_fail(&validator,
                                         "Node4 has duplicate children");
            }
        }
        validator.current_key[validator.depth - 1] = node->keys[i];
        if (!art_internal_validate_at(art, node->children[i], validator)) {
            return false;
        }
    }
    return true;
}

static art_node16_t *art_node16_create(art_t *art,
                                       const art_key_chunk_t prefix[],
                                       uint8_t prefix_size) {
    uint64_t index = art_allocate_index(art, CROARING_ART_NODE16_TYPE);
    art_node16_t *node =
        ((art_node16_t *)art->nodes[CROARING_ART_NODE16_TYPE]) + index;
    art_init_inner_node(&node->base, prefix, prefix_size);
    node->count = 0;
    return node;
}

static inline art_ref_t art_node16_find_child(const art_node16_t *node,
                                              art_key_chunk_t key) {
    for (size_t i = 0; i < node->count; ++i) {
        if (node->keys[i] == key) {
            return node->children[i];
        }
    }
    return CROARING_ART_NULL_REF;
}

static art_ref_t art_node16_insert(art_t *art, art_node16_t *node,
                                   art_ref_t child, uint8_t key) {
    if (node->count < 16) {
        size_t idx = 0;
        for (; idx < node->count; ++idx) {
            if (node->keys[idx] > key) {
                break;
            }
        }
        size_t after = node->count - idx;
        // Shift other keys to maintain sorted order.
        memmove(node->keys + idx + 1, node->keys + idx,
                after * sizeof(art_key_chunk_t));
        memmove(node->children + idx + 1, node->children + idx,
                after * sizeof(art_ref_t));

        node->children[idx] = child;
        node->keys[idx] = key;
        node->count++;
        return art_get_ref(art, (art_node_t *)node, CROARING_ART_NODE16_TYPE);
    }
    art_node48_t *new_node =
        art_node48_create(art, node->base.prefix, node->base.prefix_size);
    for (size_t i = 0; i < 16; ++i) {
        art_node48_insert(art, new_node, node->children[i], node->keys[i]);
    }
    art_node_free(art, (art_node_t *)node, CROARING_ART_NODE16_TYPE);
    return art_node48_insert(art, new_node, child, key);
}

static inline art_ref_t art_node16_erase(art_t *art, art_node16_t *node,
                                         uint8_t key_chunk) {
    for (size_t i = 0; i < node->count; ++i) {
        if (node->keys[i] == key_chunk) {
            // Shift other keys to maintain sorted order.
            size_t after_next = node->count - i - 1;
            memmove(node->keys + i, node->keys + i + 1,
                    after_next * sizeof(key_chunk));
            memmove(node->children + i, node->children + i + 1,
                    after_next * sizeof(art_ref_t));
            node->count--;
            break;
        }
    }
    if (node->count > 4) {
        return art_get_ref(art, (art_node_t *)node, CROARING_ART_NODE16_TYPE);
    }
    art_node4_t *new_node =
        art_node4_create(art, node->base.prefix, node->base.prefix_size);
    // Instead of calling insert, this could be specialized to 2x memcpy and
    // setting the count.
    for (size_t i = 0; i < 4; ++i) {
        art_node4_insert(art, new_node, node->children[i], node->keys[i]);
    }
    art_node_free(art, (art_node_t *)node, CROARING_ART_NODE16_TYPE);
    return art_get_ref(art, (art_node_t *)new_node, CROARING_ART_NODE4_TYPE);
}

static inline void art_node16_replace(art_node16_t *node,
                                      art_key_chunk_t key_chunk,
                                      art_ref_t new_child) {
    for (uint8_t i = 0; i < node->count; ++i) {
        if (node->keys[i] == key_chunk) {
            node->children[i] = new_child;
            return;
        }
    }
}

static inline art_indexed_child_t art_node16_next_child(
    const art_node16_t *node, int index) {
    art_indexed_child_t indexed_child;
    index++;
    if (index >= node->count) {
        indexed_child.child = CROARING_ART_NULL_REF;
        return indexed_child;
    }
    indexed_child.index = index;
    indexed_child.child = node->children[index];
    indexed_child.key_chunk = node->keys[index];
    return indexed_child;
}

static inline art_indexed_child_t art_node16_prev_child(
    const art_node16_t *node, int index) {
    if (index > node->count) {
        index = node->count;
    }
    index--;
    art_indexed_child_t indexed_child;
    if (index < 0) {
        indexed_child.child = CROARING_ART_NULL_REF;
        return indexed_child;
    }
    indexed_child.index = index;
    indexed_child.child = node->children[index];
    indexed_child.key_chunk = node->keys[index];
    return indexed_child;
}

static inline art_indexed_child_t art_node16_child_at(const art_node16_t *node,
                                                      int index) {
    art_indexed_child_t indexed_child;
    if (index < 0 || index >= node->count) {
        indexed_child.child = CROARING_ART_NULL_REF;
        return indexed_child;
    }
    indexed_child.index = index;
    indexed_child.child = node->children[index];
    indexed_child.key_chunk = node->keys[index];
    return indexed_child;
}

static inline art_indexed_child_t art_node16_lower_bound(
    art_node16_t *node, art_key_chunk_t key_chunk) {
    art_indexed_child_t indexed_child;
    for (size_t i = 0; i < node->count; ++i) {
        if (node->keys[i] >= key_chunk) {
            indexed_child.index = i;
            indexed_child.child = node->children[i];
            indexed_child.key_chunk = node->keys[i];
            return indexed_child;
        }
    }
    indexed_child.child = CROARING_ART_NULL_REF;
    return indexed_child;
}

static bool art_node16_internal_validate(const art_t *art,
                                         const art_node16_t *node,
                                         art_internal_validate_t validator) {
    if (node->count <= 4) {
        return art_validate_fail(&validator, "Node16 has too few children");
    }
    if (node->count > 16) {
        return art_validate_fail(&validator, "Node16 has too many children");
    }
    validator.depth++;
    for (int i = 0; i < node->count; ++i) {
        if (i > 0) {
            if (node->keys[i - 1] >= node->keys[i]) {
                return art_validate_fail(
                    &validator, "Node16 keys are not strictly increasing");
            }
        }
        for (int j = i + 1; j < node->count; ++j) {
            if (node->children[i] == node->children[j]) {
                return art_validate_fail(&validator,
                                         "Node16 has duplicate children");
            }
        }
        validator.current_key[validator.depth - 1] = node->keys[i];
        if (!art_internal_validate_at(art, node->children[i], validator)) {
            return false;
        }
    }
    return true;
}

static art_node48_t *art_node48_create(art_t *art,
                                       const art_key_chunk_t prefix[],
                                       uint8_t prefix_size) {
    uint64_t index = art_allocate_index(art, CROARING_ART_NODE48_TYPE);
    art_node48_t *node =
        ((art_node48_t *)art->nodes[CROARING_ART_NODE48_TYPE]) + index;
    art_init_inner_node(&node->base, prefix, prefix_size);
    node->count = 0;
    node->available_children = CROARING_NODE48_AVAILABLE_CHILDREN_MASK;
    for (size_t i = 0; i < 256; ++i) {
        node->keys[i] = CROARING_ART_NODE48_EMPTY_VAL;
    }
    return node;
}

static inline art_ref_t art_node48_find_child(const art_node48_t *node,
                                              art_key_chunk_t key) {
    uint8_t val_idx = node->keys[key];
    if (val_idx != CROARING_ART_NODE48_EMPTY_VAL) {
        return node->children[val_idx];
    }
    return CROARING_ART_NULL_REF;
}

static art_ref_t art_node48_insert(art_t *art, art_node48_t *node,
                                   art_ref_t child, uint8_t key) {
    if (node->count < 48) {
        // node->available_children is only zero when the node is full (count ==
        // 48), we just checked count < 48
        uint8_t val_idx = roaring_trailing_zeroes(node->available_children);
        node->keys[key] = val_idx;
        node->children[val_idx] = child;
        node->count++;
        node->available_children &= ~(UINT64_C(1) << val_idx);
        return art_get_ref(art, (art_node_t *)node, CROARING_ART_NODE48_TYPE);
    }
    art_node256_t *new_node =
        art_node256_create(art, node->base.prefix, node->base.prefix_size);
    for (size_t i = 0; i < 256; ++i) {
        uint8_t val_idx = node->keys[i];
        if (val_idx != CROARING_ART_NODE48_EMPTY_VAL) {
            art_node256_insert(art, new_node, node->children[val_idx], i);
        }
    }
    art_node_free(art, (art_node_t *)node, CROARING_ART_NODE48_TYPE);
    return art_node256_insert(art, new_node, child, key);
}

static inline art_ref_t art_node48_erase(art_t *art, art_node48_t *node,
                                         uint8_t key_chunk) {
    uint8_t val_idx = node->keys[key_chunk];
    if (val_idx == CROARING_ART_NODE48_EMPTY_VAL) {
        return art_get_ref(art, (art_node_t *)node, CROARING_ART_NODE48_TYPE);
    }
    node->keys[key_chunk] = CROARING_ART_NODE48_EMPTY_VAL;
    node->available_children |= UINT64_C(1) << val_idx;
    node->count--;
    if (node->count > 16) {
        return art_get_ref(art, (art_node_t *)node, CROARING_ART_NODE48_TYPE);
    }

    art_node16_t *new_node =
        art_node16_create(art, node->base.prefix, node->base.prefix_size);
    for (size_t i = 0; i < 256; ++i) {
        val_idx = node->keys[i];
        if (val_idx != CROARING_ART_NODE48_EMPTY_VAL) {
            art_node16_insert(art, new_node, node->children[val_idx], i);
        }
    }
    art_node_free(art, (art_node_t *)node, CROARING_ART_NODE48_TYPE);
    return art_get_ref(art, (art_node_t *)new_node, CROARING_ART_NODE16_TYPE);
}

static inline void art_node48_replace(art_node48_t *node,
                                      art_key_chunk_t key_chunk,
                                      art_ref_t new_child) {
    uint8_t val_idx = node->keys[key_chunk];
    assert(val_idx != CROARING_ART_NODE48_EMPTY_VAL);
    node->children[val_idx] = new_child;
}

static inline art_indexed_child_t art_node48_next_child(
    const art_node48_t *node, int index) {
    art_indexed_child_t indexed_child;
    index++;
    for (size_t i = index; i < 256; ++i) {
        if (node->keys[i] != CROARING_ART_NODE48_EMPTY_VAL) {
            indexed_child.index = i;
            indexed_child.child = node->children[node->keys[i]];
            indexed_child.key_chunk = i;
            return indexed_child;
        }
    }
    indexed_child.child = CROARING_ART_NULL_REF;
    return indexed_child;
}

static inline art_indexed_child_t art_node48_prev_child(
    const art_node48_t *node, int index) {
    if (index > 256) {
        index = 256;
    }
    index--;
    art_indexed_child_t indexed_child;
    for (int i = index; i >= 0; --i) {
        if (node->keys[i] != CROARING_ART_NODE48_EMPTY_VAL) {
            indexed_child.index = i;
            indexed_child.child = node->children[node->keys[i]];
            indexed_child.key_chunk = i;
            return indexed_child;
        }
    }
    indexed_child.child = CROARING_ART_NULL_REF;
    return indexed_child;
}

static inline art_indexed_child_t art_node48_child_at(const art_node48_t *node,
                                                      int index) {
    art_indexed_child_t indexed_child;
    if (index < 0 || index >= 256) {
        indexed_child.child = CROARING_ART_NULL_REF;
        return indexed_child;
    }
    indexed_child.index = index;
    indexed_child.child = node->children[node->keys[index]];
    indexed_child.key_chunk = index;
    return indexed_child;
}

static inline art_indexed_child_t art_node48_lower_bound(
    art_node48_t *node, art_key_chunk_t key_chunk) {
    art_indexed_child_t indexed_child;
    for (size_t i = key_chunk; i < 256; ++i) {
        if (node->keys[i] != CROARING_ART_NODE48_EMPTY_VAL) {
            indexed_child.index = i;
            indexed_child.child = node->children[node->keys[i]];
            indexed_child.key_chunk = i;
            return indexed_child;
        }
    }
    indexed_child.child = CROARING_ART_NULL_REF;
    return indexed_child;
}

static bool art_node48_internal_validate(const art_t *art,
                                         const art_node48_t *node,
                                         art_internal_validate_t validator) {
    if (node->count <= 16) {
        return art_validate_fail(&validator, "Node48 has too few children");
    }
    if (node->count > 48) {
        return art_validate_fail(&validator, "Node48 has too many children");
    }
    uint64_t used_children = 0;
    for (int i = 0; i < 256; ++i) {
        uint8_t child_idx = node->keys[i];
        if (child_idx != CROARING_ART_NODE48_EMPTY_VAL) {
            if (used_children & (UINT64_C(1) << child_idx)) {
                return art_validate_fail(
                    &validator, "Node48 keys point to the same child index");
            }

            art_ref_t child = node->children[child_idx];
            if (child == CROARING_ART_NULL_REF) {
                return art_validate_fail(&validator, "Node48 has a NULL child");
            }
            used_children |= UINT64_C(1) << child_idx;
        }
    }
    uint64_t expected_used_children =
        (node->available_children) ^ CROARING_NODE48_AVAILABLE_CHILDREN_MASK;
    if (used_children != expected_used_children) {
        return art_validate_fail(
            &validator,
            "Node48 available_children does not match actual children");
    }
    while (used_children != 0) {
        uint8_t child_idx = roaring_trailing_zeroes(used_children);
        used_children &= used_children - 1;

        uint64_t other_children = used_children;
        while (other_children != 0) {
            uint8_t other_child_idx = roaring_trailing_zeroes(other_children);
            if (node->children[child_idx] == node->children[other_child_idx]) {
                return art_validate_fail(&validator,
                                         "Node48 has duplicate children");
            }
            other_children &= other_children - 1;
        }
    }

    validator.depth++;
    for (int i = 0; i < 256; ++i) {
        if (node->keys[i] != CROARING_ART_NODE48_EMPTY_VAL) {
            validator.current_key[validator.depth - 1] = i;
            if (!art_internal_validate_at(art, node->children[node->keys[i]],
                                          validator)) {
                return false;
            }
        }
    }
    return true;
}

static art_node256_t *art_node256_create(art_t *art,
                                         const art_key_chunk_t prefix[],
                                         uint8_t prefix_size) {
    uint64_t index = art_allocate_index(art, CROARING_ART_NODE256_TYPE);
    art_node256_t *node =
        ((art_node256_t *)art->nodes[CROARING_ART_NODE256_TYPE]) + index;
    art_init_inner_node(&node->base, prefix, prefix_size);
    node->count = 0;
    for (size_t i = 0; i < 256; ++i) {
        node->children[i] = CROARING_ART_NULL_REF;
    }
    return node;
}

static inline art_ref_t art_node256_find_child(const art_node256_t *node,
                                               art_key_chunk_t key) {
    return node->children[key];
}

static art_ref_t art_node256_insert(art_t *art, art_node256_t *node,
                                    art_ref_t child, uint8_t key) {
    node->children[key] = child;
    node->count++;
    return art_get_ref(art, (art_node_t *)node, CROARING_ART_NODE256_TYPE);
}

static inline art_ref_t art_node256_erase(art_t *art, art_node256_t *node,
                                          uint8_t key_chunk) {
    node->children[key_chunk] = CROARING_ART_NULL_REF;
    node->count--;
    if (node->count > 48) {
        return art_get_ref(art, (art_node_t *)node, CROARING_ART_NODE256_TYPE);
    }

    art_node48_t *new_node =
        art_node48_create(art, node->base.prefix, node->base.prefix_size);
    for (size_t i = 0; i < 256; ++i) {
        if (node->children[i] != CROARING_ART_NULL_REF) {
            art_node48_insert(art, new_node, node->children[i], i);
        }
    }
    art_node_free(art, (art_node_t *)node, CROARING_ART_NODE256_TYPE);
    return art_get_ref(art, (art_node_t *)new_node, CROARING_ART_NODE48_TYPE);
}

static inline void art_node256_replace(art_node256_t *node,
                                       art_key_chunk_t key_chunk,
                                       art_ref_t new_child) {
    node->children[key_chunk] = new_child;
}

static inline art_indexed_child_t art_node256_next_child(
    const art_node256_t *node, int index) {
    art_indexed_child_t indexed_child;
    index++;
    for (size_t i = index; i < 256; ++i) {
        if (node->children[i] != CROARING_ART_NULL_REF) {
            indexed_child.index = i;
            indexed_child.child = node->children[i];
            indexed_child.key_chunk = i;
            return indexed_child;
        }
    }
    indexed_child.child = CROARING_ART_NULL_REF;
    return indexed_child;
}

static inline art_indexed_child_t art_node256_prev_child(
    const art_node256_t *node, int index) {
    if (index > 256) {
        index = 256;
    }
    index--;
    art_indexed_child_t indexed_child;
    for (int i = index; i >= 0; --i) {
        if (node->children[i] != CROARING_ART_NULL_REF) {
            indexed_child.index = i;
            indexed_child.child = node->children[i];
            indexed_child.key_chunk = i;
            return indexed_child;
        }
    }
    indexed_child.child = CROARING_ART_NULL_REF;
    return indexed_child;
}

static inline art_indexed_child_t art_node256_child_at(
    const art_node256_t *node, int index) {
    art_indexed_child_t indexed_child;
    if (index < 0 || index >= 256) {
        indexed_child.child = CROARING_ART_NULL_REF;
        return indexed_child;
    }
    indexed_child.index = index;
    indexed_child.child = node->children[index];
    indexed_child.key_chunk = index;
    return indexed_child;
}

static inline art_indexed_child_t art_node256_lower_bound(
    art_node256_t *node, art_key_chunk_t key_chunk) {
    art_indexed_child_t indexed_child;
    for (size_t i = key_chunk; i < 256; ++i) {
        if (node->children[i] != CROARING_ART_NULL_REF) {
            indexed_child.index = i;
            indexed_child.child = node->children[i];
            indexed_child.key_chunk = i;
            return indexed_child;
        }
    }
    indexed_child.child = CROARING_ART_NULL_REF;
    return indexed_child;
}

static bool art_node256_internal_validate(const art_t *art,
                                          const art_node256_t *node,
                                          art_internal_validate_t validator) {
    if (node->count <= 48) {
        return art_validate_fail(&validator, "Node256 has too few children");
    }
    if (node->count > 256) {
        return art_validate_fail(&validator, "Node256 has too many children");
    }
    validator.depth++;
    int actual_count = 0;
    for (int i = 0; i < 256; ++i) {
        if (node->children[i] != CROARING_ART_NULL_REF) {
            actual_count++;

            for (int j = i + 1; j < 256; ++j) {
                if (node->children[i] == node->children[j]) {
                    return art_validate_fail(&validator,
                                             "Node256 has duplicate children");
                }
            }

            validator.current_key[validator.depth - 1] = i;
            if (!art_internal_validate_at(art, node->children[i], validator)) {
                return false;
            }
        }
    }
    if (actual_count != node->count) {
        return art_validate_fail(
            &validator, "Node256 count does not match actual children");
    }
    return true;
}

// Finds the child with the given key chunk in the inner node, returns NULL if
// no such child is found.
static art_ref_t art_find_child(const art_inner_node_t *node,
                                art_typecode_t typecode,
                                art_key_chunk_t key_chunk) {
    switch (typecode) {
        case CROARING_ART_NODE4_TYPE:
            return art_node4_find_child((art_node4_t *)node, key_chunk);
        case CROARING_ART_NODE16_TYPE:
            return art_node16_find_child((art_node16_t *)node, key_chunk);
        case CROARING_ART_NODE48_TYPE:
            return art_node48_find_child((art_node48_t *)node, key_chunk);
        case CROARING_ART_NODE256_TYPE:
            return art_node256_find_child((art_node256_t *)node, key_chunk);
        default:
            assert(false);
            return CROARING_ART_NULL_REF;
    }
}

// Replaces the child with the given key chunk in the inner node.
static void art_replace(art_inner_node_t *node, art_typecode_t typecode,
                        art_key_chunk_t key_chunk, art_ref_t new_child) {
    switch (typecode) {
        case CROARING_ART_NODE4_TYPE:
            art_node4_replace((art_node4_t *)node, key_chunk, new_child);
            break;
        case CROARING_ART_NODE16_TYPE:
            art_node16_replace((art_node16_t *)node, key_chunk, new_child);
            break;
        case CROARING_ART_NODE48_TYPE:
            art_node48_replace((art_node48_t *)node, key_chunk, new_child);
            break;
        case CROARING_ART_NODE256_TYPE:
            art_node256_replace((art_node256_t *)node, key_chunk, new_child);
            break;
        default:
            assert(false);
    }
}

// Erases the child with the given key chunk from the inner node, returns the
// updated node (the same as the initial node if it was not shrunk).
static art_ref_t art_node_erase(art_t *art, art_inner_node_t *node,
                                art_typecode_t typecode,
                                art_key_chunk_t key_chunk) {
    switch (typecode) {
        case CROARING_ART_NODE4_TYPE:
            return art_node4_erase(art, (art_node4_t *)node, key_chunk);
        case CROARING_ART_NODE16_TYPE:
            return art_node16_erase(art, (art_node16_t *)node, key_chunk);
        case CROARING_ART_NODE48_TYPE:
            return art_node48_erase(art, (art_node48_t *)node, key_chunk);
        case CROARING_ART_NODE256_TYPE:
            return art_node256_erase(art, (art_node256_t *)node, key_chunk);
        default:
            assert(false);
            return CROARING_ART_NULL_REF;
    }
}

// Inserts the leaf with the given key chunk in the inner node, returns a
// pointer to the (possibly expanded) node.
static art_ref_t art_node_insert_leaf(art_t *art, art_inner_node_t *node,
                                      art_typecode_t typecode,
                                      art_key_chunk_t key_chunk,
                                      art_ref_t leaf) {
    switch (typecode) {
        case CROARING_ART_NODE4_TYPE:
            return art_node4_insert(art, (art_node4_t *)node, leaf, key_chunk);
        case CROARING_ART_NODE16_TYPE:
            return art_node16_insert(art, (art_node16_t *)node, leaf,
                                     key_chunk);
        case CROARING_ART_NODE48_TYPE:
            return art_node48_insert(art, (art_node48_t *)node, leaf,
                                     key_chunk);
        case CROARING_ART_NODE256_TYPE:
            return art_node256_insert(art, (art_node256_t *)node, leaf,
                                      key_chunk);
        default:
            assert(false);
            return CROARING_ART_NULL_REF;
    }
}

static uint64_t art_node_get_next_free(const art_t *art, art_ref_t ref) {
    art_node_t *node = art_deref(art, ref);
    art_typecode_t typecode = art_ref_typecode(ref);
    switch (typecode) {
        case CROARING_ART_LEAF_TYPE:
            return ((art_leaf_t *)node)->next_free;
        case CROARING_ART_NODE4_TYPE:
            return ((art_node4_t *)node)->next_free;
        case CROARING_ART_NODE16_TYPE:
            return ((art_node16_t *)node)->next_free;
        case CROARING_ART_NODE48_TYPE:
            return ((art_node48_t *)node)->next_free;
        case CROARING_ART_NODE256_TYPE:
            return ((art_node256_t *)node)->next_free;
        default:
            assert(false);
            return 0;
    }
}

static void art_node_set_next_free(art_node_t *node, art_typecode_t typecode,
                                   uint64_t next_free) {
    switch (typecode) {
        case CROARING_ART_LEAF_TYPE:
            ((art_leaf_t *)node)->next_free = next_free;
            break;
        case CROARING_ART_NODE4_TYPE:
            ((art_node4_t *)node)->next_free = next_free;
            break;
        case CROARING_ART_NODE16_TYPE:
            ((art_node16_t *)node)->next_free = next_free;
            break;
        case CROARING_ART_NODE48_TYPE:
            ((art_node48_t *)node)->next_free = next_free;
            break;
        case CROARING_ART_NODE256_TYPE:
            ((art_node256_t *)node)->next_free = next_free;
            break;
        default:
            assert(false);
    }
}

// Marks the node as unoccopied and frees its index.
static void art_node_free(art_t *art, art_node_t *node,
                          art_typecode_t typecode) {
    uint64_t index = art_get_index(art, node, typecode);
    uint64_t next_free = art->first_free[typecode];
    art_node_set_next_free(node, typecode, next_free);
    art->first_free[typecode] = index;
}

// Returns the next child in key order, or NULL if called on a leaf.
// Provided index may be in the range [-1, 255].
static art_indexed_child_t art_node_next_child(const art_node_t *node,
                                               art_typecode_t typecode,
                                               int index) {
    switch (typecode) {
        case CROARING_ART_LEAF_TYPE:
            return (art_indexed_child_t){
                .child = CROARING_ART_NULL_REF,
                .index = 0,
                .key_chunk = 0,
            };
        case CROARING_ART_NODE4_TYPE:
            return art_node4_next_child((art_node4_t *)node, index);
        case CROARING_ART_NODE16_TYPE:
            return art_node16_next_child((art_node16_t *)node, index);
        case CROARING_ART_NODE48_TYPE:
            return art_node48_next_child((art_node48_t *)node, index);
        case CROARING_ART_NODE256_TYPE:
            return art_node256_next_child((art_node256_t *)node, index);
        default:
            assert(false);
            return (art_indexed_child_t){0, 0, 0};
    }
}

// Returns the previous child in key order, or NULL if called on a leaf.
// Provided index may be in the range [0, 256].
static art_indexed_child_t art_node_prev_child(const art_node_t *node,
                                               art_typecode_t typecode,
                                               int index) {
    switch (typecode) {
        case CROARING_ART_LEAF_TYPE:
            return (art_indexed_child_t){
                .child = CROARING_ART_NULL_REF,
                .index = 0,
                .key_chunk = 0,
            };
        case CROARING_ART_NODE4_TYPE:
            return art_node4_prev_child((art_node4_t *)node, index);
        case CROARING_ART_NODE16_TYPE:
            return art_node16_prev_child((art_node16_t *)node, index);
        case CROARING_ART_NODE48_TYPE:
            return art_node48_prev_child((art_node48_t *)node, index);
        case CROARING_ART_NODE256_TYPE:
            return art_node256_prev_child((art_node256_t *)node, index);
        default:
            assert(false);
            return (art_indexed_child_t){0, 0, 0};
    }
}

// Returns the child found at the provided index, or NULL if called on a
// leaf. Provided index is only valid if returned by
// art_node_(next|prev)_child.
static art_indexed_child_t art_node_child_at(const art_node_t *node,
                                             art_typecode_t typecode,
                                             int index) {
    switch (typecode) {
        case CROARING_ART_LEAF_TYPE:
            return (art_indexed_child_t){
                .child = CROARING_ART_NULL_REF,
                .index = 0,
                .key_chunk = 0,
            };
        case CROARING_ART_NODE4_TYPE:
            return art_node4_child_at((art_node4_t *)node, index);
        case CROARING_ART_NODE16_TYPE:
            return art_node16_child_at((art_node16_t *)node, index);
        case CROARING_ART_NODE48_TYPE:
            return art_node48_child_at((art_node48_t *)node, index);
        case CROARING_ART_NODE256_TYPE:
            return art_node256_child_at((art_node256_t *)node, index);
        default:
            assert(false);
            return (art_indexed_child_t){0, 0, 0};
    }
}

// Returns the child with the smallest key equal to or greater than the
// given key chunk, NULL if called on a leaf or no such child was found.
static art_indexed_child_t art_node_lower_bound(const art_node_t *node,
                                                art_typecode_t typecode,
                                                art_key_chunk_t key_chunk) {
    switch (typecode) {
        case CROARING_ART_LEAF_TYPE:
            return (art_indexed_child_t){
                .child = CROARING_ART_NULL_REF,
                .index = 0,
                .key_chunk = 0,
            };
        case CROARING_ART_NODE4_TYPE:
            return art_node4_lower_bound((art_node4_t *)node, key_chunk);
        case CROARING_ART_NODE16_TYPE:
            return art_node16_lower_bound((art_node16_t *)node, key_chunk);
        case CROARING_ART_NODE48_TYPE:
            return art_node48_lower_bound((art_node48_t *)node, key_chunk);
        case CROARING_ART_NODE256_TYPE:
            return art_node256_lower_bound((art_node256_t *)node, key_chunk);
        default:
            assert(false);
            return (art_indexed_child_t){0, 0, 0};
    }
}

// ====================== End of node-specific functions ======================

// Compares the given ranges of two keys, returns their relative order:
// * Key range 1 <  key range 2: a negative value
// * Key range 1 == key range 2: 0
// * Key range 1 >  key range 2: a positive value
static inline int art_compare_prefix(const art_key_chunk_t key1[],
                                     uint8_t key1_from,
                                     const art_key_chunk_t key2[],
                                     uint8_t key2_from, uint8_t length) {
    return memcmp(key1 + key1_from, key2 + key2_from, length);
}

// Compares two keys in full, see art_compare_prefix.
int art_compare_keys(const art_key_chunk_t key1[],
                     const art_key_chunk_t key2[]) {
    return art_compare_prefix(key1, 0, key2, 0, ART_KEY_BYTES);
}

// Returns the length of the common prefix between two key ranges.
static uint8_t art_common_prefix(const art_key_chunk_t key1[],
                                 uint8_t key1_from, uint8_t key1_to,
                                 const art_key_chunk_t key2[],
                                 uint8_t key2_from, uint8_t key2_to) {
    uint8_t min_len = key1_to - key1_from;
    uint8_t key2_len = key2_to - key2_from;
    if (key2_len < min_len) {
        min_len = key2_len;
    }
    uint8_t offset = 0;
    for (; offset < min_len; ++offset) {
        if (key1[key1_from + offset] != key2[key2_from + offset]) {
            return offset;
        }
    }
    return offset;
}

/**
 * Extends the array of nodes of the given typecode. Invalidates pointers into
 * the array obtained by `art_deref`.
 */
static void art_extend(art_t *art, art_typecode_t typecode) {
    uint64_t size = art->first_free[typecode];
    uint64_t capacity = art->capacities[typecode];
    if (size < capacity) {
        return;
    }
    uint64_t new_capacity;
    if (capacity == 0) {
        new_capacity = 2;
    } else if (capacity < 1024) {
        new_capacity = 2 * capacity;
    } else {
        new_capacity = 5 * capacity / 4;
    }
    art->capacities[typecode] = new_capacity;
    art->nodes[typecode] = roaring_realloc(
        art->nodes[typecode], new_capacity * ART_NODE_SIZES[typecode]);
    uint64_t increase = new_capacity - capacity;
    memset(art_get_node(art, capacity, typecode), 0,
           increase * ART_NODE_SIZES[typecode]);
    for (uint64_t i = capacity; i < new_capacity; ++i) {
        art_node_set_next_free(art_get_node(art, i, typecode), typecode, i + 1);
    }
}

/**
 * Returns the next free index for the given typecode, may be equal to the
 * capacity of the array.
 */
static uint64_t art_next_free(const art_t *art, art_typecode_t typecode) {
    uint64_t index = art->first_free[typecode];
    return art_node_get_next_free(art, art_to_ref(index, typecode));
}

/**
 * Marks an index for the given typecode as used, expanding the relevant node
 * array if necessary.
 */
static uint64_t art_allocate_index(art_t *art, art_typecode_t typecode) {
    uint64_t first_free = art->first_free[typecode];
    if (first_free == art->capacities[typecode]) {
        art_extend(art, typecode);
        art->first_free[typecode]++;
        return first_free;
    }
    art->first_free[typecode] = art_next_free(art, typecode);
    return first_free;
}

// Returns a pointer to the rootmost node where the value was inserted, may
// not be equal to `node`.
static art_ref_t art_insert_at(art_t *art, art_ref_t ref,
                               const art_key_chunk_t key[], uint8_t depth,
                               art_ref_t new_leaf) {
    if (art_is_leaf(ref)) {
        art_leaf_t *leaf = (art_leaf_t *)art_deref(art, ref);
        uint8_t common_prefix = art_common_prefix(
            leaf->key, depth, ART_KEY_BYTES, key, depth, ART_KEY_BYTES);

        // Previously this was a leaf, create an inner node instead and add
        // both the existing and new leaf to it.
        art_node_t *new_node =
            (art_node_t *)art_node4_create(art, key + depth, common_prefix);

        art_ref_t new_ref = art_node_insert_leaf(
            art, (art_inner_node_t *)new_node, CROARING_ART_NODE4_TYPE,
            leaf->key[depth + common_prefix], ref);
        new_ref = art_node_insert_leaf(art, (art_inner_node_t *)new_node,
                                       CROARING_ART_NODE4_TYPE,
                                       key[depth + common_prefix], new_leaf);

        // The new inner node is now the rootmost node.
        return new_ref;
    }
    art_inner_node_t *inner_node = (art_inner_node_t *)art_deref(art, ref);
    // Not a leaf: inner node
    uint8_t common_prefix =
        art_common_prefix(inner_node->prefix, 0, inner_node->prefix_size, key,
                          depth, ART_KEY_BYTES);
    if (common_prefix != inner_node->prefix_size) {
        // Partial prefix match. Create a new internal node to hold the common
        // prefix.
        // We create a copy of the node's prefix as the creation of a new
        // node may invalidate the prefix pointer.
        art_key_chunk_t *prefix_copy = (art_key_chunk_t *)roaring_malloc(
            common_prefix * sizeof(art_key_chunk_t));
        memcpy(prefix_copy, inner_node->prefix,
               common_prefix * sizeof(art_key_chunk_t));
        art_node4_t *node4 = art_node4_create(art, prefix_copy, common_prefix);
        roaring_free(prefix_copy);

        // Deref as a new node was created.
        inner_node = (art_inner_node_t *)art_deref(art, ref);

        // Make the existing internal node a child of the new internal node.
        art_node4_insert(art, node4, ref, inner_node->prefix[common_prefix]);

        // Deref again as a new node was created.
        inner_node = (art_inner_node_t *)art_deref(art, ref);

        // Correct the prefix of the moved internal node, trimming off the
        // chunk inserted into the new internal node.
        inner_node->prefix_size = inner_node->prefix_size - common_prefix - 1;
        if (inner_node->prefix_size > 0) {
            // Move the remaining prefix to the correct position.
            memmove(inner_node->prefix, inner_node->prefix + common_prefix + 1,
                    inner_node->prefix_size);
        }

        // Insert the value in the new internal node.
        return art_node_insert_leaf(art, (art_inner_node_t *)node4,
                                    CROARING_ART_NODE4_TYPE,
                                    key[common_prefix + depth], new_leaf);
    }
    // Prefix matches entirely or node has no prefix. Look for an existing
    // child.
    art_key_chunk_t key_chunk = key[depth + common_prefix];
    art_ref_t child =
        art_find_child(inner_node, art_ref_typecode(ref), key_chunk);
    if (child != CROARING_ART_NULL_REF) {
        art_ref_t new_child =
            art_insert_at(art, child, key, depth + common_prefix + 1, new_leaf);
        if (new_child != child) {
            // Deref again as a new node may have been created.
            inner_node = (art_inner_node_t *)art_deref(art, ref);
            // Node type changed.
            art_replace(inner_node, art_ref_typecode(ref), key_chunk,
                        new_child);
        }
        return ref;
    }
    return art_node_insert_leaf(art, inner_node, art_ref_typecode(ref),
                                key_chunk, new_leaf);
}

// Erase helper struct.
typedef struct art_erase_result_s {
    // The rootmost node where the value was erased, may not be equal to
    // the original node. If no value was removed, this is
    // CROARING_ART_NULL_REF.
    art_ref_t rootmost_node;

    // True if a value was erased.
    bool erased;

    // Value removed, if any.
    art_val_t value_erased;
} art_erase_result_t;

// Searches for the given key starting at `node`, erases it if found.
static art_erase_result_t art_erase_at(art_t *art, art_ref_t ref,
                                       const art_key_chunk_t *key,
                                       uint8_t depth) {
    art_erase_result_t result;
    result.rootmost_node = CROARING_ART_NULL_REF;
    result.erased = false;

    if (art_is_leaf(ref)) {
        art_leaf_t *leaf = (art_leaf_t *)art_deref(art, ref);
        uint8_t common_prefix = art_common_prefix(leaf->key, 0, ART_KEY_BYTES,
                                                  key, 0, ART_KEY_BYTES);
        if (common_prefix != ART_KEY_BYTES) {
            // Leaf key mismatch.
            return result;
        }
        result.erased = true;
        result.value_erased = leaf->val;
        art_node_free(art, (art_node_t *)leaf, CROARING_ART_LEAF_TYPE);
        return result;
    }
    art_inner_node_t *inner_node = (art_inner_node_t *)art_deref(art, ref);
    uint8_t common_prefix =
        art_common_prefix(inner_node->prefix, 0, inner_node->prefix_size, key,
                          depth, ART_KEY_BYTES);
    if (common_prefix != inner_node->prefix_size) {
        // Prefix mismatch.
        return result;
    }
    art_key_chunk_t key_chunk = key[depth + common_prefix];
    art_ref_t child =
        art_find_child(inner_node, art_ref_typecode(ref), key_chunk);
    if (child == CROARING_ART_NULL_REF) {
        // No child with key chunk.
        return result;
    }
    // Try to erase the key further down. Skip the key chunk associated with
    // the child in the node.
    art_erase_result_t child_result =
        art_erase_at(art, child, key, depth + common_prefix + 1);
    if (!child_result.erased) {
        return result;
    }
    result.erased = true;
    result.value_erased = child_result.value_erased;
    result.rootmost_node = ref;

    // Deref again as nodes may have changed location.
    inner_node = (art_inner_node_t *)art_deref(art, ref);
    if (child_result.rootmost_node == CROARING_ART_NULL_REF) {
        // Child node was fully erased, erase it from this node's children.
        result.rootmost_node =
            art_node_erase(art, inner_node, art_ref_typecode(ref), key_chunk);
    } else if (child_result.rootmost_node != child) {
        // Child node was not fully erased, update the pointer to it in this
        // node.
        art_replace(inner_node, art_ref_typecode(ref), key_chunk,
                    child_result.rootmost_node);
    }
    return result;
}

// Searches for the given key starting at `node`, returns NULL if the key
// was not found.
static art_val_t *art_find_at(const art_t *art, art_ref_t ref,
                              const art_key_chunk_t *key, uint8_t depth) {
    while (!art_is_leaf(ref)) {
        art_inner_node_t *inner_node = (art_inner_node_t *)art_deref(art, ref);
        uint8_t common_prefix =
            art_common_prefix(inner_node->prefix, 0, inner_node->prefix_size,
                              key, depth, ART_KEY_BYTES);
        if (common_prefix != inner_node->prefix_size) {
            return NULL;
        }
        art_ref_t child = art_find_child(inner_node, art_ref_typecode(ref),
                                         key[depth + inner_node->prefix_size]);
        if (child == CROARING_ART_NULL_REF) {
            return NULL;
        }
        ref = child;
        // Include both the prefix and the child key chunk in the depth.
        depth += inner_node->prefix_size + 1;
    }
    art_leaf_t *leaf = (art_leaf_t *)art_deref(art, ref);
    if (depth >= ART_KEY_BYTES) {
        return &leaf->val;
    }
    uint8_t common_prefix =
        art_common_prefix(leaf->key, 0, ART_KEY_BYTES, key, 0, ART_KEY_BYTES);
    if (common_prefix == ART_KEY_BYTES) {
        return &leaf->val;
    }
    return NULL;
}

static void art_node_print_type(art_ref_t ref) {
    switch (art_ref_typecode(ref)) {
        case CROARING_ART_LEAF_TYPE:
            printf("Leaf");
            return;
        case CROARING_ART_NODE4_TYPE:
            printf("Node4");
            return;
        case CROARING_ART_NODE16_TYPE:
            printf("Node16");
            return;
        case CROARING_ART_NODE48_TYPE:
            printf("Node48");
            return;
        case CROARING_ART_NODE256_TYPE:
            printf("Node256");
            return;
        default:
            assert(false);
            return;
    }
}

static void art_node_printf(const art_t *art, art_ref_t ref, uint8_t depth) {
    if (art_is_leaf(ref)) {
        printf("{ type: Leaf, key: ");
        art_leaf_t *leaf = (art_leaf_t *)art_deref(art, ref);
        for (size_t i = 0; i < ART_KEY_BYTES; ++i) {
            printf("%02x", leaf->key[i]);
        }
        printf(" }\n");
        return;
    }
    printf("{\n");
    depth++;

    printf("%*s", depth, "");
    printf("type: ");
    art_node_print_type(ref);
    printf("\n");

    art_inner_node_t *inner_node = (art_inner_node_t *)art_deref(art, ref);
    printf("%*s", depth, "");
    printf("prefix_size: %d\n", inner_node->prefix_size);

    printf("%*s", depth, "");
    printf("prefix: ");
    for (uint8_t i = 0; i < inner_node->prefix_size; ++i) {
        printf("%02x", inner_node->prefix[i]);
    }
    printf("\n");

    switch (art_ref_typecode(ref)) {
        case CROARING_ART_NODE4_TYPE: {
            art_node4_t *node4 = (art_node4_t *)inner_node;
            for (uint8_t i = 0; i < node4->count; ++i) {
                printf("%*s", depth, "");
                printf("key: %02x ", node4->keys[i]);
                art_node_printf(art, node4->children[i], depth);
            }
        } break;
        case CROARING_ART_NODE16_TYPE: {
            art_node16_t *node16 = (art_node16_t *)inner_node;
            for (uint8_t i = 0; i < node16->count; ++i) {
                printf("%*s", depth, "");
                printf("key: %02x ", node16->keys[i]);
                art_node_printf(art, node16->children[i], depth);
            }
        } break;
        case CROARING_ART_NODE48_TYPE: {
            art_node48_t *node48 = (art_node48_t *)inner_node;
            for (uint16_t i = 0; i < 256; ++i) {
                if (node48->keys[i] != CROARING_ART_NODE48_EMPTY_VAL) {
                    printf("%*s", depth, "");
                    printf("key: %02x ", i);
                    printf("child: %02x ", node48->keys[i]);
                    art_node_printf(art, node48->children[node48->keys[i]],
                                    depth);
                }
            }
        } break;
        case CROARING_ART_NODE256_TYPE: {
            art_node256_t *node256 = (art_node256_t *)inner_node;
            for (uint16_t i = 0; i < 256; ++i) {
                if (node256->children[i] != CROARING_ART_NULL_REF) {
                    printf("%*s", depth, "");
                    printf("key: %02x ", i);
                    art_node_printf(art, node256->children[i], depth);
                }
            }
        } break;
        default:
            assert(false);
            break;
    }
    depth--;
    printf("%*s", depth, "");
    printf("}\n");
}

/**
 * Moves the node at `ref` to the earliest free index before it (if any),
 * returns the new ref. Assumes `art->first_free[typecode]` points to the
 * smallest free index.
 */
static art_ref_t art_move_node_to_shrink(art_t *art, art_ref_t ref) {
    uint64_t idx = art_ref_index(ref);
    art_typecode_t typecode = art_ref_typecode(ref);
    uint64_t first_free = art->first_free[typecode];
    assert(idx != first_free);
    if (idx < first_free) {
        return ref;
    }
    uint64_t from = idx;
    uint64_t to = first_free;
    uint64_t next_free = art_node_get_next_free(art, art_to_ref(to, typecode));
    memcpy(art_get_node(art, to, typecode), art_get_node(art, from, typecode),
           ART_NODE_SIZES[typecode]);

    // With an integer representing the next free index, and an `x` representing
    // an occupied index, assume the following scenario at the start of this
    // function:
    //     nodes = [1,2,5,x,x]
    //     first_free = 0
    //
    // We just moved a node from index 3 to 0:
    //     nodes = [x,2,5,?,x]
    //
    // We need to modify the free list so that the free indices are ascending.
    // This can be done by traversing the list until we find a node with a
    // `next_free` greater than the index we copied the node from, and inserting
    // the new index in between. This leads to the following:
    //     nodes = [x,2,3,5,x]
    //     first_free = 1
    uint64_t initial_next_free = next_free;
    uint64_t current = next_free;
    while (next_free < from) {
        current = next_free;
        next_free =
            art_node_get_next_free(art, art_to_ref(next_free, typecode));
    }
    art_node_set_next_free(art_deref(art, ref), typecode, next_free);
    if (current < from) {
        art_node_set_next_free(art_get_node(art, current, typecode), typecode,
                               from);
    }
    art->first_free[typecode] =
        from < initial_next_free ? from : initial_next_free;
    return art_to_ref(to, typecode);
}

/**
 * Sorts the free lists pointed to by art->first_free in ascending index order.
 */
static void art_sort_free_lists(art_t *art) {
    for (art_typecode_t type = CROARING_ART_LEAF_TYPE;
         type <= CROARING_ART_NODE256_TYPE; ++type) {
        bool *free_indices =
            (bool *)roaring_calloc(art->capacities[type], sizeof(bool));

        for (uint64_t i = art->first_free[type]; i < art->capacities[type];
             i = art_node_get_next_free(art, art_to_ref(i, type))) {
            free_indices[i] = true;
        }

        uint64_t first_free = art->capacities[type];
        for (uint64_t i = art->capacities[type]; i > 0; --i) {
            uint64_t index = i - 1;
            if (free_indices[index]) {
                art_node_set_next_free(art_get_node(art, index, type), type,
                                       first_free);
                first_free = index;
            }
        }
        art->first_free[type] = first_free;
        roaring_free(free_indices);
    }
}

/**
 * Shrinks all node arrays to `first_free`. Assumes all indices after
 * `first_free` are unused.
 */
static size_t art_shrink_node_arrays(art_t *art) {
    size_t freed = 0;
    for (art_typecode_t t = CROARING_ART_MIN_TYPE; t <= CROARING_ART_MAX_TYPE;
         ++t) {
        if (art->first_free[t] < art->capacities[t]) {
            uint64_t new_capacity = art->first_free[t];
            art->nodes[t] = roaring_realloc(art->nodes[t],
                                            new_capacity * ART_NODE_SIZES[t]);
            freed += (art->capacities[t] - new_capacity) * ART_NODE_SIZES[t];
            art->capacities[t] = new_capacity;
        }
    }
    return freed;
}

/**
 * Traverses the ART, moving nodes to earlier free indices and modifying their
 * references along the way.
 */
static void art_shrink_at(art_t *art, art_ref_t ref) {
    if (art_is_leaf(ref)) {
        return;
    }
    switch (art_ref_typecode(ref)) {
        case CROARING_ART_NODE4_TYPE: {
            art_node4_t *node4 = (art_node4_t *)art_deref(art, ref);
            for (uint8_t i = 0; i < node4->count; ++i) {
                node4->children[i] =
                    art_move_node_to_shrink(art, node4->children[i]);
                art_shrink_at(art, node4->children[i]);
            }
        } break;
        case CROARING_ART_NODE16_TYPE: {
            art_node16_t *node16 = (art_node16_t *)art_deref(art, ref);
            for (uint8_t i = 0; i < node16->count; ++i) {
                node16->children[i] =
                    art_move_node_to_shrink(art, node16->children[i]);
                art_shrink_at(art, node16->children[i]);
            }
        } break;
        case CROARING_ART_NODE48_TYPE: {
            art_node48_t *node48 = (art_node48_t *)art_deref(art, ref);
            for (int i = 0; i < 256; ++i) {
                if (node48->keys[i] != CROARING_ART_NODE48_EMPTY_VAL) {
                    uint8_t idx = node48->keys[i];
                    node48->children[idx] =
                        art_move_node_to_shrink(art, node48->children[idx]);
                    art_shrink_at(art, node48->children[idx]);
                }
            }
        } break;
        case CROARING_ART_NODE256_TYPE: {
            art_node256_t *node256 = (art_node256_t *)art_deref(art, ref);
            for (int i = 0; i < 256; ++i) {
                if (node256->children[i] != CROARING_ART_NULL_REF) {
                    node256->children[i] =
                        art_move_node_to_shrink(art, node256->children[i]);
                    art_shrink_at(art, node256->children[i]);
                }
            }
        } break;
        default:
            assert(false);
            break;
    }
}

void art_init_cleared(art_t *art) {
    art->root = CROARING_ART_NULL_REF;
    memset(art->first_free, 0, sizeof(art->first_free));
    memset(art->capacities, 0, sizeof(art->capacities));
    for (art_typecode_t t = CROARING_ART_MIN_TYPE; t <= CROARING_ART_MAX_TYPE;
         ++t) {
        art->nodes[t] = NULL;
    }
}

size_t art_shrink_to_fit(art_t *art) {
    if (art_is_shrunken(art)) {
        return 0;
    }
    if (art->root != CROARING_ART_NULL_REF) {
        art_sort_free_lists(art);
        art->root = art_move_node_to_shrink(art, art->root);
        art_shrink_at(art, art->root);
    }
    return art_shrink_node_arrays(art);
}

bool art_is_shrunken(const art_t *art) {
    for (art_typecode_t t = CROARING_ART_MIN_TYPE; t <= CROARING_ART_MAX_TYPE;
         ++t) {
        if (art->first_free[t] != art->capacities[t]) {
            return false;
        }
    }
    return true;
}

art_val_t *art_insert(art_t *art, const art_key_chunk_t *key, art_val_t val) {
    art_ref_t leaf = art_leaf_create(art, key, val);
    if (art->root == CROARING_ART_NULL_REF) {
        art->root = leaf;
        return &((art_leaf_t *)art_deref(art, leaf))->val;
    }
    art->root = art_insert_at(art, art->root, key, 0, leaf);
    return &((art_leaf_t *)art_deref(art, leaf))->val;
}

bool art_erase(art_t *art, const art_key_chunk_t *key, art_val_t *erased_val) {
    art_val_t erased_val_local;
    if (erased_val == NULL) {
        erased_val = &erased_val_local;
    }
    if (art->root == CROARING_ART_NULL_REF) {
        return false;
    }
    art_erase_result_t result = art_erase_at(art, art->root, key, 0);
    if (!result.erased) {
        return false;
    }
    art->root = result.rootmost_node;
    *erased_val = result.value_erased;
    return true;
}

art_val_t *art_find(const art_t *art, const art_key_chunk_t *key) {
    if (art->root == CROARING_ART_NULL_REF) {
        return NULL;
    }
    return art_find_at(art, art->root, key, 0);
}

bool art_is_empty(const art_t *art) {
    return art->root == CROARING_ART_NULL_REF;
}

void art_free(art_t *art) {
    for (art_typecode_t t = CROARING_ART_MIN_TYPE; t <= CROARING_ART_MAX_TYPE;
         ++t) {
        roaring_free(art->nodes[t]);
    }
}

void art_printf(const art_t *art) {
    if (art->root == CROARING_ART_NULL_REF) {
        return;
    }
    art_node_printf(art, art->root, 0);
}

// Returns a reference to the current node that the iterator is positioned
// at.
static inline art_ref_t art_iterator_ref(art_iterator_t *iterator) {
    return iterator->frames[iterator->frame].ref;
}

// Returns the current node that the iterator is positioned at.
static inline art_node_t *art_iterator_node(art_iterator_t *iterator) {
    return art_deref(iterator->art, art_iterator_ref(iterator));
}

// Sets the iterator key and value to the leaf's key and value. Always
// returns true for convenience.
static inline bool art_iterator_valid_loc(art_iterator_t *iterator,
                                          art_ref_t leaf_ref) {
    iterator->frames[iterator->frame].ref = leaf_ref;
    iterator->frames[iterator->frame].index_in_node = 0;
    art_leaf_t *leaf = (art_leaf_t *)art_deref(iterator->art, leaf_ref);
    memcpy(iterator->key, leaf->key, ART_KEY_BYTES);
    iterator->value = &leaf->val;
    return true;
}

// Invalidates the iterator key and value. Always returns false for
// convenience.
static inline bool art_iterator_invalid_loc(art_iterator_t *iterator) {
    memset(iterator->key, 0, ART_KEY_BYTES);
    iterator->value = NULL;
    return false;
}

// Moves the iterator one level down in the tree, given a node at the
// current level and the index of the child that we're going down to.
//
// Note: does not set the index at the new level.
static void art_iterator_down(art_iterator_t *iterator, art_ref_t ref,
                              uint8_t index_in_node) {
    iterator->frames[iterator->frame].ref = ref;
    iterator->frames[iterator->frame].index_in_node = index_in_node;
    iterator->frame++;
    art_inner_node_t *node = (art_inner_node_t *)art_deref(iterator->art, ref);
    art_indexed_child_t indexed_child = art_node_child_at(
        (art_node_t *)node, art_ref_typecode(ref), index_in_node);
    assert(indexed_child.child != CROARING_ART_NULL_REF);
    iterator->frames[iterator->frame].ref = indexed_child.child;
    iterator->depth += node->prefix_size + 1;
}

// Moves the iterator to the next/previous child of the current node.
// Returns the child moved to, or NULL if there is no neighboring child.
static art_ref_t art_iterator_neighbor_child(art_iterator_t *iterator,
                                             bool forward) {
    art_iterator_frame_t frame = iterator->frames[iterator->frame];
    art_node_t *node = art_deref(iterator->art, frame.ref);
    art_indexed_child_t indexed_child;
    if (forward) {
        indexed_child = art_node_next_child(node, art_ref_typecode(frame.ref),
                                            frame.index_in_node);
    } else {
        indexed_child = art_node_prev_child(node, art_ref_typecode(frame.ref),
                                            frame.index_in_node);
    }
    if (indexed_child.child != CROARING_ART_NULL_REF) {
        art_iterator_down(iterator, frame.ref, indexed_child.index);
    }
    return indexed_child.child;
}

// Moves the iterator one level up in the tree, returns false if not
// possible.
static bool art_iterator_up(art_iterator_t *iterator) {
    if (iterator->frame == 0) {
        return false;
    }
    iterator->frame--;
    // We went up, so we are at an inner node.
    iterator->depth -=
        ((art_inner_node_t *)art_iterator_node(iterator))->prefix_size + 1;
    return true;
}

// Moves the iterator one level, followed by a move to the next / previous
// leaf. Sets the status of the iterator.
static bool art_iterator_up_and_move(art_iterator_t *iterator, bool forward) {
    if (!art_iterator_up(iterator)) {
        // We're at the root.
        return art_iterator_invalid_loc(iterator);
    }
    return art_iterator_move(iterator, forward);
}

// Initializes the iterator at the first / last leaf of the given node.
// Returns true for convenience.
static bool art_node_init_iterator(art_ref_t ref, art_iterator_t *iterator,
                                   bool first) {
    while (!art_is_leaf(ref)) {
        art_node_t *node = art_deref(iterator->art, ref);
        art_indexed_child_t indexed_child;
        if (first) {
            indexed_child =
                art_node_next_child(node, art_ref_typecode(ref), -1);
        } else {
            indexed_child =
                art_node_prev_child(node, art_ref_typecode(ref), 256);
        }
        art_iterator_down(iterator, ref, indexed_child.index);
        ref = indexed_child.child;
    }
    // We're at a leaf.
    iterator->frames[iterator->frame].ref = ref;
    iterator->frames[iterator->frame].index_in_node = 0;  // Should not matter.
    return art_iterator_valid_loc(iterator, ref);
}

bool art_iterator_move(art_iterator_t *iterator, bool forward) {
    if (art_is_leaf(art_iterator_ref(iterator))) {
        bool went_up = art_iterator_up(iterator);
        if (!went_up) {
            // This leaf is the root, we're done.
            return art_iterator_invalid_loc(iterator);
        }
    }
    // Advance within inner node.
    art_ref_t neighbor_child = art_iterator_neighbor_child(iterator, forward);
    if (neighbor_child != CROARING_ART_NULL_REF) {
        // There is another child at this level, go down to the first or
        // last leaf.
        return art_node_init_iterator(neighbor_child, iterator, forward);
    }
    // No more children at this level, go up.
    return art_iterator_up_and_move(iterator, forward);
}

// Assumes the iterator is positioned at a node with an equal prefix path up
// to the depth of the iterator.
static bool art_node_iterator_lower_bound(art_ref_t ref,
                                          art_iterator_t *iterator,
                                          const art_key_chunk_t key[]) {
    while (!art_is_leaf(ref)) {
        art_inner_node_t *inner_node =
            (art_inner_node_t *)art_deref(iterator->art, ref);
        int prefix_comparison =
            art_compare_prefix(inner_node->prefix, 0, key, iterator->depth,
                               inner_node->prefix_size);
        if (prefix_comparison < 0) {
            // Prefix so far has been equal, but we've found a smaller key.
            // Since we take the lower bound within each node, we can return
            // the next leaf.
            return art_iterator_up_and_move(iterator, true);
        } else if (prefix_comparison > 0) {
            // No key equal to the key we're looking for, return the first
            // leaf.
            return art_node_init_iterator(ref, iterator, true);
        }
        // Prefix is equal, move to lower bound child.
        art_key_chunk_t key_chunk =
            key[iterator->depth + inner_node->prefix_size];
        art_indexed_child_t indexed_child = art_node_lower_bound(
            (art_node_t *)inner_node, art_ref_typecode(ref), key_chunk);
        if (indexed_child.child == CROARING_ART_NULL_REF) {
            // Only smaller keys among children.
            return art_iterator_up_and_move(iterator, true);
        }
        if (indexed_child.key_chunk > key_chunk) {
            // Only larger children, return the first larger child.
            art_iterator_down(iterator, ref, indexed_child.index);
            return art_node_init_iterator(indexed_child.child, iterator, true);
        }
        // We found a child with an equal prefix.
        art_iterator_down(iterator, ref, indexed_child.index);
        ref = indexed_child.child;
    }
    art_leaf_t *leaf = (art_leaf_t *)art_deref(iterator->art, ref);
    if (art_compare_keys(leaf->key, key) >= 0) {
        // Leaf has an equal or larger key.
        return art_iterator_valid_loc(iterator, ref);
    }
    // Leaf has an equal prefix, but the full key is smaller. Move to the
    // next leaf.
    return art_iterator_up_and_move(iterator, true);
}

art_iterator_t art_init_iterator(art_t *art, bool first) {
    art_iterator_t iterator = CROARING_ZERO_INITIALIZER;
    iterator.art = art;
    if (art->root == CROARING_ART_NULL_REF) {
        return iterator;
    }
    art_node_init_iterator(art->root, &iterator, first);
    return iterator;
}

bool art_iterator_next(art_iterator_t *iterator) {
    return art_iterator_move(iterator, true);
}

bool art_iterator_prev(art_iterator_t *iterator) {
    return art_iterator_move(iterator, false);
}

bool art_iterator_lower_bound(art_iterator_t *iterator,
                              const art_key_chunk_t *key) {
    if (iterator->value == NULL) {
        // We're beyond the end / start of the ART so the iterator does not
        // have a valid key. Start from the root.
        iterator->frame = 0;
        iterator->depth = 0;
        art_ref_t root = art_iterator_ref(iterator);
        if (root == CROARING_ART_NULL_REF) {
            return false;
        }
        return art_node_iterator_lower_bound(root, iterator, key);
    }
    int compare_result =
        art_compare_prefix(iterator->key, 0, key, 0, ART_KEY_BYTES);
    // Move up until we have an equal prefix, after which we can do a normal
    // lower bound search.
    while (compare_result != 0) {
        if (!art_iterator_up(iterator)) {
            if (compare_result < 0) {
                // Only smaller keys found.
                return art_iterator_invalid_loc(iterator);
            } else {
                return art_node_init_iterator(art_iterator_ref(iterator),
                                              iterator, true);
            }
        }
        // Since we're only moving up, we can keep comparing against the
        // iterator key.
        art_inner_node_t *inner_node =
            (art_inner_node_t *)art_iterator_node(iterator);
        compare_result =
            art_compare_prefix(iterator->key, 0, key, 0,
                               iterator->depth + inner_node->prefix_size);
    }
    if (compare_result > 0) {
        return art_node_init_iterator(art_iterator_ref(iterator), iterator,
                                      true);
    }
    return art_node_iterator_lower_bound(art_iterator_ref(iterator), iterator,
                                         key);
}

art_iterator_t art_lower_bound(art_t *art, const art_key_chunk_t *key) {
    art_iterator_t iterator = CROARING_ZERO_INITIALIZER;
    iterator.art = art;
    if (art->root != CROARING_ART_NULL_REF) {
        art_node_iterator_lower_bound(art->root, &iterator, key);
    }
    return iterator;
}

art_iterator_t art_upper_bound(art_t *art, const art_key_chunk_t *key) {
    art_iterator_t iterator = CROARING_ZERO_INITIALIZER;
    iterator.art = art;
    if (art->root != CROARING_ART_NULL_REF) {
        if (art_node_iterator_lower_bound(art->root, &iterator, key) &&
            art_compare_keys(iterator.key, key) == 0) {
            art_iterator_next(&iterator);
        }
    }
    return iterator;
}

void art_iterator_insert(art_iterator_t *iterator, const art_key_chunk_t *key,
                         art_val_t val) {
    // TODO: This can likely be faster.
    art_insert(iterator->art, key, val);
    assert(iterator->art->root != CROARING_ART_NULL_REF);
    iterator->frame = 0;
    iterator->depth = 0;
    art_node_iterator_lower_bound(iterator->art->root, iterator, key);
}

bool art_iterator_erase(art_iterator_t *iterator, art_val_t *erased_val) {
    art_val_t erased_val_local;
    if (erased_val == NULL) {
        erased_val = &erased_val_local;
    }
    if (iterator->value == NULL) {
        return false;
    }
    art_key_chunk_t initial_key[ART_KEY_BYTES];
    memcpy(initial_key, iterator->key, ART_KEY_BYTES);

    *erased_val = *iterator->value;
    // Erase the leaf.
    art_node_free(iterator->art, art_iterator_node(iterator),
                  art_ref_typecode(art_iterator_ref(iterator)));
    bool went_up = art_iterator_up(iterator);
    if (!went_up) {
        // We're erasing the root.
        iterator->art->root = CROARING_ART_NULL_REF;
        art_iterator_invalid_loc(iterator);
        return true;
    }

    // Erase the leaf in its parent.
    art_ref_t parent_ref = art_iterator_ref(iterator);
    art_inner_node_t *parent_node =
        (art_inner_node_t *)art_iterator_node(iterator);
    art_key_chunk_t key_chunk_in_parent =
        iterator->key[iterator->depth + parent_node->prefix_size];
    art_ref_t new_parent_ref =
        art_node_erase(iterator->art, parent_node, art_ref_typecode(parent_ref),
                       key_chunk_in_parent);

    if (new_parent_ref != parent_ref) {
        // Replace the pointer to the inner node we erased from in its
        // parent (it may be a leaf now).
        iterator->frames[iterator->frame].ref = new_parent_ref;
        went_up = art_iterator_up(iterator);
        if (went_up) {
            art_ref_t grandparent_ref = art_iterator_ref(iterator);
            art_inner_node_t *grandparent_node =
                (art_inner_node_t *)art_iterator_node(iterator);
            art_key_chunk_t key_chunk_in_grandparent =
                iterator->key[iterator->depth + grandparent_node->prefix_size];
            art_replace(grandparent_node, art_ref_typecode(grandparent_ref),
                        key_chunk_in_grandparent, new_parent_ref);
        } else {
            // We were already at the rootmost node.
            iterator->art->root = new_parent_ref;
        }
    }

    iterator->frame = 0;
    iterator->depth = 0;
    // Do a lower bound search for the initial key, which will find the
    // first greater key if it exists. This can likely be mildly faster if
    // we instead start from the current position.
    art_node_iterator_lower_bound(iterator->art->root, iterator, initial_key);
    return true;
}

static bool art_internal_validate_at(const art_t *art, art_ref_t ref,
                                     art_internal_validate_t validator) {
    if (ref == CROARING_ART_NULL_REF) {
        return art_validate_fail(&validator, "node is null");
    }
    if (art_is_leaf(ref)) {
        art_leaf_t *leaf = (art_leaf_t *)art_deref(art, ref);
        if (art_compare_prefix(leaf->key, 0, validator.current_key, 0,
                               validator.depth) != 0) {
            return art_validate_fail(&validator,
                                     "leaf key does not match its "
                                     "position's prefix in the tree");
        }
        if (validator.validate_cb != NULL &&
            !validator.validate_cb(leaf->val, validator.reason,
                                   validator.context)) {
            if (*validator.reason == NULL) {
                *validator.reason = "leaf validation failed";
            }
            return false;
        }
    } else {
        art_inner_node_t *inner_node = (art_inner_node_t *)art_deref(art, ref);

        if (validator.depth + inner_node->prefix_size + 1 > ART_KEY_BYTES) {
            return art_validate_fail(&validator,
                                     "node has too much prefix at given depth");
        }
        memcpy(validator.current_key + validator.depth, inner_node->prefix,
               inner_node->prefix_size);
        validator.depth += inner_node->prefix_size;

        switch (art_ref_typecode(ref)) {
            case CROARING_ART_NODE4_TYPE:
                if (!art_node4_internal_validate(art, (art_node4_t *)inner_node,
                                                 validator)) {
                    return false;
                }
                break;
            case CROARING_ART_NODE16_TYPE:
                if (!art_node16_internal_validate(
                        art, (art_node16_t *)inner_node, validator)) {
                    return false;
                }
                break;
            case CROARING_ART_NODE48_TYPE:
                if (!art_node48_internal_validate(
                        art, (art_node48_t *)inner_node, validator)) {
                    return false;
                }
                break;
            case CROARING_ART_NODE256_TYPE:
                if (!art_node256_internal_validate(
                        art, (art_node256_t *)inner_node, validator)) {
                    return false;
                }
                break;
            default:
                return art_validate_fail(&validator, "invalid node type");
        }
    }
    return true;
}

bool art_internal_validate(const art_t *art, const char **reason,
                           art_validate_cb_t validate_cb, void *context) {
    const char *reason_local;
    if (reason == NULL) {
        // Always allow assigning through *reason
        reason = &reason_local;
    }
    *reason = NULL;
    if (art->root == CROARING_ART_NULL_REF) {
        return true;
    }
    art_internal_validate_t validator = {
        .reason = reason,
        .validate_cb = validate_cb,
        .context = context,
        .depth = 0,
        .current_key = CROARING_ZERO_INITIALIZER,
    };
    for (art_typecode_t type = CROARING_ART_LEAF_TYPE;
         type <= CROARING_ART_NODE256_TYPE; ++type) {
        uint64_t capacity = art->capacities[type];
        for (uint64_t i = 0; i < capacity; ++i) {
            uint64_t first_free = art->first_free[type];
            if (first_free > capacity) {
                return art_validate_fail(&validator, "first_free > capacity");
            }
        }
    }
    return art_internal_validate_at(art, art->root, validator);
}

CROARING_STATIC_ASSERT(alignof(art_leaf_t) == alignof(art_node4_t),
                       "Serialization assumes node type alignment is equal");
CROARING_STATIC_ASSERT(alignof(art_leaf_t) == alignof(art_node16_t),
                       "Serialization assumes node type alignment is equal");
CROARING_STATIC_ASSERT(alignof(art_leaf_t) == alignof(art_node48_t),
                       "Serialization assumes node type alignment is equal");
CROARING_STATIC_ASSERT(alignof(art_leaf_t) == alignof(art_node256_t),
                       "Serialization assumes node type alignment is equal");

size_t art_size_in_bytes(const art_t *art) {
    if (!art_is_shrunken(art)) {
        return 0;
    }
    // Root.
    size_t size = sizeof(art->root);
    // Node counts.
    size += sizeof(art->capacities);
    // Alignment for leaves. The rest of the nodes are aligned the same way.
    size +=
        ((size + alignof(art_leaf_t) - 1) & ~(alignof(art_leaf_t) - 1)) - size;
    for (art_typecode_t t = CROARING_ART_MIN_TYPE; t <= CROARING_ART_MAX_TYPE;
         ++t) {
        size += art->capacities[t] * ART_NODE_SIZES[t];
    }
    return size;
}

size_t art_serialize(const art_t *art, char *buf) {
    if (buf == NULL) {
        return 0;
    }
    if (!art_is_shrunken(art)) {
        return 0;
    }
    const char *initial_buf = buf;

    // Root.
    memcpy(buf, &art->root, sizeof(art->root));
    buf += sizeof(art->root);

    // Node counts.
    memcpy(buf, art->capacities, sizeof(art->capacities));
    buf += sizeof(art->capacities);

    // Alignment for leaves. The rest of the nodes are aligned the same way.
    size_t align_bytes =
        CROARING_ART_ALIGN_SIZE_RELATIVE(buf, initial_buf, alignof(art_leaf_t));
    memset(buf, 0, align_bytes);
    buf += align_bytes;

    for (art_typecode_t t = CROARING_ART_MIN_TYPE; t <= CROARING_ART_MAX_TYPE;
         ++t) {
        if (art->capacities[t] > 0) {
            size_t size = art->capacities[t] * ART_NODE_SIZES[t];
            memcpy(buf, art->nodes[t], size);
            buf += size;
        }
    }

    return buf - initial_buf;
}

size_t art_frozen_view(const char *buf, size_t maxbytes, art_t *art) {
    if (buf == NULL || art == NULL) {
        return 0;
    }
    const char *initial_buf = buf;
    art_init_cleared(art);

    if (maxbytes < sizeof(art->root)) {
        return 0;
    }
    memcpy(&art->root, buf, sizeof(art->root));
    buf += sizeof(art->root);
    maxbytes -= sizeof(art->root);

    if (maxbytes < sizeof(art->capacities)) {
        return 0;
    }
    CROARING_STATIC_ASSERT(sizeof(art->first_free) == sizeof(art->capacities),
                           "first_free is read from capacities");
    memcpy(art->first_free, buf, sizeof(art->capacities));
    memcpy(art->capacities, buf, sizeof(art->capacities));
    buf += sizeof(art->capacities);
    maxbytes -= sizeof(art->capacities);

    // Alignment for leaves. The rest of the nodes are aligned the same way.
    const char *before_align = buf;
    buf = CROARING_ART_ALIGN_BUF(buf, alignof(art_leaf_t));
    if (maxbytes < (size_t)(buf - before_align)) {
        return 0;
    }
    maxbytes -= buf - before_align;

    for (art_typecode_t t = CROARING_ART_MIN_TYPE; t <= CROARING_ART_MAX_TYPE;
         ++t) {
        if (art->capacities[t] > 0) {
            size_t size = art->capacities[t] * ART_NODE_SIZES[t];
            if (maxbytes < size) {
                return 0;
            }
            art->nodes[t] = (char *)buf;
            buf += size;
            maxbytes -= size;
        }
    }
    return buf - initial_buf;
}

#ifdef __cplusplus
}  // extern "C"
}  // namespace roaring
}  // namespace internal
#endif
