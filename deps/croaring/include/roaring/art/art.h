#ifndef ART_ART_H
#define ART_ART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * This file contains an implementation of an Adaptive Radix Tree as described
 * in https://db.in.tum.de/~leis/papers/ART.pdf.
 *
 * The ART contains the keys in _byte lexographical_ order.
 *
 * Other features:
 *  * Fixed 48 bit key length: all keys are assumed to be be 48 bits in size.
 *    This allows us to put the key and key prefixes directly in nodes, reducing
 *    indirection at no additional memory overhead.
 *  * Key compression: the only inner nodes created are at points where key
 *    chunks _differ_. This means that if there are two entries with different
 *    high 48 bits, then there is only one inner node containing the common key
 *    prefix, and two leaves.
 *  * Mostly pointer-free: nodes are referred to by index rather than pointer,
 *    so that the structure can be deserialized with a backing buffer.
 */

// Fixed length of keys in the ART. All keys are assumed to be of this length.
#define ART_KEY_BYTES 6

#ifdef __cplusplus
extern "C" {
namespace roaring {
namespace internal {
#endif

typedef uint8_t art_key_chunk_t;

// Internal node reference type. Contains the node typecode in the low 8 bits,
// and the index in the relevant node array in the high 48 bits. Has a value of
// CROARING_ART_NULL_REF when pointing to a non-existent node.
typedef uint64_t art_ref_t;

typedef void art_node_t;

/**
 * The ART is empty when root is a null ref.
 *
 * Each node type has its own dynamic array of node structs, indexed by
 * art_ref_t. The arrays are expanded as needed, and shrink only when
 * `shrink_to_fit` is called.
 */
typedef struct art_s {
    art_ref_t root;

    // Indexed by node typecode, thus 1 larger than they need to be for
    // convenience. `first_free` indicates the index where the first free node
    // lives, which may be equal to the capacity.
    uint64_t first_free[6];
    uint64_t capacities[6];
    art_node_t *nodes[6];
} art_t;

typedef uint64_t art_val_t;

/**
 * Compares two keys, returns their relative order:
 *  * Key 1 <  key 2: returns a negative value
 *  * Key 1 == key 2: returns 0
 *  * Key 1 >  key 2: returns a positive value
 */
int art_compare_keys(const art_key_chunk_t key1[],
                     const art_key_chunk_t key2[]);

/**
 * Initializes the ART.
 */
void art_init_cleared(art_t *art);

/**
 * Inserts the given key and value. Returns a pointer to the value inserted,
 * valid as long as the ART is not modified.
 */
art_val_t *art_insert(art_t *art, const art_key_chunk_t *key, art_val_t val);

/**
 * Returns true if a value was erased. Sets `*erased_val` to the value erased,
 * if any.
 */
bool art_erase(art_t *art, const art_key_chunk_t *key, art_val_t *erased_val);

/**
 * Returns the value associated with the given key, NULL if not found.
 */
art_val_t *art_find(const art_t *art, const art_key_chunk_t *key);

/**
 * Returns true if the ART is empty.
 */
bool art_is_empty(const art_t *art);

/**
 * Frees the contents of the ART. Should not be called when using
 * `art_deserialize_frozen_safe`.
 */
void art_free(art_t *art);

/**
 * Prints the ART using printf, useful for debugging.
 */
void art_printf(const art_t *art);

/**
 * Callback for validating the value stored in a leaf. `context` is a
 * user-provided value passed to the callback without modification.
 *
 * Should return true if the value is valid, false otherwise
 * If false is returned, `*reason` should be set to a static string describing
 * the reason for the failure.
 */
typedef bool (*art_validate_cb_t)(const art_val_t val, const char **reason,
                                  void *context);

/**
 * Validate the ART tree, ensuring it is internally consistent. `context` is a
 * user-provided value passed to the callback without modification.
 */
bool art_internal_validate(const art_t *art, const char **reason,
                           art_validate_cb_t validate_cb, void *context);

/**
 * ART-internal iterator bookkeeping. Users should treat this as an opaque type.
 */
typedef struct art_iterator_frame_s {
    art_ref_t ref;
    uint8_t index_in_node;
} art_iterator_frame_t;

/**
 * Users should only access `key` and `value` in iterators. The iterator is
 * valid when `value != NULL`.
 */
typedef struct art_iterator_s {
    art_key_chunk_t key[ART_KEY_BYTES];
    art_val_t *value;

    art_t *art;

    uint8_t depth;  // Key depth
    uint8_t frame;  // Node depth

    // State for each node in the ART the iterator has travelled from the root.
    // This is `ART_KEY_BYTES + 1` because it includes state for the leaf too.
    art_iterator_frame_t frames[ART_KEY_BYTES + 1];
} art_iterator_t;

/**
 * Creates an iterator initialzed to the first or last entry in the ART,
 * depending on `first`. The iterator is not valid if there are no entries in
 * the ART.
 */
art_iterator_t art_init_iterator(art_t *art, bool first);

/**
 * Returns an initialized iterator positioned at a key equal to or greater than
 * the given key, if it exists.
 */
art_iterator_t art_lower_bound(art_t *art, const art_key_chunk_t *key);

/**
 * Returns an initialized iterator positioned at a key greater than the given
 * key, if it exists.
 */
art_iterator_t art_upper_bound(art_t *art, const art_key_chunk_t *key);

/**
 * The following iterator movement functions return true if a new entry was
 * encountered.
 */
bool art_iterator_move(art_iterator_t *iterator, bool forward);
bool art_iterator_next(art_iterator_t *iterator);
bool art_iterator_prev(art_iterator_t *iterator);

/**
 * Moves the iterator forward to a key equal to or greater than the given key.
 */
bool art_iterator_lower_bound(art_iterator_t *iterator,
                              const art_key_chunk_t *key);

/**
 * Insert the value and positions the iterator at the key.
 */
void art_iterator_insert(art_iterator_t *iterator, const art_key_chunk_t *key,
                         art_val_t val);

/**
 * Erase the value pointed at by the iterator. Moves the iterator to the next
 * leaf.
 * Returns true if a value was erased. Sets `*erased_val` to the value erased,
 * if any.
 */
bool art_iterator_erase(art_iterator_t *iterator, art_val_t *erased_val);

/**
 * Shrinks the internal arrays in the ART to remove any unused elements. Returns
 * the number of bytes freed.
 */
size_t art_shrink_to_fit(art_t *art);

/**
 * Returns true if the ART has no unused elements.
 */
bool art_is_shrunken(const art_t *art);

/**
 * Returns the serialized size in bytes.
 * Requires `art_shrink_to_fit` to be called first.
 */
size_t art_size_in_bytes(const art_t *art);

/**
 * Serializes the ART and returns the number of bytes written. Returns 0 on
 * error. Requires `art_shrink_to_fit` to be called first.
 */
size_t art_serialize(const art_t *art, char *buf);

/**
 * Deserializes the ART from a serialized buffer, reading up to `maxbytes`
 * bytes. Returns 0 on error. Requires `buf` to be 8 byte aligned.
 *
 * An ART deserialized in this way should only be used in a readonly context.The
 * underlying buffer must not be freed before the ART. `art_free` should not be
 * called on the ART deserialized in this way.
 */
size_t art_frozen_view(const char *buf, size_t maxbytes, art_t *art);

#ifdef __cplusplus
}  // extern "C"
}  // namespace roaring
}  // namespace internal
#endif

#endif
