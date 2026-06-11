#include <assert.h>
#include <stdalign.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#include <roaring/art/art.h>
#include <roaring/portability.h>
#include <roaring/roaring64.h>

// For serialization / deserialization
#include <roaring/containers/array.h>
#include <roaring/containers/bitset.h>
#include <roaring/containers/run.h>
#include <roaring/roaring.h>
#include <roaring/roaring_array.h>
// containers.h last to avoid conflict with ROARING_CONTAINER_T.
#include <roaring/containers/containers.h>

#define CROARING_ALIGN_BUF(buf, alignment)          \
    (char *)(((uintptr_t)(buf) + ((alignment)-1)) & \
             (ptrdiff_t)(~((alignment)-1)))

#define CROARING_BITSET_ALIGNMENT 64

#ifdef __cplusplus
using namespace ::roaring::internal;

extern "C" {
namespace roaring {
namespace api {
#endif

// TODO: Copy on write.
// TODO: Error on failed allocation.

typedef struct roaring64_bitmap_s {
    art_t art;
    uint8_t flags;
    uint64_t first_free;
    uint64_t capacity;
    container_t **containers;
} roaring64_bitmap_t;

// Leaf type of the ART used to keep the high 48 bits of each entry.
// Low 8 bits: typecode
// High 56 bits: container index
typedef roaring64_leaf_t leaf_t;

// Iterator struct to hold iteration state.
typedef struct roaring64_iterator_s {
    const roaring64_bitmap_t *r;
    art_iterator_t art_it;
    roaring_container_iterator_t container_it;
    uint64_t high48;  // Key that art_it points to.

    uint64_t value;
    bool has_value;

    // If has_value is false, then the iterator is saturated. This field
    // indicates the direction of saturation. If true, there are no more values
    // in the forward direction. If false, there are no more values in the
    // backward direction.
    bool saturated_forward;
} roaring64_iterator_t;

static inline bool is_frozen64(const roaring64_bitmap_t *r) {
    return r->flags & ROARING_FLAG_FROZEN;
}

// Splits the given uint64 key into high 48 bit and low 16 bit components.
// Expects high48_out to be of length ART_KEY_BYTES.
static inline uint16_t split_key(uint64_t key, uint8_t high48_out[]) {
    uint64_t tmp = croaring_htobe64(key);
    memcpy(high48_out, (uint8_t *)(&tmp), ART_KEY_BYTES);
    return (uint16_t)key;
}

// Recombines the high 48 bit and low 16 bit components into a uint64 key.
// Expects high48_out to be of length ART_KEY_BYTES.
static inline uint64_t combine_key(const uint8_t high48[], uint16_t low16) {
    uint64_t result = 0;
    memcpy((uint8_t *)(&result), high48, ART_KEY_BYTES);
    return croaring_be64toh(result) | low16;
}

static inline uint64_t minimum(uint64_t a, uint64_t b) {
    return (a < b) ? a : b;
}

static inline leaf_t create_leaf(uint64_t container_index, uint8_t typecode) {
    return (container_index << 8) | typecode;
}

static inline uint8_t get_typecode(leaf_t leaf) { return (uint8_t)leaf; }

static inline uint64_t get_index(leaf_t leaf) { return leaf >> 8; }

static inline container_t *get_container(const roaring64_bitmap_t *r,
                                         leaf_t leaf) {
    return r->containers[get_index(leaf)];
}

// Replaces the container of `leaf` with the given container. Returns the
// modified leaf for convenience.
static inline leaf_t replace_container(roaring64_bitmap_t *r, leaf_t *leaf,
                                       container_t *container,
                                       uint8_t typecode) {
    uint64_t index = get_index(*leaf);
    r->containers[index] = container;
    *leaf = create_leaf(index, typecode);
    return *leaf;
}

/**
 * Extends the array of container pointers.
 */
static void extend_containers(roaring64_bitmap_t *r) {
    uint64_t size = r->first_free;
    if (size < r->capacity) {
        return;
    }
    uint64_t new_capacity;
    if (r->capacity == 0) {
        new_capacity = 2;
    } else if (r->capacity < 1024) {
        new_capacity = 2 * r->capacity;
    } else {
        new_capacity = 5 * r->capacity / 4;
    }
    uint64_t increase = new_capacity - r->capacity;
    r->containers = (container_t **)roaring_realloc(
        r->containers, new_capacity * sizeof(container_t *));
    memset(r->containers + r->capacity, 0, increase * sizeof(container_t *));
    r->capacity = new_capacity;
}

static uint64_t next_free_container_idx(const roaring64_bitmap_t *r) {
    for (uint64_t i = r->first_free + 1; i < r->capacity; ++i) {
        if (r->containers[i] == NULL) {
            return i;
        }
    }
    return r->capacity;
}

static uint64_t allocate_index(roaring64_bitmap_t *r) {
    uint64_t first_free = r->first_free;
    if (first_free == r->capacity) {
        extend_containers(r);
    }
    r->first_free = next_free_container_idx(r);
    return first_free;
}

static leaf_t add_container(roaring64_bitmap_t *r, container_t *container,
                            uint8_t typecode) {
    uint64_t index = allocate_index(r);
    r->containers[index] = container;
    return create_leaf(index, typecode);
}

static void remove_container(roaring64_bitmap_t *r, leaf_t leaf) {
    uint64_t index = get_index(leaf);
    r->containers[index] = NULL;
    if (index < r->first_free) {
        r->first_free = index;
    }
}

// Copies the container referenced by `leaf` from `r1` to `r2`.
static inline leaf_t copy_leaf_container(const roaring64_bitmap_t *r1,
                                         roaring64_bitmap_t *r2, leaf_t leaf) {
    uint8_t typecode = get_typecode(leaf);
    // get_copy_of_container modifies the typecode passed in.
    container_t *container = get_copy_of_container(
        get_container(r1, leaf), &typecode, /*copy_on_write=*/false);
    return add_container(r2, container, typecode);
}

static inline int compare_high48(art_key_chunk_t key1[],
                                 art_key_chunk_t key2[]) {
    return art_compare_keys(key1, key2);
}

static inline bool roaring64_iterator_init_at_leaf_first(
    roaring64_iterator_t *it) {
    it->high48 = combine_key(it->art_it.key, 0);
    leaf_t leaf = (leaf_t)*it->art_it.value;
    uint16_t low16 = 0;
    it->container_it = container_init_iterator(get_container(it->r, leaf),
                                               get_typecode(leaf), &low16);
    it->value = it->high48 | low16;
    return (it->has_value = true);
}

static inline bool roaring64_iterator_init_at_leaf_last(
    roaring64_iterator_t *it) {
    it->high48 = combine_key(it->art_it.key, 0);
    leaf_t leaf = (leaf_t)*it->art_it.value;
    uint16_t low16 = 0;
    it->container_it = container_init_iterator_last(get_container(it->r, leaf),
                                                    get_typecode(leaf), &low16);
    it->value = it->high48 | low16;
    return (it->has_value = true);
}

static inline roaring64_iterator_t *roaring64_iterator_init_at(
    const roaring64_bitmap_t *r, roaring64_iterator_t *it, bool first) {
    it->r = r;
    it->art_it = art_init_iterator((art_t *)&r->art, first);
    it->has_value = it->art_it.value != NULL;
    if (it->has_value) {
        if (first) {
            roaring64_iterator_init_at_leaf_first(it);
        } else {
            roaring64_iterator_init_at_leaf_last(it);
        }
    } else {
        it->saturated_forward = first;
    }
    return it;
}

roaring64_bitmap_t *roaring64_bitmap_create(void) {
    roaring64_bitmap_t *r =
        (roaring64_bitmap_t *)roaring_malloc(sizeof(roaring64_bitmap_t));
    art_init_cleared(&r->art);
    r->flags = 0;
    r->capacity = 0;
    r->first_free = 0;
    r->containers = NULL;
    return r;
}

void roaring64_bitmap_free(roaring64_bitmap_t *r) {
    if (!r) {
        return;
    }
    art_iterator_t it = art_init_iterator(&r->art, /*first=*/true);
    while (it.value != NULL) {
        leaf_t leaf = (leaf_t)*it.value;
        if (is_frozen64(r)) {
            // Only free the container itself, not the buffer-backed contents
            // within.
            roaring_free(get_container(r, leaf));
        } else {
            container_free(get_container(r, leaf), get_typecode(leaf));
        }
        art_iterator_next(&it);
    }
    if (!is_frozen64(r)) {
        art_free(&r->art);
    }
    roaring_free(r->containers);
    roaring_free(r);
}

roaring64_bitmap_t *roaring64_bitmap_copy(const roaring64_bitmap_t *r) {
    roaring64_bitmap_t *result = roaring64_bitmap_create();

    art_iterator_t it = art_init_iterator((art_t *)&r->art, /*first=*/true);
    while (it.value != NULL) {
        leaf_t leaf = (leaf_t)*it.value;
        uint8_t result_typecode = get_typecode(leaf);
        container_t *result_container = get_copy_of_container(
            get_container(r, leaf), &result_typecode, /*copy_on_write=*/false);
        leaf_t result_leaf =
            add_container(result, result_container, result_typecode);
        art_insert(&result->art, it.key, (art_val_t)result_leaf);
        art_iterator_next(&it);
    }
    return result;
}

void roaring64_bitmap_overwrite(roaring64_bitmap_t *dest,
                                const roaring64_bitmap_t *src) {
    if (dest == src) {
        return;
    }

    // Free dest's containers.
    art_iterator_t it = art_init_iterator(&dest->art, /*first=*/true);
    while (it.value != NULL) {
        leaf_t leaf = (leaf_t)*it.value;
        container_free(get_container(dest, leaf), get_typecode(leaf));
        art_iterator_next(&it);
    }
    art_free(&dest->art);

    // Reinitialize dest.
    art_init_cleared(&dest->art);
    dest->flags = 0;
    dest->first_free = 0;
    if (dest->capacity > 0) {
        memset(dest->containers, 0,
               sizeof(dest->containers[0]) * dest->capacity);
    }

    // Copy src's containers into dest.
    it = art_init_iterator((art_t *)&src->art, /*first=*/true);
    while (it.value != NULL) {
        leaf_t leaf = (leaf_t)*it.value;
        uint8_t typecode = get_typecode(leaf);
        container_t *container = get_copy_of_container(
            get_container(src, leaf), &typecode, /*copy_on_write=*/false);
        leaf_t dest_leaf = add_container(dest, container, typecode);
        art_insert(&dest->art, it.key, (art_val_t)dest_leaf);
        art_iterator_next(&it);
    }
}

/**
 * Steal the containers from a 32-bit bitmap and insert them into a 64-bit
 * bitmap (with an offset)
 *
 * After calling this function, the original bitmap will be empty, and the
 * returned bitmap will contain all the values from the original bitmap.
 */
static void move_from_roaring32_offset(roaring64_bitmap_t *dst,
                                       roaring_bitmap_t *src,
                                       uint32_t high_bits) {
    uint64_t key_base = ((uint64_t)high_bits) << 32;
    uint32_t r32_size = ra_get_size(&src->high_low_container);
    for (uint32_t i = 0; i < r32_size; ++i) {
        uint16_t key = ra_get_key_at_index(&src->high_low_container, i);
        uint8_t typecode;
        container_t *container = ra_get_container_at_index(
            &src->high_low_container, (uint16_t)i, &typecode);

        uint8_t high48[ART_KEY_BYTES];
        uint64_t high48_bits = key_base | ((uint64_t)key << 16);
        split_key(high48_bits, high48);
        leaf_t leaf = add_container(dst, container, typecode);
        art_insert(&dst->art, high48, (art_val_t)leaf);
    }
    // We stole all the containers, so leave behind a size of zero
    src->high_low_container.size = 0;
}

roaring64_bitmap_t *roaring64_bitmap_move_from_roaring32(
    roaring_bitmap_t *bitmap32) {
    roaring64_bitmap_t *result = roaring64_bitmap_create();

    move_from_roaring32_offset(result, bitmap32, 0);

    return result;
}

roaring64_bitmap_t *roaring64_bitmap_from_range(uint64_t min, uint64_t max,
                                                uint64_t step) {
    if (step == 0 || max <= min) {
        return NULL;
    }
    roaring64_bitmap_t *r = roaring64_bitmap_create();
    if (step >= (1 << 16)) {
        // Only one value per container.
        for (uint64_t value = min; value < max; value += step) {
            roaring64_bitmap_add(r, value);
            if (value > UINT64_MAX - step) {
                break;
            }
        }
        return r;
    }
    do {
        uint64_t high_bits = min & 0xFFFFFFFFFFFF0000;
        uint16_t container_min = min & 0xFFFF;
        uint32_t container_max = (uint32_t)minimum(max - high_bits, 1 << 16);

        uint8_t typecode;
        container_t *container = container_from_range(
            &typecode, container_min, container_max, (uint16_t)step);

        uint8_t high48[ART_KEY_BYTES];
        split_key(min, high48);
        leaf_t leaf = add_container(r, container, typecode);
        art_insert(&r->art, high48, (art_val_t)leaf);

        uint64_t gap = container_max - container_min + step - 1;
        uint64_t increment = gap - (gap % step);
        if (min > UINT64_MAX - increment) {
            break;
        }
        min += increment;
    } while (min < max);
    return r;
}

roaring64_bitmap_t *roaring64_bitmap_of_ptr(size_t n_args,
                                            const uint64_t *vals) {
    roaring64_bitmap_t *r = roaring64_bitmap_create();
    roaring64_bitmap_add_many(r, n_args, vals);
    return r;
}

static inline leaf_t *containerptr_roaring64_bitmap_add(roaring64_bitmap_t *r,
                                                        uint8_t *high48,
                                                        uint16_t low16,
                                                        leaf_t *leaf) {
    if (leaf != NULL) {
        uint8_t typecode = get_typecode(*leaf);
        container_t *container = get_container(r, *leaf);
        uint8_t typecode2;
        container_t *container2 =
            container_add(container, low16, typecode, &typecode2);
        if (container2 != container) {
            container_free(container, typecode);
            replace_container(r, leaf, container2, typecode2);
        }
        return leaf;
    } else {
        array_container_t *ac = array_container_create();
        uint8_t typecode;
        container_t *container =
            container_add(ac, low16, ARRAY_CONTAINER_TYPE, &typecode);
        assert(ac == container);
        leaf_t new_leaf = add_container(r, container, typecode);
        return (leaf_t *)art_insert(&r->art, high48, (art_val_t)new_leaf);
    }
}

void roaring64_bitmap_add(roaring64_bitmap_t *r, uint64_t val) {
    uint8_t high48[ART_KEY_BYTES];
    uint16_t low16 = split_key(val, high48);
    leaf_t *leaf = (leaf_t *)art_find(&r->art, high48);
    containerptr_roaring64_bitmap_add(r, high48, low16, leaf);
}

bool roaring64_bitmap_add_checked(roaring64_bitmap_t *r, uint64_t val) {
    uint8_t high48[ART_KEY_BYTES];
    uint16_t low16 = split_key(val, high48);
    leaf_t *leaf = (leaf_t *)art_find(&r->art, high48);

    int old_cardinality = 0;
    if (leaf != NULL) {
        old_cardinality = container_get_cardinality(get_container(r, *leaf),
                                                    get_typecode(*leaf));
    }
    leaf = containerptr_roaring64_bitmap_add(r, high48, low16, leaf);
    int new_cardinality =
        container_get_cardinality(get_container(r, *leaf), get_typecode(*leaf));
    return old_cardinality != new_cardinality;
}

void roaring64_bitmap_add_bulk(roaring64_bitmap_t *r,
                               roaring64_bulk_context_t *context,
                               uint64_t val) {
    uint8_t high48[ART_KEY_BYTES];
    uint16_t low16 = split_key(val, high48);
    leaf_t *leaf = context->leaf;
    if (leaf != NULL && compare_high48(context->high_bytes, high48) == 0) {
        // We're at a container with the correct high bits.
        uint8_t typecode1 = get_typecode(*leaf);
        container_t *container1 = get_container(r, *leaf);
        uint8_t typecode2;
        container_t *container2 =
            container_add(container1, low16, typecode1, &typecode2);
        if (container2 != container1) {
            container_free(container1, typecode1);
            replace_container(r, leaf, container2, typecode2);
        }
    } else {
        // We're not positioned anywhere yet or the high bits of the key
        // differ.
        leaf = (leaf_t *)art_find(&r->art, high48);
        context->leaf =
            containerptr_roaring64_bitmap_add(r, high48, low16, leaf);
        memcpy(context->high_bytes, high48, ART_KEY_BYTES);
    }
}

void roaring64_bitmap_add_many(roaring64_bitmap_t *r, size_t n_args,
                               const uint64_t *vals) {
    if (n_args == 0) {
        return;
    }
    const uint64_t *end = vals + n_args;
    roaring64_bulk_context_t context = CROARING_ZERO_INITIALIZER;
    for (const uint64_t *current_val = vals; current_val != end;
         current_val++) {
        roaring64_bitmap_add_bulk(r, &context, *current_val);
    }
}

static inline void add_range_closed_at(roaring64_bitmap_t *r, art_t *art,
                                       uint8_t *high48, uint16_t min,
                                       uint16_t max) {
    leaf_t *leaf = (leaf_t *)art_find(art, high48);
    if (leaf != NULL) {
        uint8_t typecode1 = get_typecode(*leaf);
        container_t *container1 = get_container(r, *leaf);
        uint8_t typecode2;
        container_t *container2 =
            container_add_range(container1, typecode1, min, max, &typecode2);
        if (container2 != container1) {
            container_free(container1, typecode1);
            replace_container(r, leaf, container2, typecode2);
        }
        return;
    }
    uint8_t typecode;
    // container_add_range is inclusive, but `container_range_of_ones` is
    // exclusive.
    container_t *container = container_range_of_ones(min, max + 1, &typecode);
    leaf_t new_leaf = add_container(r, container, typecode);
    art_insert(art, high48, (art_val_t)new_leaf);
}

void roaring64_bitmap_add_range(roaring64_bitmap_t *r, uint64_t min,
                                uint64_t max) {
    if (min >= max) {
        return;
    }
    roaring64_bitmap_add_range_closed(r, min, max - 1);
}

void roaring64_bitmap_add_range_closed(roaring64_bitmap_t *r, uint64_t min,
                                       uint64_t max) {
    if (min > max) {
        return;
    }

    art_t *art = &r->art;
    uint8_t min_high48[ART_KEY_BYTES];
    uint16_t min_low16 = split_key(min, min_high48);
    uint8_t max_high48[ART_KEY_BYTES];
    uint16_t max_low16 = split_key(max, max_high48);
    if (compare_high48(min_high48, max_high48) == 0) {
        // Only populate range within one container.
        add_range_closed_at(r, art, min_high48, min_low16, max_low16);
        return;
    }

    // Populate a range across containers. Fill intermediate containers
    // entirely.
    add_range_closed_at(r, art, min_high48, min_low16, 0xffff);
    uint64_t min_high_bits = min >> 16;
    uint64_t max_high_bits = max >> 16;
    for (uint64_t current = min_high_bits + 1; current < max_high_bits;
         ++current) {
        uint8_t current_high48[ART_KEY_BYTES];
        split_key(current << 16, current_high48);
        add_range_closed_at(r, art, current_high48, 0, 0xffff);
    }
    add_range_closed_at(r, art, max_high48, 0, max_low16);
}

bool roaring64_bitmap_contains(const roaring64_bitmap_t *r, uint64_t val) {
    uint8_t high48[ART_KEY_BYTES];
    uint16_t low16 = split_key(val, high48);
    leaf_t *leaf = (leaf_t *)art_find(&r->art, high48);
    if (leaf != NULL) {
        return container_contains(get_container(r, *leaf), low16,
                                  get_typecode(*leaf));
    }
    return false;
}

bool roaring64_bitmap_contains_range(const roaring64_bitmap_t *r, uint64_t min,
                                     uint64_t max) {
    if (min >= max) {
        return true;
    }

    uint8_t min_high48[ART_KEY_BYTES];
    uint16_t min_low16 = split_key(min, min_high48);
    uint8_t max_high48[ART_KEY_BYTES];
    uint16_t max_low16 = split_key(max, max_high48);
    uint64_t max_high48_bits = (max - 1) & 0xFFFFFFFFFFFF0000;  // Inclusive

    art_iterator_t it = art_lower_bound((art_t *)&r->art, min_high48);
    if (it.value == NULL || combine_key(it.key, 0) > min) {
        return false;
    }
    uint64_t prev_high48_bits = min & 0xFFFFFFFFFFFF0000;
    while (it.value != NULL) {
        uint64_t current_high48_bits = combine_key(it.key, 0);
        if (current_high48_bits > max_high48_bits) {
            // We've passed the end of the range with all containers containing
            // the range.
            return true;
        }
        if (current_high48_bits - prev_high48_bits > 0x10000) {
            // There is a gap in the iterator that falls in the range.
            return false;
        }

        leaf_t leaf = (leaf_t)*it.value;
        uint32_t container_min = 0;
        if (compare_high48(it.key, min_high48) == 0) {
            container_min = min_low16;
        }
        uint32_t container_max = 0xFFFF + 1;  // Exclusive
        if (compare_high48(it.key, max_high48) == 0) {
            container_max = max_low16;
        }

        // For the first and last containers we use container_contains_range,
        // for the intermediate containers we can use container_is_full.
        if (container_min == 0 && container_max == 0xFFFF + 1) {
            if (!container_is_full(get_container(r, leaf),
                                   get_typecode(leaf))) {
                return false;
            }
        } else if (!container_contains_range(get_container(r, leaf),
                                             container_min, container_max,
                                             get_typecode(leaf))) {
            return false;
        }
        prev_high48_bits = current_high48_bits;
        art_iterator_next(&it);
    }
    return prev_high48_bits == max_high48_bits;
}

bool roaring64_bitmap_contains_bulk(const roaring64_bitmap_t *r,
                                    roaring64_bulk_context_t *context,
                                    uint64_t val) {
    uint8_t high48[ART_KEY_BYTES];
    uint16_t low16 = split_key(val, high48);

    if (context->leaf == NULL ||
        art_compare_keys(context->high_bytes, high48) != 0) {
        // We're not positioned anywhere yet or the high bits of the key
        // differ.
        leaf_t *leaf = (leaf_t *)art_find(&r->art, high48);
        if (leaf == NULL) {
            return false;
        }
        context->leaf = leaf;
        memcpy(context->high_bytes, high48, ART_KEY_BYTES);
    }
    return container_contains(get_container(r, *context->leaf), low16,
                              get_typecode(*context->leaf));
}

bool roaring64_bitmap_select(const roaring64_bitmap_t *r, uint64_t rank,
                             uint64_t *element) {
    art_iterator_t it = art_init_iterator((art_t *)&r->art, /*first=*/true);
    uint64_t start_rank = 0;
    while (it.value != NULL) {
        leaf_t leaf = (leaf_t)*it.value;
        uint64_t cardinality = container_get_cardinality(get_container(r, leaf),
                                                         get_typecode(leaf));
        if (start_rank + cardinality > rank) {
            uint32_t uint32_start = 0;
            uint32_t uint32_rank = rank - start_rank;
            uint32_t uint32_element = 0;
            if (container_select(get_container(r, leaf), get_typecode(leaf),
                                 &uint32_start, uint32_rank, &uint32_element)) {
                *element = combine_key(it.key, (uint16_t)uint32_element);
                return true;
            }
            return false;
        }
        start_rank += cardinality;
        art_iterator_next(&it);
    }
    return false;
}

uint64_t roaring64_bitmap_rank(const roaring64_bitmap_t *r, uint64_t val) {
    uint8_t high48[ART_KEY_BYTES];
    uint16_t low16 = split_key(val, high48);

    art_iterator_t it = art_init_iterator((art_t *)&r->art, /*first=*/true);
    uint64_t rank = 0;
    while (it.value != NULL) {
        leaf_t leaf = (leaf_t)*it.value;
        int compare_result = compare_high48(it.key, high48);
        if (compare_result < 0) {
            rank += container_get_cardinality(get_container(r, leaf),
                                              get_typecode(leaf));
        } else if (compare_result == 0) {
            return rank + container_rank(get_container(r, leaf),
                                         get_typecode(leaf), low16);
        } else {
            return rank;
        }
        art_iterator_next(&it);
    }
    return rank;
}

bool roaring64_bitmap_get_index(const roaring64_bitmap_t *r, uint64_t val,
                                uint64_t *out_index) {
    uint8_t high48[ART_KEY_BYTES];
    uint16_t low16 = split_key(val, high48);

    art_iterator_t it = art_init_iterator((art_t *)&r->art, /*first=*/true);
    uint64_t index = 0;
    while (it.value != NULL) {
        leaf_t leaf = (leaf_t)*it.value;
        int compare_result = compare_high48(it.key, high48);
        if (compare_result < 0) {
            index += container_get_cardinality(get_container(r, leaf),
                                               get_typecode(leaf));
        } else if (compare_result == 0) {
            int index16 = container_get_index(get_container(r, leaf),
                                              get_typecode(leaf), low16);
            if (index16 < 0) {
                return false;
            }
            *out_index = index + index16;
            return true;
        } else {
            return false;
        }
        art_iterator_next(&it);
    }
    return false;
}

// Returns true if a container was removed.
static inline bool containerptr_roaring64_bitmap_remove(roaring64_bitmap_t *r,
                                                        uint8_t *high48,
                                                        uint16_t low16,
                                                        leaf_t *leaf) {
    if (leaf == NULL) {
        return false;
    }

    uint8_t typecode = get_typecode(*leaf);
    container_t *container = get_container(r, *leaf);
    uint8_t typecode2;
    container_t *container2 =
        container_remove(container, low16, typecode, &typecode2);
    if (container2 != container) {
        container_free(container, typecode);
        replace_container(r, leaf, container2, typecode2);
    }
    if (!container_nonzero_cardinality(container2, typecode2)) {
        container_free(container2, typecode2);
        bool erased = art_erase(&r->art, high48, (art_val_t *)leaf);
        assert(erased);
        (void)erased;
        remove_container(r, *leaf);
        return true;
    }
    return false;
}

void roaring64_bitmap_remove(roaring64_bitmap_t *r, uint64_t val) {
    art_t *art = &r->art;
    uint8_t high48[ART_KEY_BYTES];
    uint16_t low16 = split_key(val, high48);

    leaf_t *leaf = (leaf_t *)art_find(art, high48);
    containerptr_roaring64_bitmap_remove(r, high48, low16, leaf);
}

bool roaring64_bitmap_remove_checked(roaring64_bitmap_t *r, uint64_t val) {
    art_t *art = &r->art;
    uint8_t high48[ART_KEY_BYTES];
    uint16_t low16 = split_key(val, high48);
    leaf_t *leaf = (leaf_t *)art_find(art, high48);

    if (leaf == NULL) {
        return false;
    }
    int old_cardinality =
        container_get_cardinality(get_container(r, *leaf), get_typecode(*leaf));
    if (containerptr_roaring64_bitmap_remove(r, high48, low16, leaf)) {
        return true;
    }
    int new_cardinality =
        container_get_cardinality(get_container(r, *leaf), get_typecode(*leaf));
    return new_cardinality != old_cardinality;
}

void roaring64_bitmap_remove_bulk(roaring64_bitmap_t *r,
                                  roaring64_bulk_context_t *context,
                                  uint64_t val) {
    art_t *art = &r->art;
    uint8_t high48[ART_KEY_BYTES];
    uint16_t low16 = split_key(val, high48);
    if (context->leaf != NULL &&
        compare_high48(context->high_bytes, high48) == 0) {
        // We're at a container with the correct high bits.
        uint8_t typecode = get_typecode(*context->leaf);
        container_t *container = get_container(r, *context->leaf);
        uint8_t typecode2;
        container_t *container2 =
            container_remove(container, low16, typecode, &typecode2);
        if (container2 != container) {
            container_free(container, typecode);
            replace_container(r, context->leaf, container2, typecode2);
        }
        if (!container_nonzero_cardinality(container2, typecode2)) {
            container_free(container2, typecode2);
            leaf_t leaf = 0;
            bool erased = art_erase(art, high48, (art_val_t *)&leaf);
            assert(erased);
            (void)erased;
            remove_container(r, leaf);
            context->leaf = NULL;
        }
    } else {
        // We're not positioned anywhere yet or the high bits of the key
        // differ.
        leaf_t *leaf = (leaf_t *)art_find(art, high48);
        containerptr_roaring64_bitmap_remove(r, high48, low16, leaf);
        context->leaf = leaf;
        memcpy(context->high_bytes, high48, ART_KEY_BYTES);
    }
}

void roaring64_bitmap_remove_many(roaring64_bitmap_t *r, size_t n_args,
                                  const uint64_t *vals) {
    if (n_args == 0) {
        return;
    }
    const uint64_t *end = vals + n_args;
    roaring64_bulk_context_t context = CROARING_ZERO_INITIALIZER;
    for (const uint64_t *current_val = vals; current_val != end;
         current_val++) {
        roaring64_bitmap_remove_bulk(r, &context, *current_val);
    }
}

static inline void remove_range_closed_at(roaring64_bitmap_t *r, art_t *art,
                                          uint8_t *high48, uint16_t min,
                                          uint16_t max) {
    leaf_t *leaf = (leaf_t *)art_find(art, high48);
    if (leaf == NULL) {
        return;
    }
    uint8_t typecode = get_typecode(*leaf);
    container_t *container = get_container(r, *leaf);
    uint8_t typecode2;
    container_t *container2 =
        container_remove_range(container, typecode, min, max, &typecode2);
    if (container2 != container) {
        container_free(container, typecode);
        if (container2 != NULL) {
            replace_container(r, leaf, container2, typecode2);
        } else {
            bool erased = art_erase(art, high48, NULL);
            assert(erased);
            (void)erased;
            remove_container(r, *leaf);
        }
    }
}

void roaring64_bitmap_remove_range(roaring64_bitmap_t *r, uint64_t min,
                                   uint64_t max) {
    if (min >= max) {
        return;
    }
    roaring64_bitmap_remove_range_closed(r, min, max - 1);
}

void roaring64_bitmap_remove_range_closed(roaring64_bitmap_t *r, uint64_t min,
                                          uint64_t max) {
    if (min > max) {
        return;
    }

    art_t *art = &r->art;
    uint8_t min_high48[ART_KEY_BYTES];
    uint16_t min_low16 = split_key(min, min_high48);
    uint8_t max_high48[ART_KEY_BYTES];
    uint16_t max_low16 = split_key(max, max_high48);
    if (compare_high48(min_high48, max_high48) == 0) {
        // Only remove a range within one container.
        remove_range_closed_at(r, art, min_high48, min_low16, max_low16);
        return;
    }

    // Remove a range across containers. Remove intermediate containers
    // entirely.
    remove_range_closed_at(r, art, min_high48, min_low16, 0xffff);

    art_iterator_t it = art_upper_bound(art, min_high48);
    while (it.value != NULL && art_compare_keys(it.key, max_high48) < 0) {
        leaf_t leaf = 0;
        bool erased = art_iterator_erase(&it, (art_val_t *)&leaf);
        assert(erased);
        (void)erased;
        container_free(get_container(r, leaf), get_typecode(leaf));
        remove_container(r, leaf);
    }
    remove_range_closed_at(r, art, max_high48, 0, max_low16);
}

void roaring64_bitmap_clear(roaring64_bitmap_t *r) {
    roaring64_bitmap_remove_range_closed(r, 0, UINT64_MAX);
}

uint64_t roaring64_bitmap_get_cardinality(const roaring64_bitmap_t *r) {
    art_iterator_t it = art_init_iterator((art_t *)&r->art, /*first=*/true);
    uint64_t cardinality = 0;
    while (it.value != NULL) {
        leaf_t leaf = (leaf_t)*it.value;
        cardinality += container_get_cardinality(get_container(r, leaf),
                                                 get_typecode(leaf));
        art_iterator_next(&it);
    }
    return cardinality;
}

uint64_t roaring64_bitmap_range_cardinality(const roaring64_bitmap_t *r,
                                            uint64_t min, uint64_t max) {
    if (min >= max) {
        return 0;
    }
    // Convert to a closed range
    // No underflow here: passing the above condition implies min < max, so
    // there is a number less than max
    return roaring64_bitmap_range_closed_cardinality(r, min, max - 1);
}

uint64_t roaring64_bitmap_range_closed_cardinality(const roaring64_bitmap_t *r,
                                                   uint64_t min, uint64_t max) {
    if (min > max) {
        return 0;
    }

    uint64_t cardinality = 0;
    uint8_t min_high48[ART_KEY_BYTES];
    uint16_t min_low16 = split_key(min, min_high48);
    uint8_t max_high48[ART_KEY_BYTES];
    uint16_t max_low16 = split_key(max, max_high48);

    art_iterator_t it = art_lower_bound((art_t *)&r->art, min_high48);
    while (it.value != NULL) {
        int max_compare_result = compare_high48(it.key, max_high48);
        if (max_compare_result > 0) {
            // We're outside the range.
            break;
        }

        leaf_t leaf = (leaf_t)*it.value;
        uint8_t typecode = get_typecode(leaf);
        container_t *container = get_container(r, leaf);
        if (max_compare_result == 0) {
            // We're at the max high key, add only the range up to the low
            // 16 bits of max.
            cardinality += container_rank(container, typecode, max_low16);
        } else {
            // We're not yet at the max high key, add the full container
            // range.
            cardinality += container_get_cardinality(container, typecode);
        }
        if (compare_high48(it.key, min_high48) == 0 && min_low16 > 0) {
            // We're at the min high key, remove the range up to the low 16
            // bits of min.
            cardinality -= container_rank(container, typecode, min_low16 - 1);
        }
        art_iterator_next(&it);
    }
    return cardinality;
}

bool roaring64_bitmap_is_empty(const roaring64_bitmap_t *r) {
    return art_is_empty(&r->art);
}

uint64_t roaring64_bitmap_minimum(const roaring64_bitmap_t *r) {
    art_iterator_t it = art_init_iterator((art_t *)&r->art, /*first=*/true);
    if (it.value == NULL) {
        return UINT64_MAX;
    }
    leaf_t leaf = (leaf_t)*it.value;
    return combine_key(
        it.key, container_minimum(get_container(r, leaf), get_typecode(leaf)));
}

uint64_t roaring64_bitmap_maximum(const roaring64_bitmap_t *r) {
    art_iterator_t it = art_init_iterator((art_t *)&r->art, /*first=*/false);
    if (it.value == NULL) {
        return 0;
    }
    leaf_t leaf = (leaf_t)*it.value;
    return combine_key(
        it.key, container_maximum(get_container(r, leaf), get_typecode(leaf)));
}

bool roaring64_bitmap_remove_run_compression(roaring64_bitmap_t *r) {
    art_iterator_t it = art_init_iterator(&r->art, /*first=*/true);
    bool removed = false;
    while (it.value != NULL) {
        leaf_t *leaf = (leaf_t *)it.value;
        if (get_typecode(*leaf) == RUN_CONTAINER_TYPE) {
            run_container_t *run = CAST_run(get_container(r, *leaf));
            int32_t card = run_container_cardinality(run);
            uint8_t new_typecode;
            container_t *new_container =
                convert_to_bitset_or_array_container(run, card, &new_typecode);
            run_container_free(run);
            replace_container(r, leaf, new_container, new_typecode);
            removed = true;
        }
        art_iterator_next(&it);
    }
    return removed;
}

bool roaring64_bitmap_run_optimize(roaring64_bitmap_t *r) {
    art_iterator_t it = art_init_iterator(&r->art, /*first=*/true);
    bool has_run_container = false;
    while (it.value != NULL) {
        leaf_t *leaf = (leaf_t *)it.value;
        uint8_t new_typecode;
        // We don't need to free the existing container if a new one was
        // created, convert_run_optimize does that internally.
        container_t *new_container = convert_run_optimize(
            get_container(r, *leaf), get_typecode(*leaf), &new_typecode);
        replace_container(r, leaf, new_container, new_typecode);
        has_run_container |= new_typecode == RUN_CONTAINER_TYPE;
        art_iterator_next(&it);
    }
    return has_run_container;
}

static void move_to_shrink(roaring64_bitmap_t *r, leaf_t *leaf) {
    uint64_t idx = get_index(*leaf);
    if (idx < r->first_free) {
        return;
    }
    r->containers[r->first_free] = get_container(r, *leaf);
    r->containers[idx] = NULL;
    *leaf = create_leaf(r->first_free, get_typecode(*leaf));
    r->first_free = next_free_container_idx(r);
}

static inline bool is_shrunken(const roaring64_bitmap_t *r) {
    return art_is_shrunken(&r->art) && r->first_free == r->capacity;
}

size_t roaring64_bitmap_shrink_to_fit(roaring64_bitmap_t *r) {
    size_t freed = art_shrink_to_fit(&r->art);
    art_iterator_t it = art_init_iterator(&r->art, true);
    while (it.value != NULL) {
        leaf_t *leaf = (leaf_t *)it.value;
        freed += container_shrink_to_fit(get_container(r, *leaf),
                                         get_typecode(*leaf));
        move_to_shrink(r, leaf);
        art_iterator_next(&it);
    }
    if (is_shrunken(r)) {
        return freed;
    }
    uint64_t new_capacity = r->first_free;
    if (new_capacity < r->capacity) {
        r->containers = (container_t **)roaring_realloc(
            r->containers, new_capacity * sizeof(container_t *));
        freed += (r->capacity - new_capacity) * sizeof(container_t *);
        r->capacity = new_capacity;
    }
    return freed;
}

/**
 *  (For advanced users.)
 * Collect statistics about the bitmap
 */
void roaring64_bitmap_statistics(const roaring64_bitmap_t *r,
                                 roaring64_statistics_t *stat) {
    memset(stat, 0, sizeof(*stat));
    stat->min_value = roaring64_bitmap_minimum(r);
    stat->max_value = roaring64_bitmap_maximum(r);

    art_iterator_t it = art_init_iterator((art_t *)&r->art, true);
    while (it.value != NULL) {
        leaf_t leaf = (leaf_t)*it.value;
        stat->n_containers++;
        uint8_t truetype =
            get_container_type(get_container(r, leaf), get_typecode(leaf));
        uint32_t card = container_get_cardinality(get_container(r, leaf),
                                                  get_typecode(leaf));
        uint32_t sbytes =
            container_size_in_bytes(get_container(r, leaf), get_typecode(leaf));
        stat->cardinality += card;
        switch (truetype) {
            case BITSET_CONTAINER_TYPE:
                stat->n_bitset_containers++;
                stat->n_values_bitset_containers += card;
                stat->n_bytes_bitset_containers += sbytes;
                break;
            case ARRAY_CONTAINER_TYPE:
                stat->n_array_containers++;
                stat->n_values_array_containers += card;
                stat->n_bytes_array_containers += sbytes;
                break;
            case RUN_CONTAINER_TYPE:
                stat->n_run_containers++;
                stat->n_values_run_containers += card;
                stat->n_bytes_run_containers += sbytes;
                break;
            default:
                assert(false);
                roaring_unreachable;
        }
        art_iterator_next(&it);
    }
}

static bool roaring64_leaf_internal_validate(const art_val_t val,
                                             const char **reason,
                                             void *context) {
    leaf_t leaf = (leaf_t)val;
    roaring64_bitmap_t *r = (roaring64_bitmap_t *)context;
    return container_internal_validate(get_container(r, leaf),
                                       get_typecode(leaf), reason);
}

bool roaring64_bitmap_internal_validate(const roaring64_bitmap_t *r,
                                        const char **reason) {
    return art_internal_validate(&r->art, reason,
                                 roaring64_leaf_internal_validate, (void *)r);
}

bool roaring64_bitmap_equals(const roaring64_bitmap_t *r1,
                             const roaring64_bitmap_t *r2) {
    art_iterator_t it1 = art_init_iterator((art_t *)&r1->art, /*first=*/true);
    art_iterator_t it2 = art_init_iterator((art_t *)&r2->art, /*first=*/true);

    while (it1.value != NULL && it2.value != NULL) {
        if (compare_high48(it1.key, it2.key) != 0) {
            return false;
        }
        leaf_t leaf1 = (leaf_t)*it1.value;
        leaf_t leaf2 = (leaf_t)*it2.value;
        if (!container_equals(get_container(r1, leaf1), get_typecode(leaf1),
                              get_container(r2, leaf2), get_typecode(leaf2))) {
            return false;
        }
        art_iterator_next(&it1);
        art_iterator_next(&it2);
    }
    return it1.value == NULL && it2.value == NULL;
}

bool roaring64_bitmap_is_subset(const roaring64_bitmap_t *r1,
                                const roaring64_bitmap_t *r2) {
    art_iterator_t it1 = art_init_iterator((art_t *)&r1->art, /*first=*/true);
    art_iterator_t it2 = art_init_iterator((art_t *)&r2->art, /*first=*/true);

    while (it1.value != NULL) {
        bool it2_present = it2.value != NULL;

        int compare_result = 0;
        if (it2_present) {
            compare_result = compare_high48(it1.key, it2.key);
            if (compare_result == 0) {
                leaf_t leaf1 = (leaf_t)*it1.value;
                leaf_t leaf2 = (leaf_t)*it2.value;
                if (!container_is_subset(
                        get_container(r1, leaf1), get_typecode(leaf1),
                        get_container(r2, leaf2), get_typecode(leaf2))) {
                    return false;
                }
                art_iterator_next(&it1);
                art_iterator_next(&it2);
            }
        }
        if (!it2_present || compare_result < 0) {
            return false;
        } else if (compare_result > 0) {
            art_iterator_lower_bound(&it2, it1.key);
        }
    }
    return true;
}

bool roaring64_bitmap_is_strict_subset(const roaring64_bitmap_t *r1,
                                       const roaring64_bitmap_t *r2) {
    return roaring64_bitmap_get_cardinality(r1) <
               roaring64_bitmap_get_cardinality(r2) &&
           roaring64_bitmap_is_subset(r1, r2);
}

roaring64_bitmap_t *roaring64_bitmap_and(const roaring64_bitmap_t *r1,
                                         const roaring64_bitmap_t *r2) {
    roaring64_bitmap_t *result = roaring64_bitmap_create();

    art_iterator_t it1 = art_init_iterator((art_t *)&r1->art, /*first=*/true);
    art_iterator_t it2 = art_init_iterator((art_t *)&r2->art, /*first=*/true);

    while (it1.value != NULL && it2.value != NULL) {
        // Cases:
        // 1. it1 <  it2 -> it1++
        // 2. it1 == it1 -> output it1 & it2, it1++, it2++
        // 3. it1 >  it2 -> it2++
        int compare_result = compare_high48(it1.key, it2.key);
        if (compare_result == 0) {
            // Case 2: iterators at the same high key position.
            leaf_t leaf1 = (leaf_t)*it1.value;
            leaf_t leaf2 = (leaf_t)*it2.value;
            uint8_t result_typecode;
            container_t *result_container =
                container_and(get_container(r1, leaf1), get_typecode(leaf1),
                              get_container(r2, leaf2), get_typecode(leaf2),
                              &result_typecode);
            if (container_nonzero_cardinality(result_container,
                                              result_typecode)) {
                leaf_t result_leaf =
                    add_container(result, result_container, result_typecode);
                art_insert(&result->art, it1.key, (art_val_t)result_leaf);
            } else {
                container_free(result_container, result_typecode);
            }
            art_iterator_next(&it1);
            art_iterator_next(&it2);
        } else if (compare_result < 0) {
            // Case 1: it1 is before it2.
            art_iterator_lower_bound(&it1, it2.key);
        } else {
            // Case 3: it2 is before it1.
            art_iterator_lower_bound(&it2, it1.key);
        }
    }
    return result;
}

uint64_t roaring64_bitmap_and_cardinality(const roaring64_bitmap_t *r1,
                                          const roaring64_bitmap_t *r2) {
    uint64_t result = 0;

    art_iterator_t it1 = art_init_iterator((art_t *)&r1->art, /*first=*/true);
    art_iterator_t it2 = art_init_iterator((art_t *)&r2->art, /*first=*/true);

    while (it1.value != NULL && it2.value != NULL) {
        // Cases:
        // 1. it1 <  it2 -> it1++
        // 2. it1 == it1 -> output cardinaltiy it1 & it2, it1++, it2++
        // 3. it1 >  it2 -> it2++
        int compare_result = compare_high48(it1.key, it2.key);
        if (compare_result == 0) {
            // Case 2: iterators at the same high key position.
            leaf_t leaf1 = (leaf_t)*it1.value;
            leaf_t leaf2 = (leaf_t)*it2.value;
            result += container_and_cardinality(
                get_container(r1, leaf1), get_typecode(leaf1),
                get_container(r2, leaf2), get_typecode(leaf2));
            art_iterator_next(&it1);
            art_iterator_next(&it2);
        } else if (compare_result < 0) {
            // Case 1: it1 is before it2.
            art_iterator_lower_bound(&it1, it2.key);
        } else {
            // Case 3: it2 is before it1.
            art_iterator_lower_bound(&it2, it1.key);
        }
    }
    return result;
}

// Inplace and (modifies its first argument).
void roaring64_bitmap_and_inplace(roaring64_bitmap_t *r1,
                                  const roaring64_bitmap_t *r2) {
    if (r1 == r2) {
        return;
    }
    art_iterator_t it1 = art_init_iterator(&r1->art, /*first=*/true);
    art_iterator_t it2 = art_init_iterator((art_t *)&r2->art, /*first=*/true);

    while (it1.value != NULL) {
        // Cases:
        // 1. !it2_present -> erase it1
        // 2. it2_present
        //    a. it1 <  it2 -> erase it1
        //    b. it1 == it2 -> output it1 & it2, it1++, it2++
        //    c. it1 >  it2 -> it2++
        bool it2_present = it2.value != NULL;
        int compare_result = 0;
        if (it2_present) {
            compare_result = compare_high48(it1.key, it2.key);
            if (compare_result == 0) {
                // Case 2a: iterators at the same high key position.
                leaf_t *leaf1 = (leaf_t *)it1.value;
                leaf_t leaf2 = (leaf_t)*it2.value;

                // We do the computation "in place" only when c1 is not a
                // shared container. Rationale: using a shared container
                // safely with in place computation would require making a
                // copy and then doing the computation in place which is
                // likely less efficient than avoiding in place entirely and
                // always generating a new container.
                uint8_t typecode = get_typecode(*leaf1);
                container_t *container = get_container(r1, *leaf1);
                uint8_t typecode2;
                container_t *container2;
                if (typecode == SHARED_CONTAINER_TYPE) {
                    container2 = container_and(container, typecode,
                                               get_container(r2, leaf2),
                                               get_typecode(leaf2), &typecode2);
                } else {
                    container2 = container_iand(
                        container, typecode, get_container(r2, leaf2),
                        get_typecode(leaf2), &typecode2);
                }

                if (container2 != container) {
                    container_free(container, typecode);
                }
                if (!container_nonzero_cardinality(container2, typecode2)) {
                    container_free(container2, typecode2);
                    art_iterator_erase(&it1, NULL);
                    remove_container(r1, *leaf1);
                } else {
                    if (container2 != container) {
                        replace_container(r1, leaf1, container2, typecode2);
                    }
                    // Only advance the iterator if we didn't delete the
                    // leaf, as erasing advances by itself.
                    art_iterator_next(&it1);
                }
                art_iterator_next(&it2);
            }
        }

        if (!it2_present || compare_result < 0) {
            // Cases 1 and 3a: it1 is the only iterator or is before it2.
            leaf_t leaf = 0;
            bool erased = art_iterator_erase(&it1, (art_val_t *)&leaf);
            assert(erased);
            (void)erased;
            container_free(get_container(r1, leaf), get_typecode(leaf));
            remove_container(r1, leaf);
        } else if (compare_result > 0) {
            // Case 2c: it1 is after it2.
            art_iterator_lower_bound(&it2, it1.key);
        }
    }
}

bool roaring64_bitmap_intersect(const roaring64_bitmap_t *r1,
                                const roaring64_bitmap_t *r2) {
    bool intersect = false;
    art_iterator_t it1 = art_init_iterator((art_t *)&r1->art, /*first=*/true);
    art_iterator_t it2 = art_init_iterator((art_t *)&r2->art, /*first=*/true);

    while (it1.value != NULL && it2.value != NULL) {
        // Cases:
        // 1. it1 <  it2 -> it1++
        // 2. it1 == it1 -> intersect |= it1 & it2, it1++, it2++
        // 3. it1 >  it2 -> it2++
        int compare_result = compare_high48(it1.key, it2.key);
        if (compare_result == 0) {
            // Case 2: iterators at the same high key position.
            leaf_t leaf1 = (leaf_t)*it1.value;
            leaf_t leaf2 = (leaf_t)*it2.value;
            intersect |= container_intersect(
                get_container(r1, leaf1), get_typecode(leaf1),
                get_container(r2, leaf2), get_typecode(leaf2));
            art_iterator_next(&it1);
            art_iterator_next(&it2);
        } else if (compare_result < 0) {
            // Case 1: it1 is before it2.
            art_iterator_lower_bound(&it1, it2.key);
        } else {
            // Case 3: it2 is before it1.
            art_iterator_lower_bound(&it2, it1.key);
        }
    }
    return intersect;
}

bool roaring64_bitmap_intersect_with_range(const roaring64_bitmap_t *r,
                                           uint64_t min, uint64_t max) {
    if (min >= max) {
        return false;
    }
    roaring64_iterator_t it;
    roaring64_iterator_init_at(r, &it, /*first=*/true);
    if (!roaring64_iterator_move_equalorlarger(&it, min)) {
        return false;
    }
    return roaring64_iterator_has_value(&it) &&
           roaring64_iterator_value(&it) < max;
}

double roaring64_bitmap_jaccard_index(const roaring64_bitmap_t *r1,
                                      const roaring64_bitmap_t *r2) {
    uint64_t c1 = roaring64_bitmap_get_cardinality(r1);
    uint64_t c2 = roaring64_bitmap_get_cardinality(r2);
    uint64_t inter = roaring64_bitmap_and_cardinality(r1, r2);
    return (double)inter / (double)(c1 + c2 - inter);
}

roaring64_bitmap_t *roaring64_bitmap_or(const roaring64_bitmap_t *r1,
                                        const roaring64_bitmap_t *r2) {
    roaring64_bitmap_t *result = roaring64_bitmap_create();

    art_iterator_t it1 = art_init_iterator((art_t *)&r1->art, /*first=*/true);
    art_iterator_t it2 = art_init_iterator((art_t *)&r2->art, /*first=*/true);

    while (it1.value != NULL || it2.value != NULL) {
        bool it1_present = it1.value != NULL;
        bool it2_present = it2.value != NULL;

        // Cases:
        // 1. it1_present  && !it2_present -> output it1, it1++
        // 2. !it1_present && it2_present  -> output it2, it2++
        // 3. it1_present  && it2_present
        //    a. it1 <  it2 -> output it1, it1++
        //    b. it1 == it2 -> output it1 | it2, it1++, it2++
        //    c. it1 >  it2 -> output it2, it2++
        int compare_result = 0;
        if (it1_present && it2_present) {
            compare_result = compare_high48(it1.key, it2.key);
            if (compare_result == 0) {
                // Case 3b: iterators at the same high key position.
                leaf_t leaf1 = (leaf_t)*it1.value;
                leaf_t leaf2 = (leaf_t)*it2.value;
                uint8_t result_typecode;
                container_t *result_container =
                    container_or(get_container(r1, leaf1), get_typecode(leaf1),
                                 get_container(r2, leaf2), get_typecode(leaf2),
                                 &result_typecode);
                leaf_t result_leaf =
                    add_container(result, result_container, result_typecode);
                art_insert(&result->art, it1.key, (art_val_t)result_leaf);
                art_iterator_next(&it1);
                art_iterator_next(&it2);
            }
        }
        if ((it1_present && !it2_present) || compare_result < 0) {
            // Cases 1 and 3a: it1 is the only iterator or is before it2.
            leaf_t result_leaf =
                copy_leaf_container(r1, result, (leaf_t)*it1.value);
            art_insert(&result->art, it1.key, (art_val_t)result_leaf);
            art_iterator_next(&it1);
        } else if ((!it1_present && it2_present) || compare_result > 0) {
            // Cases 2 and 3c: it2 is the only iterator or is before it1.
            leaf_t result_leaf =
                copy_leaf_container(r2, result, (leaf_t)*it2.value);
            art_insert(&result->art, it2.key, (art_val_t)result_leaf);
            art_iterator_next(&it2);
        }
    }
    return result;
}

uint64_t roaring64_bitmap_or_cardinality(const roaring64_bitmap_t *r1,
                                         const roaring64_bitmap_t *r2) {
    uint64_t c1 = roaring64_bitmap_get_cardinality(r1);
    uint64_t c2 = roaring64_bitmap_get_cardinality(r2);
    uint64_t inter = roaring64_bitmap_and_cardinality(r1, r2);
    return c1 + c2 - inter;
}

void roaring64_bitmap_or_inplace(roaring64_bitmap_t *r1,
                                 const roaring64_bitmap_t *r2) {
    if (r1 == r2) {
        return;
    }
    art_iterator_t it1 = art_init_iterator(&r1->art, /*first=*/true);
    art_iterator_t it2 = art_init_iterator((art_t *)&r2->art, /*first=*/true);

    while (it1.value != NULL || it2.value != NULL) {
        bool it1_present = it1.value != NULL;
        bool it2_present = it2.value != NULL;

        // Cases:
        // 1. it1_present  && !it2_present -> it1++
        // 2. !it1_present && it2_present  -> add it2, it2++
        // 3. it1_present  && it2_present
        //    a. it1 <  it2 -> it1++
        //    b. it1 == it2 -> it1 | it2, it1++, it2++
        //    c. it1 >  it2 -> add it2, it2++
        int compare_result = 0;
        if (it1_present && it2_present) {
            compare_result = compare_high48(it1.key, it2.key);
            if (compare_result == 0) {
                // Case 3b: iterators at the same high key position.
                leaf_t *leaf1 = (leaf_t *)it1.value;
                leaf_t leaf2 = (leaf_t)*it2.value;
                uint8_t typecode1 = get_typecode(*leaf1);
                container_t *container1 = get_container(r1, *leaf1);
                uint8_t typecode2;
                container_t *container2;
                if (get_typecode(*leaf1) == SHARED_CONTAINER_TYPE) {
                    container2 = container_or(container1, typecode1,
                                              get_container(r2, leaf2),
                                              get_typecode(leaf2), &typecode2);
                } else {
                    container2 = container_ior(container1, typecode1,
                                               get_container(r2, leaf2),
                                               get_typecode(leaf2), &typecode2);
                }
                if (container2 != container1) {
                    container_free(container1, typecode1);
                    replace_container(r1, leaf1, container2, typecode2);
                }
                art_iterator_next(&it1);
                art_iterator_next(&it2);
            }
        }
        if ((it1_present && !it2_present) || compare_result < 0) {
            // Cases 1 and 3a: it1 is the only iterator or is before it2.
            art_iterator_next(&it1);
        } else if ((!it1_present && it2_present) || compare_result > 0) {
            // Cases 2 and 3c: it2 is the only iterator or is before it1.
            leaf_t result_leaf =
                copy_leaf_container(r2, r1, (leaf_t)*it2.value);
            art_iterator_insert(&it1, it2.key, (art_val_t)result_leaf);
            art_iterator_next(&it2);
        }
    }
}

roaring64_bitmap_t *roaring64_bitmap_xor(const roaring64_bitmap_t *r1,
                                         const roaring64_bitmap_t *r2) {
    roaring64_bitmap_t *result = roaring64_bitmap_create();

    art_iterator_t it1 = art_init_iterator((art_t *)&r1->art, /*first=*/true);
    art_iterator_t it2 = art_init_iterator((art_t *)&r2->art, /*first=*/true);

    while (it1.value != NULL || it2.value != NULL) {
        bool it1_present = it1.value != NULL;
        bool it2_present = it2.value != NULL;

        // Cases:
        // 1. it1_present  && !it2_present -> output it1, it1++
        // 2. !it1_present && it2_present  -> output it2, it2++
        // 3. it1_present  && it2_present
        //    a. it1 <  it2 -> output it1, it1++
        //    b. it1 == it2 -> output it1 ^ it2, it1++, it2++
        //    c. it1 >  it2 -> output it2, it2++
        int compare_result = 0;
        if (it1_present && it2_present) {
            compare_result = compare_high48(it1.key, it2.key);
            if (compare_result == 0) {
                // Case 3b: iterators at the same high key position.
                leaf_t leaf1 = (leaf_t)*it1.value;
                leaf_t leaf2 = (leaf_t)*it2.value;
                uint8_t result_typecode;
                container_t *result_container =
                    container_xor(get_container(r1, leaf1), get_typecode(leaf1),
                                  get_container(r2, leaf2), get_typecode(leaf2),
                                  &result_typecode);
                if (container_nonzero_cardinality(result_container,
                                                  result_typecode)) {
                    leaf_t result_leaf = add_container(result, result_container,
                                                       result_typecode);
                    art_insert(&result->art, it1.key, (art_val_t)result_leaf);
                } else {
                    container_free(result_container, result_typecode);
                }
                art_iterator_next(&it1);
                art_iterator_next(&it2);
            }
        }
        if ((it1_present && !it2_present) || compare_result < 0) {
            // Cases 1 and 3a: it1 is the only iterator or is before it2.
            leaf_t result_leaf =
                copy_leaf_container(r1, result, (leaf_t)*it1.value);
            art_insert(&result->art, it1.key, (art_val_t)result_leaf);
            art_iterator_next(&it1);
        } else if ((!it1_present && it2_present) || compare_result > 0) {
            // Cases 2 and 3c: it2 is the only iterator or is before it1.
            leaf_t result_leaf =
                copy_leaf_container(r2, result, (leaf_t)*it2.value);
            art_insert(&result->art, it2.key, (art_val_t)result_leaf);
            art_iterator_next(&it2);
        }
    }
    return result;
}

uint64_t roaring64_bitmap_xor_cardinality(const roaring64_bitmap_t *r1,
                                          const roaring64_bitmap_t *r2) {
    uint64_t c1 = roaring64_bitmap_get_cardinality(r1);
    uint64_t c2 = roaring64_bitmap_get_cardinality(r2);
    uint64_t inter = roaring64_bitmap_and_cardinality(r1, r2);
    return c1 + c2 - 2 * inter;
}

void roaring64_bitmap_xor_inplace(roaring64_bitmap_t *r1,
                                  const roaring64_bitmap_t *r2) {
    assert(r1 != r2);
    art_iterator_t it1 = art_init_iterator(&r1->art, /*first=*/true);
    art_iterator_t it2 = art_init_iterator((art_t *)&r2->art, /*first=*/true);

    while (it1.value != NULL || it2.value != NULL) {
        bool it1_present = it1.value != NULL;
        bool it2_present = it2.value != NULL;

        // Cases:
        // 1.  it1_present && !it2_present -> it1++
        // 2. !it1_present &&  it2_present -> add it2, it2++
        // 3.  it1_present &&  it2_present
        //    a. it1 <  it2 -> it1++
        //    b. it1 == it2 -> it1 ^ it2, it1++, it2++
        //    c. it1 >  it2 -> add it2, it2++
        int compare_result = 0;
        if (it1_present && it2_present) {
            compare_result = compare_high48(it1.key, it2.key);
            if (compare_result == 0) {
                // Case 3b: iterators at the same high key position.
                leaf_t *leaf1 = (leaf_t *)it1.value;
                leaf_t leaf2 = (leaf_t)*it2.value;
                uint8_t typecode1 = get_typecode(*leaf1);
                container_t *container1 = get_container(r1, *leaf1);
                uint8_t typecode2;
                container_t *container2;
                if (typecode1 == SHARED_CONTAINER_TYPE) {
                    container2 = container_xor(container1, typecode1,
                                               get_container(r2, leaf2),
                                               get_typecode(leaf2), &typecode2);
                    if (container2 != container1) {
                        // We only free when doing container_xor, not
                        // container_ixor, as ixor frees the original
                        // internally.
                        container_free(container1, typecode1);
                    }
                } else {
                    container2 = container_ixor(
                        container1, typecode1, get_container(r2, leaf2),
                        get_typecode(leaf2), &typecode2);
                }

                if (!container_nonzero_cardinality(container2, typecode2)) {
                    container_free(container2, typecode2);
                    bool erased = art_iterator_erase(&it1, NULL);
                    assert(erased);
                    (void)erased;
                    remove_container(r1, *leaf1);
                } else {
                    if (container2 != container1) {
                        replace_container(r1, leaf1, container2, typecode2);
                    }
                    // Only advance the iterator if we didn't delete the
                    // leaf, as erasing advances by itself.
                    art_iterator_next(&it1);
                }
                art_iterator_next(&it2);
            }
        }
        if ((it1_present && !it2_present) || compare_result < 0) {
            // Cases 1 and 3a: it1 is the only iterator or is before it2.
            art_iterator_next(&it1);
        } else if ((!it1_present && it2_present) || compare_result > 0) {
            // Cases 2 and 3c: it2 is the only iterator or is before it1.
            leaf_t result_leaf =
                copy_leaf_container(r2, r1, (leaf_t)*it2.value);
            if (it1_present) {
                art_iterator_insert(&it1, it2.key, (art_val_t)result_leaf);
                art_iterator_next(&it1);
            } else {
                art_insert(&r1->art, it2.key, (art_val_t)result_leaf);
            }
            art_iterator_next(&it2);
        }
    }
}

roaring64_bitmap_t *roaring64_bitmap_andnot(const roaring64_bitmap_t *r1,
                                            const roaring64_bitmap_t *r2) {
    roaring64_bitmap_t *result = roaring64_bitmap_create();

    art_iterator_t it1 = art_init_iterator((art_t *)&r1->art, /*first=*/true);
    art_iterator_t it2 = art_init_iterator((art_t *)&r2->art, /*first=*/true);

    while (it1.value != NULL) {
        // Cases:
        // 1. it1_present && !it2_present -> output it1, it1++
        // 2. it1_present && it2_present
        //    a. it1 <  it2 -> output it1, it1++
        //    b. it1 == it2 -> output it1 - it2, it1++, it2++
        //    c. it1 >  it2 -> it2++
        bool it2_present = it2.value != NULL;
        int compare_result = 0;
        if (it2_present) {
            compare_result = compare_high48(it1.key, it2.key);
            if (compare_result == 0) {
                // Case 2b: iterators at the same high key position.
                leaf_t *leaf1 = (leaf_t *)it1.value;
                leaf_t leaf2 = (leaf_t)*it2.value;
                uint8_t result_typecode;
                container_t *result_container = container_andnot(
                    get_container(r1, *leaf1), get_typecode(*leaf1),
                    get_container(r2, leaf2), get_typecode(leaf2),
                    &result_typecode);

                if (container_nonzero_cardinality(result_container,
                                                  result_typecode)) {
                    leaf_t result_leaf = add_container(result, result_container,
                                                       result_typecode);
                    art_insert(&result->art, it1.key, (art_val_t)result_leaf);
                } else {
                    container_free(result_container, result_typecode);
                }
                art_iterator_next(&it1);
                art_iterator_next(&it2);
            }
        }
        if (!it2_present || compare_result < 0) {
            // Cases 1 and 2a: it1 is the only iterator or is before it2.
            leaf_t result_leaf =
                copy_leaf_container(r1, result, (leaf_t)*it1.value);
            art_insert(&result->art, it1.key, (art_val_t)result_leaf);
            art_iterator_next(&it1);
        } else if (compare_result > 0) {
            // Case 2c: it1 is after it2.
            art_iterator_next(&it2);
        }
    }
    return result;
}

uint64_t roaring64_bitmap_andnot_cardinality(const roaring64_bitmap_t *r1,
                                             const roaring64_bitmap_t *r2) {
    uint64_t c1 = roaring64_bitmap_get_cardinality(r1);
    uint64_t inter = roaring64_bitmap_and_cardinality(r1, r2);
    return c1 - inter;
}

void roaring64_bitmap_andnot_inplace(roaring64_bitmap_t *r1,
                                     const roaring64_bitmap_t *r2) {
    art_iterator_t it1 = art_init_iterator(&r1->art, /*first=*/true);
    art_iterator_t it2 = art_init_iterator((art_t *)&r2->art, /*first=*/true);

    while (it1.value != NULL) {
        // Cases:
        // 1. it1_present && !it2_present -> it1++
        // 2. it1_present &&  it2_present
        //    a. it1 <  it2 -> it1++
        //    b. it1 == it2 -> it1 - it2, it1++, it2++
        //    c. it1 >  it2 -> it2++
        bool it2_present = it2.value != NULL;
        int compare_result = 0;
        if (it2_present) {
            compare_result = compare_high48(it1.key, it2.key);
            if (compare_result == 0) {
                // Case 2b: iterators at the same high key position.
                leaf_t *leaf1 = (leaf_t *)it1.value;
                leaf_t leaf2 = (leaf_t)*it2.value;
                uint8_t typecode1 = get_typecode(*leaf1);
                container_t *container1 = get_container(r1, *leaf1);
                uint8_t typecode2;
                container_t *container2;
                if (typecode1 == SHARED_CONTAINER_TYPE) {
                    container2 = container_andnot(
                        container1, typecode1, get_container(r2, leaf2),
                        get_typecode(leaf2), &typecode2);
                    if (container2 != container1) {
                        // We only free when doing container_andnot, not
                        // container_iandnot, as iandnot frees the original
                        // internally.
                        container_free(container1, typecode1);
                    }
                } else {
                    container2 = container_iandnot(
                        container1, typecode1, get_container(r2, leaf2),
                        get_typecode(leaf2), &typecode2);
                }

                if (!container_nonzero_cardinality(container2, typecode2)) {
                    container_free(container2, typecode2);
                    bool erased = art_iterator_erase(&it1, NULL);
                    assert(erased);
                    (void)erased;
                    remove_container(r1, *leaf1);
                } else {
                    if (container2 != container1) {
                        replace_container(r1, leaf1, container2, typecode2);
                    }
                    // Only advance the iterator if we didn't delete the
                    // leaf, as erasing advances by itself.
                    art_iterator_next(&it1);
                }
                art_iterator_next(&it2);
            }
        }
        if (!it2_present || compare_result < 0) {
            // Cases 1 and 2a: it1 is the only iterator or is before it2.
            art_iterator_next(&it1);
        } else if (compare_result > 0) {
            // Case 2c: it1 is after it2.
            art_iterator_next(&it2);
        }
    }
}

/**
 * Flips the leaf at high48 in the range [min, max), adding the result to
 * `r2`. If the high48 key is not found in `r1`, a new container is created.
 */
static void roaring64_flip_leaf(const roaring64_bitmap_t *r1,
                                roaring64_bitmap_t *r2, uint8_t high48[],
                                uint32_t min, uint32_t max) {
    leaf_t *leaf1 = (leaf_t *)art_find(&r1->art, high48);
    uint8_t typecode2;
    container_t *container2;
    if (leaf1 == NULL) {
        // No container at this key, create a full container.
        container2 = container_range_of_ones(min, max, &typecode2);
    } else if (min == 0 && max > 0xFFFF) {
        // Flip whole container.
        container2 = container_not(get_container(r1, *leaf1),
                                   get_typecode(*leaf1), &typecode2);
    } else {
        // Partially flip a container.
        container2 =
            container_not_range(get_container(r1, *leaf1), get_typecode(*leaf1),
                                min, max, &typecode2);
    }
    if (container_nonzero_cardinality(container2, typecode2)) {
        leaf_t leaf2 = add_container(r2, container2, typecode2);
        art_insert(&r2->art, high48, (art_val_t)leaf2);
    } else {
        container_free(container2, typecode2);
    }
}

/**
 * Flips the leaf at high48 in the range [min, max). If the high48 key is
 * not found in the bitmap, a new container is created. Deletes the leaf and
 * associated container if the negation results in an empty range.
 */
static void roaring64_flip_leaf_inplace(roaring64_bitmap_t *r, uint8_t high48[],
                                        uint32_t min, uint32_t max) {
    leaf_t *leaf = (leaf_t *)art_find(&r->art, high48);
    container_t *container2;
    uint8_t typecode2;
    if (leaf == NULL) {
        // No container at this key, insert a full container.
        container2 = container_range_of_ones(min, max, &typecode2);
        leaf_t new_leaf = add_container(r, container2, typecode2);
        art_insert(&r->art, high48, (art_val_t)new_leaf);
        return;
    }

    if (min == 0 && max > 0xFFFF) {
        // Flip whole container.
        container2 = container_inot(get_container(r, *leaf),
                                    get_typecode(*leaf), &typecode2);
    } else {
        // Partially flip a container.
        container2 = container_inot_range(
            get_container(r, *leaf), get_typecode(*leaf), min, max, &typecode2);
    }

    if (container_nonzero_cardinality(container2, typecode2)) {
        replace_container(r, leaf, container2, typecode2);
    } else {
        bool erased = art_erase(&r->art, high48, NULL);
        assert(erased);
        (void)erased;
        container_free(container2, typecode2);
        remove_container(r, *leaf);
    }
}

roaring64_bitmap_t *roaring64_bitmap_flip(const roaring64_bitmap_t *r,
                                          uint64_t min, uint64_t max) {
    if (min >= max) {
        return roaring64_bitmap_copy(r);
    }
    return roaring64_bitmap_flip_closed(r, min, max - 1);
}

roaring64_bitmap_t *roaring64_bitmap_flip_closed(const roaring64_bitmap_t *r1,
                                                 uint64_t min, uint64_t max) {
    if (min > max) {
        return roaring64_bitmap_copy(r1);
    }
    uint8_t min_high48_key[ART_KEY_BYTES];
    uint16_t min_low16 = split_key(min, min_high48_key);
    uint8_t max_high48_key[ART_KEY_BYTES];
    uint16_t max_low16 = split_key(max, max_high48_key);
    uint64_t min_high48_bits = (min & 0xFFFFFFFFFFFF0000ULL) >> 16;
    uint64_t max_high48_bits = (max & 0xFFFFFFFFFFFF0000ULL) >> 16;

    roaring64_bitmap_t *r2 = roaring64_bitmap_create();
    art_iterator_t it = art_init_iterator((art_t *)&r1->art, /*first=*/true);

    // Copy the containers before min unchanged.
    while (it.value != NULL && compare_high48(it.key, min_high48_key) < 0) {
        leaf_t leaf1 = (leaf_t)*it.value;
        uint8_t typecode2 = get_typecode(leaf1);
        container_t *container2 = get_copy_of_container(
            get_container(r1, leaf1), &typecode2, /*copy_on_write=*/false);
        leaf_t leaf2 = add_container(r2, container2, typecode2);
        art_insert(&r2->art, it.key, (art_val_t)leaf2);
        art_iterator_next(&it);
    }

    // Flip the range (including non-existent containers!) between min and
    // max.
    for (uint64_t high48_bits = min_high48_bits; high48_bits <= max_high48_bits;
         high48_bits++) {
        uint8_t current_high48_key[ART_KEY_BYTES];
        split_key(high48_bits << 16, current_high48_key);

        uint32_t min_container = 0;
        if (high48_bits == min_high48_bits) {
            min_container = min_low16;
        }
        uint32_t max_container = 0xFFFF + 1;  // Exclusive range.
        if (high48_bits == max_high48_bits) {
            max_container = max_low16 + 1;  // Exclusive.
        }

        roaring64_flip_leaf(r1, r2, current_high48_key, min_container,
                            max_container);
    }

    // Copy the containers after max unchanged.
    it = art_upper_bound((art_t *)&r1->art, max_high48_key);
    while (it.value != NULL) {
        leaf_t leaf1 = (leaf_t)*it.value;
        uint8_t typecode2 = get_typecode(leaf1);
        container_t *container2 = get_copy_of_container(
            get_container(r1, leaf1), &typecode2, /*copy_on_write=*/false);
        leaf_t leaf2 = add_container(r2, container2, typecode2);
        art_insert(&r2->art, it.key, (art_val_t)leaf2);
        art_iterator_next(&it);
    }

    return r2;
}

void roaring64_bitmap_flip_inplace(roaring64_bitmap_t *r, uint64_t min,
                                   uint64_t max) {
    if (min >= max) {
        return;
    }
    roaring64_bitmap_flip_closed_inplace(r, min, max - 1);
}

void roaring64_bitmap_flip_closed_inplace(roaring64_bitmap_t *r, uint64_t min,
                                          uint64_t max) {
    if (min > max) {
        return;
    }
    uint16_t min_low16 = (uint16_t)min;
    uint16_t max_low16 = (uint16_t)max;
    uint64_t min_high48_bits = (min & 0xFFFFFFFFFFFF0000ULL) >> 16;
    uint64_t max_high48_bits = (max & 0xFFFFFFFFFFFF0000ULL) >> 16;

    // Flip the range (including non-existent containers!) between min and
    // max.
    for (uint64_t high48_bits = min_high48_bits; high48_bits <= max_high48_bits;
         high48_bits++) {
        uint8_t current_high48_key[ART_KEY_BYTES];
        split_key(high48_bits << 16, current_high48_key);

        uint32_t min_container = 0;
        if (high48_bits == min_high48_bits) {
            min_container = min_low16;
        }
        uint32_t max_container = 0xFFFF + 1;  // Exclusive range.
        if (high48_bits == max_high48_bits) {
            max_container = max_low16 + 1;  // Exclusive.
        }

        roaring64_flip_leaf_inplace(r, current_high48_key, min_container,
                                    max_container);
    }
}

roaring64_bitmap_t *roaring64_bitmap_add_offset_signed(
    const roaring64_bitmap_t *r, bool positive, uint64_t offset) {
    if (offset == 0) {
        return roaring64_bitmap_copy(r);
    }

    roaring64_bitmap_t *answer = roaring64_bitmap_create();

    // Decompose the offset into a signed container-level shift and an
    // intra-container shift. For negative offsets the low 16 bits wrap: e.g.
    // -1 = container_offset(-1) + in_offset(0xffff), because shifting by -1
    // container is a shift of -0x1_0000, so we need to shift up within
    // containers to get back to -1
    uint16_t low16 = (uint16_t)offset;
    int64_t container_offset;
    uint16_t in_offset;
    if (positive) {
        container_offset = (int64_t)(offset >> 16);
        in_offset = low16;
    } else if (low16 == 0) {
        container_offset = -(int64_t)(offset >> 16);
        in_offset = 0;
    } else {
        container_offset = -(int64_t)(offset >> 16) - 1;
        in_offset = (uint16_t)-low16;
    }

    art_iterator_t it = art_init_iterator((art_t *)&r->art, /*first=*/true);

    if (in_offset == 0) {
        while (it.value != NULL) {
            leaf_t leaf = (leaf_t)*it.value;
            int64_t k =
                (int64_t)(combine_key(it.key, 0) >> 16) + container_offset;
            if ((uint64_t)k < (uint64_t)1 << 48) {
                uint8_t new_high48[ART_KEY_BYTES];
                split_key((uint64_t)k << 16, new_high48);
                uint8_t typecode = get_typecode(leaf);
                container_t *container =
                    get_copy_of_container(get_container(r, leaf), &typecode,
                                          /*copy_on_write=*/false);
                leaf_t new_leaf = add_container(answer, container, typecode);
                art_insert(&answer->art, new_high48, (art_val_t)new_leaf);
            }
            art_iterator_next(&it);
        }
        return answer;
    }

    // Track the most recently inserted hi container so that the next
    // iteration's lo can merge with it without re-searching the ART.
    leaf_t *prev_hi_leaf = NULL;
    int64_t prev_hi_k = -1;

    while (it.value != NULL) {
        leaf_t leaf = (leaf_t)*it.value;
        int64_t k = (int64_t)(combine_key(it.key, 0) >> 16) + container_offset;

        container_t *lo = NULL, *hi = NULL;
        container_t **lo_ptr = NULL, **hi_ptr = NULL;

        if ((uint64_t)k < (uint64_t)1 << 48) {
            lo_ptr = &lo;
        }
        if ((uint64_t)(k + 1) < (uint64_t)1 << 48) {
            hi_ptr = &hi;
        }
        if (lo_ptr == NULL && hi_ptr == NULL) {
            art_iterator_next(&it);
            continue;
        }

        uint8_t typecode = get_typecode(leaf);
        const container_t *c =
            container_unwrap_shared(get_container(r, leaf), &typecode);
        container_add_offset(c, typecode, lo_ptr, hi_ptr, in_offset);

        if (lo != NULL) {
            if (prev_hi_leaf != NULL && prev_hi_k == k) {
                uint8_t existing_type = get_typecode(*prev_hi_leaf);
                container_t *existing_c = get_container(answer, *prev_hi_leaf);
                uint8_t merged_type;
                container_t *merged_c = container_ior(
                    existing_c, existing_type, lo, typecode, &merged_type);
                if (merged_c != existing_c) {
                    container_free(existing_c, existing_type);
                }
                replace_container(answer, prev_hi_leaf, merged_c, merged_type);
                container_free(lo, typecode);
            } else {
                uint8_t lo_high48[ART_KEY_BYTES];
                split_key((uint64_t)k << 16, lo_high48);
                leaf_t new_leaf = add_container(answer, lo, typecode);
                art_insert(&answer->art, lo_high48, (art_val_t)new_leaf);
            }
        }

        prev_hi_leaf = NULL;
        if (hi != NULL) {
            uint8_t hi_high48[ART_KEY_BYTES];
            split_key((uint64_t)(k + 1) << 16, hi_high48);
            leaf_t new_leaf = add_container(answer, hi, typecode);
            prev_hi_leaf = (leaf_t *)art_insert(&answer->art, hi_high48,
                                                (art_val_t)new_leaf);
            prev_hi_k = k + 1;
        }

        art_iterator_next(&it);
    }

    // Repair containers (e.g., convert low-cardinality bitset containers to
    // array containers after lazy union operations).
    art_iterator_t repair_it = art_init_iterator(&answer->art, /*first=*/true);
    while (repair_it.value != NULL) {
        leaf_t *leaf_ptr = (leaf_t *)repair_it.value;
        uint8_t typecode = get_typecode(*leaf_ptr);
        container_t *repaired = container_repair_after_lazy(
            get_container(answer, *leaf_ptr), &typecode);
        replace_container(answer, leaf_ptr, repaired, typecode);
        art_iterator_next(&repair_it);
    }

    return answer;
}

// Returns the number of distinct high 32-bit entries in the bitmap.
static inline uint64_t count_high32(const roaring64_bitmap_t *r) {
    art_iterator_t it = art_init_iterator((art_t *)&r->art, /*first=*/true);
    uint64_t high32_count = 0;
    uint32_t prev_high32 = 0;
    while (it.value != NULL) {
        uint32_t current_high32 = (uint32_t)(combine_key(it.key, 0) >> 32);
        if (high32_count == 0 || prev_high32 != current_high32) {
            high32_count++;
            prev_high32 = current_high32;
        }
        art_iterator_next(&it);
    }
    return high32_count;
}

// Frees the (32-bit!) bitmap without freeing the containers.
static inline void roaring_bitmap_free_without_containers(roaring_bitmap_t *r) {
    ra_clear_without_containers(&r->high_low_container);
    roaring_free(r);
}

size_t roaring64_bitmap_portable_size_in_bytes(const roaring64_bitmap_t *r) {
    // https://github.com/RoaringBitmap/RoaringFormatSpec#extension-for-64-bit-implementations
    size_t size = 0;

    // Write as uint64 the distinct number of "buckets", where a bucket is
    // defined as the most significant 32 bits of an element.
    uint64_t high32_count;
    size += sizeof(high32_count);

    art_iterator_t it = art_init_iterator((art_t *)&r->art, /*first=*/true);
    uint32_t prev_high32 = 0;
    roaring_bitmap_t *bitmap32 = NULL;

    // Iterate through buckets ordered by increasing keys.
    while (it.value != NULL) {
        uint32_t current_high32 = (uint32_t)(combine_key(it.key, 0) >> 32);
        if (bitmap32 == NULL || prev_high32 != current_high32) {
            if (bitmap32 != NULL) {
                // Write as uint32 the most significant 32 bits of the
                // bucket.
                size += sizeof(prev_high32);

                // Write the 32-bit Roaring bitmaps representing the least
                // significant bits of a set of elements.
                size += roaring_bitmap_portable_size_in_bytes(bitmap32);
                roaring_bitmap_free_without_containers(bitmap32);
            }

            // Start a new 32-bit bitmap with the current high 32 bits.
            art_iterator_t it2 = it;
            uint32_t containers_with_high32 = 0;
            while (it2.value != NULL && (uint32_t)(combine_key(it2.key, 0) >>
                                                   32) == current_high32) {
                containers_with_high32++;
                art_iterator_next(&it2);
            }
            bitmap32 =
                roaring_bitmap_create_with_capacity(containers_with_high32);

            prev_high32 = current_high32;
        }
        leaf_t leaf = (leaf_t)*it.value;
        ra_append(&bitmap32->high_low_container,
                  (uint16_t)(current_high32 >> 16), get_container(r, leaf),
                  get_typecode(leaf));
        art_iterator_next(&it);
    }

    if (bitmap32 != NULL) {
        // Write as uint32 the most significant 32 bits of the bucket.
        size += sizeof(prev_high32);

        // Write the 32-bit Roaring bitmaps representing the least
        // significant bits of a set of elements.
        size += roaring_bitmap_portable_size_in_bytes(bitmap32);
        roaring_bitmap_free_without_containers(bitmap32);
    }

    return size;
}

size_t roaring64_bitmap_portable_serialize(const roaring64_bitmap_t *r,
                                           char *buf) {
    // https://github.com/RoaringBitmap/RoaringFormatSpec#extension-for-64-bit-implementations
    if (buf == NULL) {
        return 0;
    }
    const char *initial_buf = buf;

    // Write as uint64 the distinct number of "buckets", where a bucket is
    // defined as the most significant 32 bits of an element.
    uint64_t high32_count = count_high32(r);
    memcpy(buf, &high32_count, sizeof(high32_count));
    buf += sizeof(high32_count);

    art_iterator_t it = art_init_iterator((art_t *)&r->art, /*first=*/true);
    uint32_t prev_high32 = 0;
    roaring_bitmap_t *bitmap32 = NULL;

    // Iterate through buckets ordered by increasing keys.
    while (it.value != NULL) {
        uint64_t current_high48 = combine_key(it.key, 0);
        uint32_t current_high32 = (uint32_t)(current_high48 >> 32);
        if (bitmap32 == NULL || prev_high32 != current_high32) {
            if (bitmap32 != NULL) {
                // Write as uint32 the most significant 32 bits of the
                // bucket.
                memcpy(buf, &prev_high32, sizeof(prev_high32));
                buf += sizeof(prev_high32);

                // Write the 32-bit Roaring bitmaps representing the least
                // significant bits of a set of elements.
                buf += roaring_bitmap_portable_serialize(bitmap32, buf);
                roaring_bitmap_free_without_containers(bitmap32);
            }

            // Start a new 32-bit bitmap with the current high 32 bits.
            art_iterator_t it2 = it;
            uint32_t containers_with_high32 = 0;
            while (it2.value != NULL &&
                   (uint32_t)combine_key(it2.key, 0) == current_high32) {
                containers_with_high32++;
                art_iterator_next(&it2);
            }
            bitmap32 =
                roaring_bitmap_create_with_capacity(containers_with_high32);

            prev_high32 = current_high32;
        }
        leaf_t leaf = (leaf_t)*it.value;
        ra_append(&bitmap32->high_low_container,
                  (uint16_t)(current_high48 >> 16), get_container(r, leaf),
                  get_typecode(leaf));
        art_iterator_next(&it);
    }

    if (bitmap32 != NULL) {
        // Write as uint32 the most significant 32 bits of the bucket.
        memcpy(buf, &prev_high32, sizeof(prev_high32));
        buf += sizeof(prev_high32);

        // Write the 32-bit Roaring bitmaps representing the least
        // significant bits of a set of elements.
        buf += roaring_bitmap_portable_serialize(bitmap32, buf);
        roaring_bitmap_free_without_containers(bitmap32);
    }

    return buf - initial_buf;
}

size_t roaring64_bitmap_portable_deserialize_size(const char *buf,
                                                  size_t maxbytes) {
    // https://github.com/RoaringBitmap/RoaringFormatSpec#extension-for-64-bit-implementations
    if (buf == NULL) {
        return 0;
    }
    size_t read_bytes = 0;

    // Read as uint64 the distinct number of "buckets", where a bucket is
    // defined as the most significant 32 bits of an element.
    uint64_t buckets;
    if (read_bytes + sizeof(buckets) > maxbytes) {
        return 0;
    }
    memcpy(&buckets, buf, sizeof(buckets));
    buf += sizeof(buckets);
    read_bytes += sizeof(buckets);

    // Buckets should be 32 bits with 4 bits of zero padding.
    if (buckets > UINT32_MAX) {
        return 0;
    }

    // Iterate through buckets ordered by increasing keys.
    for (uint64_t bucket = 0; bucket < buckets; ++bucket) {
        // Read as uint32 the most significant 32 bits of the bucket.
        uint32_t high32;
        if (read_bytes + sizeof(high32) > maxbytes) {
            return 0;
        }
        buf += sizeof(high32);
        read_bytes += sizeof(high32);

        // Read the 32-bit Roaring bitmaps representing the least
        // significant bits of a set of elements.
        size_t bitmap32_size = roaring_bitmap_portable_deserialize_size(
            buf, maxbytes - read_bytes);
        if (bitmap32_size == 0) {
            return 0;
        }
        buf += bitmap32_size;
        read_bytes += bitmap32_size;
    }
    return read_bytes;
}

roaring64_bitmap_t *roaring64_bitmap_portable_deserialize_safe(
    const char *buf, size_t maxbytes) {
    // https://github.com/RoaringBitmap/RoaringFormatSpec#extension-for-64-bit-implementations
    if (buf == NULL) {
        return NULL;
    }
    size_t read_bytes = 0;

    // Read as uint64 the distinct number of "buckets", where a bucket is
    // defined as the most significant 32 bits of an element.
    uint64_t buckets;
    if (read_bytes + sizeof(buckets) > maxbytes) {
        return NULL;
    }
    memcpy(&buckets, buf, sizeof(buckets));
    buf += sizeof(buckets);
    read_bytes += sizeof(buckets);

    // Buckets should be 32 bits with 4 bits of zero padding.
    if (buckets > UINT32_MAX) {
        return NULL;
    }

    roaring64_bitmap_t *r = roaring64_bitmap_create();
    // Iterate through buckets ordered by increasing keys.
    int64_t previous_high32 = -1;
    for (uint64_t bucket = 0; bucket < buckets; ++bucket) {
        // Read as uint32 the most significant 32 bits of the bucket.
        uint32_t high32;
        if (read_bytes + sizeof(high32) > maxbytes) {
            roaring64_bitmap_free(r);
            return NULL;
        }
        memcpy(&high32, buf, sizeof(high32));
        buf += sizeof(high32);
        read_bytes += sizeof(high32);
        // High 32 bits must be strictly increasing.
        if (high32 <= previous_high32) {
            roaring64_bitmap_free(r);
            return NULL;
        }
        previous_high32 = high32;

        // Read the 32-bit Roaring bitmaps representing the least
        // significant bits of a set of elements.
        size_t bitmap32_size = roaring_bitmap_portable_deserialize_size(
            buf, maxbytes - read_bytes);
        if (bitmap32_size == 0) {
            roaring64_bitmap_free(r);
            return NULL;
        }

        roaring_bitmap_t *bitmap32 = roaring_bitmap_portable_deserialize_safe(
            buf, maxbytes - read_bytes);
        if (bitmap32 == NULL) {
            roaring64_bitmap_free(r);
            return NULL;
        }
        buf += bitmap32_size;
        read_bytes += bitmap32_size;

        // While we don't attempt to validate much, we must ensure that there
        // is no duplication in the high 48 bits - inserting into the ART
        // assumes (or UB) no duplicate keys. The top 32 bits must be unique
        // because we check for strict increasing values of  high32, but we
        // must also ensure the top 16 bits within each 32-bit bitmap are also
        // at least unique (we ensure they're strictly increasing as well,
        // which they must be for a _valid_ bitmap, since it's cheaper to check)
        int32_t last_bitmap_key = -1;
        for (int i = 0; i < bitmap32->high_low_container.size; i++) {
            uint16_t key = bitmap32->high_low_container.keys[i];
            if (key <= last_bitmap_key) {
                roaring_bitmap_free(bitmap32);
                roaring64_bitmap_free(r);
                return NULL;
            }
            last_bitmap_key = key;
        }

        // Insert all containers of the 32-bit bitmap into the 64-bit bitmap.
        move_from_roaring32_offset(r, bitmap32, high32);
        roaring_bitmap_free(bitmap32);
    }
    return r;
}

// Returns an "element count" for the given container. This has a different
// meaning for each container type, but the purpose is the minimal information
// required to serialize the container metadata.
static inline uint32_t container_get_element_count(const container_t *c,
                                                   uint8_t typecode) {
    switch (typecode) {
        case BITSET_CONTAINER_TYPE: {
            return ((bitset_container_t *)c)->cardinality;
        }
        case ARRAY_CONTAINER_TYPE: {
            return ((array_container_t *)c)->cardinality;
        }
        case RUN_CONTAINER_TYPE: {
            return ((run_container_t *)c)->n_runs;
        }
        default: {
            assert(false);
            roaring_unreachable;
            return 0;
        }
    }
}

static inline size_t container_get_frozen_size(const container_t *c,
                                               uint8_t typecode) {
    switch (typecode) {
        case BITSET_CONTAINER_TYPE: {
            return BITSET_CONTAINER_SIZE_IN_WORDS * sizeof(uint64_t);
        }
        case ARRAY_CONTAINER_TYPE: {
            return container_get_element_count(c, typecode) * sizeof(uint16_t);
        }
        case RUN_CONTAINER_TYPE: {
            return container_get_element_count(c, typecode) * sizeof(rle16_t);
        }
        default: {
            assert(false);
            roaring_unreachable;
            return 0;
        }
    }
}

uint64_t align_size(uint64_t size, uint64_t alignment) {
    return (size + alignment - 1) & ~(alignment - 1);
}

size_t roaring64_bitmap_frozen_size_in_bytes(const roaring64_bitmap_t *r) {
    if (!is_shrunken(r)) {
        return 0;
    }
    // Flags.
    uint64_t size = sizeof(r->flags);
    // Container count.
    size += sizeof(r->capacity);
    // Container element counts.
    size += r->capacity * sizeof(uint16_t);
    // Total container sizes.
    size += 3 * sizeof(uint64_t);
    // ART (8 byte aligned).
    size = align_size(size, 8);
    size += art_size_in_bytes(&r->art);

    uint64_t total_sizes[4] =
        CROARING_ZERO_INITIALIZER;  // Indexed by typecode.
    art_iterator_t it = art_init_iterator((art_t *)&r->art, /*first=*/true);
    while (it.value != NULL) {
        leaf_t leaf = (leaf_t)*it.value;
        uint8_t typecode = get_typecode(leaf);
        total_sizes[typecode] +=
            container_get_frozen_size(get_container(r, leaf), typecode);
        art_iterator_next(&it);
    }
    // Containers (aligned).
    size = align_size(size, CROARING_BITSET_ALIGNMENT);
    size += total_sizes[BITSET_CONTAINER_TYPE];
    size = align_size(size, alignof(rle16_t));
    size += total_sizes[ARRAY_CONTAINER_TYPE];
    size = align_size(size, alignof(uint16_t));
    size += total_sizes[RUN_CONTAINER_TYPE];
    // Padding to make overall size a multiple of required alignment.
    size = align_size(size, CROARING_BITSET_ALIGNMENT);
    return size;
}

static inline void container_frozen_serialize(const container_t *container,
                                              uint8_t typecode,
                                              uint64_t **bitsets,
                                              uint16_t **arrays,
                                              rle16_t **runs) {
    size_t size = container_get_frozen_size(container, typecode);
    switch (typecode) {
        case BITSET_CONTAINER_TYPE: {
            bitset_container_t *bitset = (bitset_container_t *)container;
            memcpy(*bitsets, bitset->words, size);
            *bitsets += BITSET_CONTAINER_SIZE_IN_WORDS;
            break;
        }
        case ARRAY_CONTAINER_TYPE: {
            array_container_t *array = (array_container_t *)container;
            memcpy(*arrays, array->array, size);
            *arrays += container_get_element_count(container, typecode);
            break;
        }
        case RUN_CONTAINER_TYPE: {
            run_container_t *run = (run_container_t *)container;
            memcpy(*runs, run->runs, size);
            *runs += container_get_element_count(container, typecode);
            break;
        }
        default: {
            assert(false);
            roaring_unreachable;
        }
    }
}

static inline char *pad_align(char *buf, const char *initial_buf,
                              size_t alignment) {
    uint64_t buf_size = buf - initial_buf;
    uint64_t pad = align_size(buf_size, alignment) - buf_size;
    memset(buf, 0, pad);
    return buf + pad;
}

size_t roaring64_bitmap_frozen_serialize(const roaring64_bitmap_t *r,
                                         char *buf) {
    if (buf == NULL) {
        return 0;
    }
    if (!is_shrunken(r)) {
        return 0;
    }
    const char *initial_buf = buf;

    // Flags.
    memcpy(buf, &r->flags, sizeof(r->flags));
    buf += sizeof(r->flags);

    // Container count.
    memcpy(buf, &r->capacity, sizeof(r->capacity));
    buf += sizeof(r->capacity);

    // Container element counts.
    uint64_t total_sizes[4] =
        CROARING_ZERO_INITIALIZER;  // Indexed by typecode.
    art_iterator_t it = art_init_iterator((art_t *)&r->art, /*first=*/true);
    while (it.value != NULL) {
        leaf_t leaf = (leaf_t)*it.value;
        uint8_t typecode = get_typecode(leaf);
        container_t *container = get_container(r, leaf);

        uint32_t elem_count = container_get_element_count(container, typecode);
        uint16_t compressed_elem_count = (uint16_t)(elem_count - 1);
        memcpy(buf, &compressed_elem_count, sizeof(compressed_elem_count));
        buf += sizeof(compressed_elem_count);

        total_sizes[typecode] += container_get_frozen_size(container, typecode);
        art_iterator_next(&it);
    }

    // Total container sizes.
    memcpy(buf, &(total_sizes[BITSET_CONTAINER_TYPE]), sizeof(uint64_t));
    buf += sizeof(uint64_t);
    memcpy(buf, &(total_sizes[RUN_CONTAINER_TYPE]), sizeof(uint64_t));
    buf += sizeof(uint64_t);
    memcpy(buf, &(total_sizes[ARRAY_CONTAINER_TYPE]), sizeof(uint64_t));
    buf += sizeof(uint64_t);

    // ART.
    buf = pad_align(buf, initial_buf, 8);
    buf += art_serialize(&r->art, buf);

    // Containers (aligned).
    // Runs before arrays as run elements are larger than array elements and
    // smaller than bitset elements.
    buf = pad_align(buf, initial_buf, CROARING_BITSET_ALIGNMENT);
    uint64_t *bitsets = (uint64_t *)buf;
    buf += total_sizes[BITSET_CONTAINER_TYPE];
    buf = pad_align(buf, initial_buf, alignof(rle16_t));
    rle16_t *runs = (rle16_t *)buf;
    buf += total_sizes[RUN_CONTAINER_TYPE];
    buf = pad_align(buf, initial_buf, alignof(uint16_t));
    uint16_t *arrays = (uint16_t *)buf;
    buf += total_sizes[ARRAY_CONTAINER_TYPE];

    it = art_init_iterator((art_t *)&r->art, /*first=*/true);
    while (it.value != NULL) {
        leaf_t leaf = (leaf_t)*it.value;
        uint8_t typecode = get_typecode(leaf);
        container_t *container = get_container(r, leaf);
        container_frozen_serialize(container, typecode, &bitsets, &arrays,
                                   &runs);
        art_iterator_next(&it);
    }

    // Padding to make overall size a multiple of required alignment.
    buf = pad_align(buf, initial_buf, CROARING_BITSET_ALIGNMENT);

    return buf - initial_buf;
}

static container_t *container_frozen_view(uint8_t typecode, uint32_t elem_count,
                                          const uint64_t **bitsets,
                                          const uint16_t **arrays,
                                          const rle16_t **runs) {
    switch (typecode) {
        case BITSET_CONTAINER_TYPE: {
            bitset_container_t *c = (bitset_container_t *)roaring_malloc(
                sizeof(bitset_container_t));
            c->cardinality = elem_count;
            c->words = (uint64_t *)*bitsets;
            *bitsets += BITSET_CONTAINER_SIZE_IN_WORDS;
            return (container_t *)c;
        }
        case ARRAY_CONTAINER_TYPE: {
            array_container_t *c =
                (array_container_t *)roaring_malloc(sizeof(array_container_t));
            c->cardinality = elem_count;
            c->capacity = elem_count;
            c->array = (uint16_t *)*arrays;
            *arrays += elem_count;
            return (container_t *)c;
        }
        case RUN_CONTAINER_TYPE: {
            run_container_t *c =
                (run_container_t *)roaring_malloc(sizeof(run_container_t));
            c->n_runs = elem_count;
            c->capacity = elem_count;
            c->runs = (rle16_t *)*runs;
            *runs += elem_count;
            return (container_t *)c;
        }
        default: {
            assert(false);
            roaring_unreachable;
            return NULL;
        }
    }
}

roaring64_bitmap_t *roaring64_bitmap_frozen_view(const char *buf,
                                                 size_t maxbytes) {
    if (buf == NULL) {
        return NULL;
    }
    if ((uintptr_t)buf % CROARING_BITSET_ALIGNMENT != 0) {
        return NULL;
    }

    roaring64_bitmap_t *r = roaring64_bitmap_create();

    // Flags.
    if (maxbytes < sizeof(r->flags)) {
        roaring64_bitmap_free(r);
        return NULL;
    }
    memcpy(&r->flags, buf, sizeof(r->flags));
    buf += sizeof(r->flags);
    maxbytes -= sizeof(r->flags);
    r->flags |= ROARING_FLAG_FROZEN;

    // Container count.
    if (maxbytes < sizeof(r->capacity)) {
        roaring64_bitmap_free(r);
        return NULL;
    }
    memcpy(&r->capacity, buf, sizeof(r->capacity));
    buf += sizeof(r->capacity);
    maxbytes -= sizeof(r->capacity);

    r->containers =
        (container_t **)roaring_malloc(r->capacity * sizeof(container_t *));

    // Container element counts.
    if (maxbytes < r->capacity * sizeof(uint16_t)) {
        roaring64_bitmap_free(r);
        return NULL;
    }
    const char *elem_counts = buf;
    buf += r->capacity * sizeof(uint16_t);
    maxbytes -= r->capacity * sizeof(uint16_t);

    // Total container sizes.
    uint64_t total_sizes[4];
    if (maxbytes < sizeof(uint64_t) * 3) {
        roaring64_bitmap_free(r);
        return NULL;
    }
    memcpy(&(total_sizes[BITSET_CONTAINER_TYPE]), buf, sizeof(uint64_t));
    buf += sizeof(uint64_t);
    maxbytes -= sizeof(uint64_t);
    memcpy(&(total_sizes[RUN_CONTAINER_TYPE]), buf, sizeof(uint64_t));
    buf += sizeof(uint64_t);
    maxbytes -= sizeof(uint64_t);
    memcpy(&(total_sizes[ARRAY_CONTAINER_TYPE]), buf, sizeof(uint64_t));
    buf += sizeof(uint64_t);
    maxbytes -= sizeof(uint64_t);

    // ART (8 byte aligned).
    buf = CROARING_ALIGN_BUF(buf, 8);
    size_t art_size = art_frozen_view(buf, maxbytes, &r->art);
    if (art_size == 0) {
        roaring64_bitmap_free(r);
        return NULL;
    }
    buf += art_size;
    maxbytes -= art_size;

    // Containers (aligned).
    const char *before_containers = buf;
    buf = CROARING_ALIGN_BUF(buf, CROARING_BITSET_ALIGNMENT);
    const uint64_t *bitsets = (const uint64_t *)buf;
    buf += total_sizes[BITSET_CONTAINER_TYPE];
    buf = CROARING_ALIGN_BUF(buf, alignof(rle16_t));
    const rle16_t *runs = (const rle16_t *)buf;
    buf += total_sizes[RUN_CONTAINER_TYPE];
    buf = CROARING_ALIGN_BUF(buf, alignof(uint16_t));
    const uint16_t *arrays = (const uint16_t *)buf;
    buf += total_sizes[ARRAY_CONTAINER_TYPE];
    if (maxbytes < (uint64_t)(buf - before_containers)) {
        roaring64_bitmap_free(r);
        return NULL;
    }
    maxbytes -= buf - before_containers;

    // Deserialize in ART iteration order.
    art_iterator_t it = art_init_iterator(&r->art, /*first=*/true);
    for (size_t i = 0; it.value != NULL; ++i) {
        leaf_t leaf = (leaf_t)*it.value;
        uint8_t typecode = get_typecode(leaf);

        uint16_t compressed_elem_count;
        memcpy(&compressed_elem_count, elem_counts + (i * sizeof(uint16_t)),
               sizeof(compressed_elem_count));
        uint32_t elem_count = (uint32_t)(compressed_elem_count) + 1;

        // The container index is unrelated to the iteration order.
        uint64_t index = get_index(leaf);
        r->containers[index] = container_frozen_view(typecode, elem_count,
                                                     &bitsets, &arrays, &runs);

        art_iterator_next(&it);
    }

    // Padding to make overall size a multiple of required alignment.
    buf = CROARING_ALIGN_BUF(buf, CROARING_BITSET_ALIGNMENT);

    return r;
}

bool roaring64_bitmap_iterate(const roaring64_bitmap_t *r,
                              roaring_iterator64 iterator, void *ptr) {
    art_iterator_t it = art_init_iterator((art_t *)&r->art, /*first=*/true);
    while (it.value != NULL) {
        uint64_t high48 = combine_key(it.key, 0);
        uint64_t high32 = high48 & 0xFFFFFFFF00000000ULL;
        uint32_t low32 = high48;
        leaf_t leaf = (leaf_t)*it.value;
        if (!container_iterate64(get_container(r, leaf), get_typecode(leaf),
                                 low32, iterator, high32, ptr)) {
            return false;
        }
        art_iterator_next(&it);
    }
    return true;
}

void roaring64_bitmap_to_uint64_array(const roaring64_bitmap_t *r,
                                      uint64_t *out) {
    roaring64_iterator_t it;  // gets initialized in the next line
    roaring64_iterator_init_at(r, &it, /*first=*/true);
    roaring64_iterator_read(&it, out, UINT64_MAX);
}

roaring64_iterator_t *roaring64_iterator_create(const roaring64_bitmap_t *r) {
    roaring64_iterator_t *it =
        (roaring64_iterator_t *)roaring_malloc(sizeof(roaring64_iterator_t));
    return roaring64_iterator_init_at(r, it, /*first=*/true);
}

roaring64_iterator_t *roaring64_iterator_create_last(
    const roaring64_bitmap_t *r) {
    roaring64_iterator_t *it =
        (roaring64_iterator_t *)roaring_malloc(sizeof(roaring64_iterator_t));
    return roaring64_iterator_init_at(r, it, /*first=*/false);
}

void roaring64_iterator_reinit(const roaring64_bitmap_t *r,
                               roaring64_iterator_t *it) {
    roaring64_iterator_init_at(r, it, /*first=*/true);
}

void roaring64_iterator_reinit_last(const roaring64_bitmap_t *r,
                                    roaring64_iterator_t *it) {
    roaring64_iterator_init_at(r, it, /*first=*/false);
}

roaring64_iterator_t *roaring64_iterator_copy(const roaring64_iterator_t *it) {
    roaring64_iterator_t *new_it =
        (roaring64_iterator_t *)roaring_malloc(sizeof(roaring64_iterator_t));
    memcpy(new_it, it, sizeof(*it));
    return new_it;
}

void roaring64_iterator_free(roaring64_iterator_t *it) { roaring_free(it); }

bool roaring64_iterator_has_value(const roaring64_iterator_t *it) {
    return it->has_value;
}

uint64_t roaring64_iterator_value(const roaring64_iterator_t *it) {
    return it->value;
}

bool roaring64_iterator_advance(roaring64_iterator_t *it) {
    if (it->art_it.value == NULL) {
        if (it->saturated_forward) {
            return (it->has_value = false);
        }
        roaring64_iterator_init_at(it->r, it, /*first=*/true);
        return it->has_value;
    }
    leaf_t leaf = (leaf_t)*it->art_it.value;
    uint16_t low16 = (uint16_t)it->value;
    if (container_iterator_next(get_container(it->r, leaf), get_typecode(leaf),
                                &it->container_it, &low16)) {
        it->value = it->high48 | low16;
        return (it->has_value = true);
    }
    if (art_iterator_next(&it->art_it)) {
        return roaring64_iterator_init_at_leaf_first(it);
    }
    it->saturated_forward = true;
    return (it->has_value = false);
}

bool roaring64_iterator_previous(roaring64_iterator_t *it) {
    if (it->art_it.value == NULL) {
        if (!it->saturated_forward) {
            // Saturated backward.
            return (it->has_value = false);
        }
        roaring64_iterator_init_at(it->r, it, /*first=*/false);
        return it->has_value;
    }
    leaf_t leaf = (leaf_t)*it->art_it.value;
    uint16_t low16 = (uint16_t)it->value;
    if (container_iterator_prev(get_container(it->r, leaf), get_typecode(leaf),
                                &it->container_it, &low16)) {
        it->value = it->high48 | low16;
        return (it->has_value = true);
    }
    if (art_iterator_prev(&it->art_it)) {
        return roaring64_iterator_init_at_leaf_last(it);
    }
    it->saturated_forward = false;  // Saturated backward.
    return (it->has_value = false);
}

bool roaring64_iterator_move_equalorlarger(roaring64_iterator_t *it,
                                           uint64_t val) {
    uint8_t val_high48[ART_KEY_BYTES];
    uint16_t val_low16 = split_key(val, val_high48);
    if (!it->has_value || it->high48 != (val & 0xFFFFFFFFFFFF0000)) {
        // The ART iterator is before or after the high48 bits of `val` (or
        // beyond the ART altogether), so we need to move to a leaf with a
        // key equal or greater.
        if (!art_iterator_lower_bound(&it->art_it, val_high48)) {
            // Only smaller keys found.
            it->saturated_forward = true;
            return (it->has_value = false);
        }
        it->high48 = combine_key(it->art_it.key, 0);
        // Fall through to the next if statement.
    }

    if (it->high48 == (val & 0xFFFFFFFFFFFF0000)) {
        // We're at equal high bits, check if a suitable value can be found
        // in this container.
        leaf_t leaf = (leaf_t)*it->art_it.value;
        uint16_t low16 = (uint16_t)it->value;
        if (container_iterator_lower_bound(
                get_container(it->r, leaf), get_typecode(leaf),
                &it->container_it, &low16, val_low16)) {
            it->value = it->high48 | low16;
            return (it->has_value = true);
        }
        // Only smaller entries in this container, move to the next.
        if (!art_iterator_next(&it->art_it)) {
            it->saturated_forward = true;
            return (it->has_value = false);
        }
    }

    // We're at a leaf with high bits greater than `val`, so the first entry
    // in this container is our result.
    return roaring64_iterator_init_at_leaf_first(it);
}

uint64_t roaring64_iterator_read(roaring64_iterator_t *it, uint64_t *buf,
                                 uint64_t count) {
    uint64_t consumed = 0;
    while (it->has_value && consumed < count) {
        uint32_t container_consumed;
        leaf_t leaf = (leaf_t)*it->art_it.value;
        uint16_t low16 = (uint16_t)it->value;
        uint32_t container_count = UINT32_MAX;
        if (count - consumed < (uint64_t)UINT32_MAX) {
            container_count = count - consumed;
        }
        bool has_value = container_iterator_read_into_uint64(
            get_container(it->r, leaf), get_typecode(leaf), &it->container_it,
            it->high48, buf, container_count, &container_consumed, &low16);
        consumed += container_consumed;
        buf += container_consumed;
        if (has_value) {
            it->has_value = true;
            it->value = it->high48 | low16;
            assert(consumed == count);
            return consumed;
        }
        it->has_value = art_iterator_next(&it->art_it);
        if (it->has_value) {
            roaring64_iterator_init_at_leaf_first(it);
        } else {
            it->saturated_forward = true;
        }
    }
    return consumed;
}

uint64_t roaring64_iterator_read_backward(roaring64_iterator_t *it,
                                          uint64_t *buf, uint64_t count) {
    uint64_t consumed = 0;
    while (it->has_value && consumed < count) {
        uint32_t container_consumed;
        leaf_t leaf = *it->art_it.value;
        uint16_t low16 = (uint16_t)it->value;
        uint32_t container_count = UINT32_MAX;
        if (count - consumed < (uint64_t)UINT32_MAX) {
            container_count = count - consumed;
        }
        bool has_value = container_iterator_read_backward_into_uint64(
            get_container(it->r, leaf), get_typecode(leaf), &it->container_it,
            it->high48, buf, container_count, &container_consumed, &low16);
        consumed += container_consumed;
        buf += container_consumed;
        if (has_value) {
            it->has_value = true;
            it->value = it->high48 | low16;
            assert(consumed == count);
            return consumed;
        }
        it->has_value = art_iterator_prev(&it->art_it);
        if (it->has_value) {
            roaring64_iterator_init_at_leaf_last(it);
        } else {
            it->saturated_forward = false;
        }
    }
    return consumed;
}

size_t roaring64_iterator_read_ranges(roaring64_iterator_t *it,
                                      roaring64_range_closed_t *buf,
                                      size_t count) {
    size_t ret = 0;
    while (it->has_value && ret < count) {
        buf[ret].min = it->value;
        for (;;) {
            uint16_t low16 = (uint16_t)it->value;
            leaf_t leaf = (leaf_t)*it->art_it.value;
            bool container_has_more;
            uint16_t run_end_low16 = container_iterator_find_run_end(
                get_container(it->r, leaf), get_typecode(leaf),
                &it->container_it, &low16, &container_has_more);
            buf[ret].max = it->high48 | run_end_low16;

            if (container_has_more) {
                it->value = it->high48 | low16;
                break;
            }
            // Move to next leaf
            it->has_value = art_iterator_next(&it->art_it);
            if (it->has_value) {
                roaring64_iterator_init_at_leaf_first(it);
            } else {
                it->saturated_forward = true;
                break;
            }
            // Continue merging only if the run reached the container
            // boundary and the next leaf starts exactly at max+1.
            if (run_end_low16 != UINT16_MAX || it->value != buf[ret].max + 1) {
                break;
            }
        }
        ret++;
    }
    return ret;
}

size_t roaring64_iterator_read_prev_ranges(roaring64_iterator_t *it,
                                           roaring64_range_closed_t *buf,
                                           size_t count) {
    size_t ret = 0;
    while (it->has_value && ret < count) {
        buf[ret].max = it->value;
        for (;;) {
            uint16_t low16 = (uint16_t)it->value;
            leaf_t leaf = (leaf_t)*it->art_it.value;
            bool container_has_more;
            uint16_t run_start_low16 = container_iterator_find_run_start(
                get_container(it->r, leaf), get_typecode(leaf),
                &it->container_it, &low16, &container_has_more);
            buf[ret].min = it->high48 | run_start_low16;

            if (container_has_more) {
                it->value = it->high48 | low16;
                break;
            }
            // Move to previous leaf
            it->has_value = art_iterator_prev(&it->art_it);
            if (it->has_value) {
                roaring64_iterator_init_at_leaf_last(it);
            } else {
                it->saturated_forward = false;
                break;
            }
            // Continue merging only if the run reached the container
            // boundary and the previous leaf ends exactly at min-1.
            if (run_start_low16 != 0 || it->value != buf[ret].min - 1) {
                break;
            }
        }
        ret++;
    }
    return ret;
}

#ifdef __cplusplus
}  // extern "C"
}  // namespace roaring
}  // namespace api
#endif
