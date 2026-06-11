#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <roaring/roaring.h>

// Include after roaring.h
#include <roaring/array_util.h>
#include <roaring/bitset_util.h>
#include <roaring/containers/containers.h>
#include <roaring/roaring_array.h>

#ifdef __cplusplus
using namespace ::roaring::internal;

extern "C" {
namespace roaring {
namespace api {
#endif

#define CROARING_SERIALIZATION_ARRAY_UINT32 1
#define CROARING_SERIALIZATION_CONTAINER 2
extern inline bool roaring_bitmap_contains(const roaring_bitmap_t *r,
                                           uint32_t val);
extern inline int roaring_trailing_zeroes(unsigned long long input_num);
extern inline int roaring_leading_zeroes(unsigned long long input_num);
extern inline void roaring_bitmap_init_cleared(roaring_bitmap_t *r);
extern inline bool roaring_bitmap_get_copy_on_write(const roaring_bitmap_t *r);
extern inline void roaring_bitmap_set_copy_on_write(roaring_bitmap_t *r,
                                                    bool cow);
extern inline roaring_bitmap_t *roaring_bitmap_create(void);
extern inline void roaring_bitmap_add_range(roaring_bitmap_t *r, uint64_t min,
                                            uint64_t max);
extern inline void roaring_bitmap_remove_range(roaring_bitmap_t *r,
                                               uint64_t min, uint64_t max);

static inline bool is_cow(const roaring_bitmap_t *r) {
    return r->high_low_container.flags & ROARING_FLAG_COW;
}
static inline bool is_frozen(const roaring_bitmap_t *r) {
    return r->high_low_container.flags & ROARING_FLAG_FROZEN;
}

// this is like roaring_bitmap_add, but it populates pointer arguments in such a
// way
// that we can recover the container touched, which, in turn can be used to
// accelerate some functions (when you repeatedly need to add to the same
// container)
static inline container_t *containerptr_roaring_bitmap_add(roaring_bitmap_t *r,
                                                           uint32_t val,
                                                           uint8_t *type,
                                                           int *index) {
    roaring_array_t *ra = &r->high_low_container;

    uint16_t hb = val >> 16;
    const int i = ra_get_index(ra, hb);
    if (i >= 0) {
        ra_unshare_container_at_index(ra, (uint16_t)i);
        container_t *c = ra_get_container_at_index(ra, (uint16_t)i, type);
        uint8_t new_type = *type;
        container_t *c2 = container_add(c, val & 0xFFFF, *type, &new_type);
        *index = i;
        if (c2 != c) {
            container_free(c, *type);
            ra_set_container_at_index(ra, i, c2, new_type);
            *type = new_type;
            return c2;
        } else {
            return c;
        }
    } else {
        array_container_t *new_ac = array_container_create();
        container_t *c =
            container_add(new_ac, val & 0xFFFF, ARRAY_CONTAINER_TYPE, type);
        // we could just assume that it stays an array container
        ra_insert_new_key_value_at(ra, -i - 1, hb, c, *type);
        *index = -i - 1;
        return c;
    }
}

roaring_bitmap_t *roaring_bitmap_create_with_capacity(uint32_t cap) {
    roaring_bitmap_t *ans =
        (roaring_bitmap_t *)roaring_malloc(sizeof(roaring_bitmap_t));
    if (!ans) {
        return NULL;
    }
    bool is_ok = ra_init_with_capacity(&ans->high_low_container, cap);
    if (!is_ok) {
        roaring_free(ans);
        return NULL;
    }
    return ans;
}

bool roaring_bitmap_init_with_capacity(roaring_bitmap_t *r, uint32_t cap) {
    return ra_init_with_capacity(&r->high_low_container, cap);
}

static inline void add_bulk_impl(roaring_bitmap_t *r,
                                 roaring_bulk_context_t *context,
                                 uint32_t val) {
    uint16_t key = val >> 16;
    if (context->container == NULL || context->key != key) {
        uint8_t typecode;
        int idx;
        context->container =
            containerptr_roaring_bitmap_add(r, val, &typecode, &idx);
        context->typecode = typecode;
        context->idx = idx;
        context->key = key;
    } else {
        // no need to seek the container, it is at hand
        // because we already have the container at hand, we can do the
        // insertion directly, bypassing the roaring_bitmap_add call
        uint8_t new_typecode;
        container_t *container2 = container_add(
            context->container, val & 0xFFFF, context->typecode, &new_typecode);
        if (container2 != context->container) {
            // rare instance when we need to change the container type
            container_free(context->container, context->typecode);
            ra_set_container_at_index(&r->high_low_container, context->idx,
                                      container2, new_typecode);
            context->typecode = new_typecode;
            context->container = container2;
        }
    }
}

void roaring_bitmap_add_many(roaring_bitmap_t *r, size_t n_args,
                             const uint32_t *vals) {
    uint32_t val;
    const uint32_t *start = vals;
    const uint32_t *end = vals + n_args;
    const uint32_t *current_val = start;

    if (n_args == 0) {
        return;
    }

    uint8_t typecode;
    int idx;
    container_t *container;
    val = *current_val;
    container = containerptr_roaring_bitmap_add(r, val, &typecode, &idx);
    roaring_bulk_context_t context = {container, idx, (uint16_t)(val >> 16),
                                      typecode};

    for (; current_val != end; current_val++) {
        memcpy(&val, current_val, sizeof(val));
        add_bulk_impl(r, &context, val);
    }
}

void roaring_bitmap_add_bulk(roaring_bitmap_t *r,
                             roaring_bulk_context_t *context, uint32_t val) {
    add_bulk_impl(r, context, val);
}

bool roaring_bitmap_contains_bulk(const roaring_bitmap_t *r,
                                  roaring_bulk_context_t *context,
                                  uint32_t val) {
    uint16_t key = val >> 16;
    if (context->container == NULL || context->key != key) {
        int32_t start_idx = -1;
        if (context->container != NULL && context->key < key) {
            start_idx = context->idx;
        }
        int idx = ra_advance_until(&r->high_low_container, key, start_idx);
        if (idx == ra_get_size(&r->high_low_container)) {
            return false;
        }
        uint8_t typecode;
        context->container = ra_get_container_at_index(
            &r->high_low_container, (uint16_t)idx, &typecode);
        context->typecode = typecode;
        context->idx = idx;
        context->key =
            ra_get_key_at_index(&r->high_low_container, (uint16_t)idx);
        // ra_advance_until finds the next key >= the target, we found a later
        // container.
        if (context->key != key) {
            return false;
        }
    }
    // context is now set up
    return container_contains(context->container, val & 0xFFFF,
                              context->typecode);
}

roaring_bitmap_t *roaring_bitmap_of_ptr(size_t n_args, const uint32_t *vals) {
    roaring_bitmap_t *answer = roaring_bitmap_create();
    roaring_bitmap_add_many(answer, n_args, vals);
    return answer;
}

roaring_bitmap_t *roaring_bitmap_of(size_t n_args, ...) {
    // todo: could be greatly optimized but we do not expect this call to ever
    // include long lists
    roaring_bitmap_t *answer = roaring_bitmap_create();
    roaring_bulk_context_t context = CROARING_ZERO_INITIALIZER;
    va_list ap;
    va_start(ap, n_args);
    for (size_t i = 0; i < n_args; i++) {
        uint32_t val = va_arg(ap, uint32_t);
        roaring_bitmap_add_bulk(answer, &context, val);
    }
    va_end(ap);
    return answer;
}

static inline uint64_t minimum_uint64(uint64_t a, uint64_t b) {
    return (a < b) ? a : b;
}

roaring_bitmap_t *roaring_bitmap_from_range(uint64_t min, uint64_t max,
                                            uint32_t step) {
    if (max >= UINT64_C(0x100000000)) {
        max = UINT64_C(0x100000000);
    }
    if (step == 0) return NULL;
    if (max <= min) return NULL;
    roaring_bitmap_t *answer = roaring_bitmap_create();
    if (step >= (1 << 16)) {
        for (uint32_t value = (uint32_t)min; value < max; value += step) {
            roaring_bitmap_add(answer, value);
        }
        return answer;
    }
    uint64_t min_tmp = min;
    do {
        uint32_t key = (uint32_t)min_tmp >> 16;
        uint32_t container_min = min_tmp & 0xFFFF;
        uint32_t container_max =
            (uint32_t)minimum_uint64(max - (key << 16), 1 << 16);
        uint8_t type;
        container_t *container = container_from_range(
            &type, container_min, container_max, (uint16_t)step);
        ra_append(&answer->high_low_container, (uint16_t)key, container, type);
        uint32_t gap = container_max - container_min + step - 1;
        min_tmp += gap - (gap % step);
    } while (min_tmp < max);
    // cardinality of bitmap will be ((uint64_t) max - min + step - 1 ) / step
    return answer;
}

void roaring_bitmap_add_range_closed(roaring_bitmap_t *r, uint32_t min,
                                     uint32_t max) {
    if (min > max) {
        return;
    }

    roaring_array_t *ra = &r->high_low_container;

    uint32_t min_key = min >> 16;
    uint32_t max_key = max >> 16;

    int32_t num_required_containers = max_key - min_key + 1;
    int32_t suffix_length =
        count_greater(ra->keys, ra->size, (uint16_t)max_key);
    int32_t prefix_length =
        count_less(ra->keys, ra->size - suffix_length, (uint16_t)min_key);
    int32_t common_length = ra->size - prefix_length - suffix_length;

    if (num_required_containers > common_length) {
        ra_shift_tail(ra, suffix_length,
                      num_required_containers - common_length);
    }

    int32_t src = prefix_length + common_length - 1;
    int32_t dst = ra->size - suffix_length - 1;
    for (uint32_t key = max_key; key != min_key - 1;
         key--) {  // beware of min_key==0
        uint32_t container_min = (min_key == key) ? (min & 0xffff) : 0;
        uint32_t container_max = (max_key == key) ? (max & 0xffff) : 0xffff;
        container_t *new_container;
        uint8_t new_type;

        if (src >= 0 && ra->keys[src] == key) {
            ra_unshare_container_at_index(ra, (uint16_t)src);
            new_container =
                container_add_range(ra->containers[src], ra->typecodes[src],
                                    container_min, container_max, &new_type);
            if (new_container != ra->containers[src]) {
                container_free(ra->containers[src], ra->typecodes[src]);
            }
            src--;
        } else {
            new_container = container_from_range(&new_type, container_min,
                                                 container_max + 1, 1);
        }
        ra_replace_key_and_container_at_index(ra, dst, (uint16_t)key,
                                              new_container, new_type);
        dst--;
    }
}

void roaring_bitmap_remove_range_closed(roaring_bitmap_t *r, uint32_t min,
                                        uint32_t max) {
    if (min > max) {
        return;
    }

    roaring_array_t *ra = &r->high_low_container;

    uint32_t min_key = min >> 16;
    uint32_t max_key = max >> 16;

    int32_t src = count_less(ra->keys, ra->size, (uint16_t)min_key);
    int32_t dst = src;
    while (src < ra->size && ra->keys[src] <= max_key) {
        uint32_t container_min =
            (min_key == ra->keys[src]) ? (min & 0xffff) : 0;
        uint32_t container_max =
            (max_key == ra->keys[src]) ? (max & 0xffff) : 0xffff;
        ra_unshare_container_at_index(ra, (uint16_t)src);
        container_t *new_container;
        uint8_t new_type;
        new_container =
            container_remove_range(ra->containers[src], ra->typecodes[src],
                                   container_min, container_max, &new_type);
        if (new_container != ra->containers[src]) {
            container_free(ra->containers[src], ra->typecodes[src]);
        }
        if (new_container) {
            ra_replace_key_and_container_at_index(ra, dst, ra->keys[src],
                                                  new_container, new_type);
            dst++;
        }
        src++;
    }
    if (src > dst) {
        ra_shift_tail(ra, ra->size - src, dst - src);
    }
}

void roaring_bitmap_printf(const roaring_bitmap_t *r) {
    const roaring_array_t *ra = &r->high_low_container;

    printf("{");
    for (int i = 0; i < ra->size; ++i) {
        container_printf_as_uint32_array(ra->containers[i], ra->typecodes[i],
                                         ((uint32_t)ra->keys[i]) << 16);

        if (i + 1 < ra->size) {
            printf(",");
        }
    }
    printf("}");
}

void roaring_bitmap_printf_describe(const roaring_bitmap_t *r) {
    const roaring_array_t *ra = &r->high_low_container;

    printf("{");
    for (int i = 0; i < ra->size; ++i) {
        printf("%d: %s (%d)", ra->keys[i],
               get_full_container_name(ra->containers[i], ra->typecodes[i]),
               container_get_cardinality(ra->containers[i], ra->typecodes[i]));
        if (ra->typecodes[i] == SHARED_CONTAINER_TYPE) {
            printf("(shared count = %" PRIu32 " )",
                   croaring_refcount_get(
                       &(CAST_shared(ra->containers[i])->counter)));
        }

        if (i + 1 < ra->size) {
            printf(", ");
        }
    }
    printf("}");
}

/**
 *  (For advanced users.)
 * Collect statistics about the bitmap
 */
void roaring_bitmap_statistics(const roaring_bitmap_t *r,
                               roaring_statistics_t *stat) {
    const roaring_array_t *ra = &r->high_low_container;

    memset(stat, 0, sizeof(*stat));
    stat->n_containers = ra->size;
    stat->min_value = roaring_bitmap_minimum(r);
    stat->max_value = roaring_bitmap_maximum(r);

    for (int i = 0; i < ra->size; ++i) {
        uint8_t truetype =
            get_container_type(ra->containers[i], ra->typecodes[i]);
        uint32_t card =
            container_get_cardinality(ra->containers[i], ra->typecodes[i]);
        uint32_t sbytes =
            container_size_in_bytes(ra->containers[i], ra->typecodes[i]);
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
    }
}

bool roaring_contains_shared(const roaring_bitmap_t *r) {
    const roaring_array_t *ra = &r->high_low_container;
    for (int i = 0; i < ra->size; ++i) {
        if (ra->typecodes[i] == SHARED_CONTAINER_TYPE) {
            return true;
        }
    }
    return false;
}

bool roaring_unshare_all(roaring_bitmap_t *r) {
    const roaring_array_t *ra = &r->high_low_container;
    bool unshared = false;
    for (int i = 0; i < ra->size; ++i) {
        uint8_t typecode = ra->typecodes[i];
        if (typecode == SHARED_CONTAINER_TYPE) {
            ra->containers[i] = get_writable_copy_if_shared(ra->containers[i],
                                                            &ra->typecodes[i]);
            unshared = true;
        }
    }
    return unshared;
}

/*
 * Checks that:
 * - Array containers are sorted and contain no duplicates
 * - Range containers are sorted and contain no overlapping ranges
 * - Roaring containers are sorted by key and there are no duplicate keys
 * - The correct container type is use for each container (e.g. bitmaps aren't
 * used for small containers)
 * - Shared containers are only used when the bitmap is COW
 */
bool roaring_bitmap_internal_validate(const roaring_bitmap_t *r,
                                      const char **reason) {
    const char *reason_local;
    if (reason == NULL) {
        // Always allow assigning through *reason
        reason = &reason_local;
    }
    *reason = NULL;
    const roaring_array_t *ra = &r->high_low_container;
    if (ra->size < 0) {
        *reason = "negative size";
        return false;
    }
    if (ra->allocation_size < 0) {
        *reason = "negative allocation size";
        return false;
    }
    if (ra->size > ra->allocation_size) {
        *reason = "more containers than allocated space";
        return false;
    }
    if (ra->flags & ~(ROARING_FLAG_COW | ROARING_FLAG_FROZEN)) {
        *reason = "invalid flags";
        return false;
    }
    if (ra->size == 0) {
        return true;
    }

    if (ra->keys == NULL) {
        *reason = "keys is NULL";
        return false;
    }
    if (ra->typecodes == NULL) {
        *reason = "typecodes is NULL";
        return false;
    }
    if (ra->containers == NULL) {
        *reason = "containers is NULL";
        return false;
    }

    uint32_t prev_key = ra->keys[0];
    for (int32_t i = 1; i < ra->size; ++i) {
        if (ra->keys[i] <= prev_key) {
            *reason = "keys not strictly increasing";
            return false;
        }
        prev_key = ra->keys[i];
    }

    bool cow = roaring_bitmap_get_copy_on_write(r);

    for (int32_t i = 0; i < ra->size; ++i) {
        if (ra->typecodes[i] == SHARED_CONTAINER_TYPE && !cow) {
            *reason = "shared container in non-COW bitmap";
            return false;
        }
        if (!container_internal_validate(ra->containers[i], ra->typecodes[i],
                                         reason)) {
            // reason should already be set
            if (*reason == NULL) {
                *reason = "container failed to validate but no reason given";
            }
            return false;
        }
    }

    return true;
}

roaring_bitmap_t *roaring_bitmap_copy(const roaring_bitmap_t *r) {
    roaring_bitmap_t *ans =
        (roaring_bitmap_t *)roaring_malloc(sizeof(roaring_bitmap_t));
    if (!ans) {
        return NULL;
    }
    if (!ra_init_with_capacity(  // allocation of list of containers can fail
            &ans->high_low_container, r->high_low_container.size)) {
        roaring_free(ans);
        return NULL;
    }
    if (!ra_overwrite(  // memory allocation of individual containers may fail
            &r->high_low_container, &ans->high_low_container, is_cow(r))) {
        roaring_bitmap_free(ans);  // overwrite should leave in freeable state
        return NULL;
    }
    roaring_bitmap_set_copy_on_write(ans, is_cow(r));
    return ans;
}

bool roaring_bitmap_overwrite(roaring_bitmap_t *dest,
                              const roaring_bitmap_t *src) {
    roaring_bitmap_set_copy_on_write(dest, is_cow(src));
    return ra_overwrite(&src->high_low_container, &dest->high_low_container,
                        is_cow(src));
}

void roaring_bitmap_free(const roaring_bitmap_t *r) {
    if (r == NULL) {
        return;
    }
    if (!is_frozen(r)) {
        ra_clear((roaring_array_t *)&r->high_low_container);
    }
    roaring_free((roaring_bitmap_t *)r);
}

void roaring_bitmap_clear(roaring_bitmap_t *r) {
    ra_reset(&r->high_low_container);
}

void roaring_bitmap_add(roaring_bitmap_t *r, uint32_t val) {
    roaring_array_t *ra = &r->high_low_container;

    const uint16_t hb = val >> 16;
    const int i = ra_get_index(ra, hb);
    uint8_t typecode;
    if (i >= 0) {
        ra_unshare_container_at_index(ra, (uint16_t)i);
        container_t *container =
            ra_get_container_at_index(ra, (uint16_t)i, &typecode);
        uint8_t newtypecode = typecode;
        container_t *container2 =
            container_add(container, val & 0xFFFF, typecode, &newtypecode);
        if (container2 != container) {
            container_free(container, typecode);
            ra_set_container_at_index(&r->high_low_container, i, container2,
                                      newtypecode);
        }
    } else {
        array_container_t *newac = array_container_create();
        container_t *container =
            container_add(newac, val & 0xFFFF, ARRAY_CONTAINER_TYPE, &typecode);
        // we could just assume that it stays an array container
        ra_insert_new_key_value_at(&r->high_low_container, -i - 1, hb,
                                   container, typecode);
    }
}

bool roaring_bitmap_add_checked(roaring_bitmap_t *r, uint32_t val) {
    const uint16_t hb = val >> 16;
    const int i = ra_get_index(&r->high_low_container, hb);
    uint8_t typecode;
    bool result = false;
    if (i >= 0) {
        ra_unshare_container_at_index(&r->high_low_container, (uint16_t)i);
        container_t *container = ra_get_container_at_index(
            &r->high_low_container, (uint16_t)i, &typecode);

        const int oldCardinality =
            container_get_cardinality(container, typecode);

        uint8_t newtypecode = typecode;
        container_t *container2 =
            container_add(container, val & 0xFFFF, typecode, &newtypecode);
        if (container2 != container) {
            container_free(container, typecode);
            ra_set_container_at_index(&r->high_low_container, i, container2,
                                      newtypecode);
            result = true;
        } else {
            const int newCardinality =
                container_get_cardinality(container, newtypecode);

            result = oldCardinality != newCardinality;
        }
    } else {
        array_container_t *newac = array_container_create();
        container_t *container =
            container_add(newac, val & 0xFFFF, ARRAY_CONTAINER_TYPE, &typecode);
        // we could just assume that it stays an array container
        ra_insert_new_key_value_at(&r->high_low_container, -i - 1, hb,
                                   container, typecode);
        result = true;
    }

    return result;
}

void roaring_bitmap_remove(roaring_bitmap_t *r, uint32_t val) {
    const uint16_t hb = val >> 16;
    const int i = ra_get_index(&r->high_low_container, hb);
    uint8_t typecode;
    if (i >= 0) {
        ra_unshare_container_at_index(&r->high_low_container, (uint16_t)i);
        container_t *container = ra_get_container_at_index(
            &r->high_low_container, (uint16_t)i, &typecode);
        uint8_t newtypecode = typecode;
        container_t *container2 =
            container_remove(container, val & 0xFFFF, typecode, &newtypecode);
        if (container2 != container) {
            container_free(container, typecode);
            ra_set_container_at_index(&r->high_low_container, i, container2,
                                      newtypecode);
        }
        if (container_nonzero_cardinality(container2, newtypecode)) {
            ra_set_container_at_index(&r->high_low_container, i, container2,
                                      newtypecode);
        } else {
            ra_remove_at_index_and_free(&r->high_low_container, i);
        }
    }
}

bool roaring_bitmap_remove_checked(roaring_bitmap_t *r, uint32_t val) {
    const uint16_t hb = val >> 16;
    const int i = ra_get_index(&r->high_low_container, hb);
    uint8_t typecode;
    bool result = false;
    if (i >= 0) {
        ra_unshare_container_at_index(&r->high_low_container, (uint16_t)i);
        container_t *container = ra_get_container_at_index(
            &r->high_low_container, (uint16_t)i, &typecode);

        const int oldCardinality =
            container_get_cardinality(container, typecode);

        uint8_t newtypecode = typecode;
        container_t *container2 =
            container_remove(container, val & 0xFFFF, typecode, &newtypecode);
        if (container2 != container) {
            container_free(container, typecode);
            ra_set_container_at_index(&r->high_low_container, i, container2,
                                      newtypecode);
        }

        const int newCardinality =
            container_get_cardinality(container2, newtypecode);

        if (newCardinality != 0) {
            ra_set_container_at_index(&r->high_low_container, i, container2,
                                      newtypecode);
        } else {
            ra_remove_at_index_and_free(&r->high_low_container, i);
        }

        result = oldCardinality != newCardinality;
    }
    return result;
}

void roaring_bitmap_remove_many(roaring_bitmap_t *r, size_t n_args,
                                const uint32_t *vals) {
    if (n_args == 0 || r->high_low_container.size == 0) {
        return;
    }
    int32_t pos =
        -1;  // position of the container used in the previous iteration
    for (size_t i = 0; i < n_args; i++) {
        uint16_t key = (uint16_t)(vals[i] >> 16);
        if (pos < 0 || key != r->high_low_container.keys[pos]) {
            pos = ra_get_index(&r->high_low_container, key);
        }
        if (pos >= 0) {
            uint8_t new_typecode;
            container_t *new_container;
            new_container = container_remove(
                r->high_low_container.containers[pos], vals[i] & 0xffff,
                r->high_low_container.typecodes[pos], &new_typecode);
            if (new_container != r->high_low_container.containers[pos]) {
                container_free(r->high_low_container.containers[pos],
                               r->high_low_container.typecodes[pos]);
                ra_replace_key_and_container_at_index(&r->high_low_container,
                                                      pos, key, new_container,
                                                      new_typecode);
            }
            if (!container_nonzero_cardinality(new_container, new_typecode)) {
                container_free(new_container, new_typecode);
                ra_remove_at_index(&r->high_low_container, pos);
                pos = -1;
            }
        }
    }
}

// there should be some SIMD optimizations possible here
roaring_bitmap_t *roaring_bitmap_and(const roaring_bitmap_t *x1,
                                     const roaring_bitmap_t *x2) {
    uint8_t result_type = 0;
    const int length1 = x1->high_low_container.size,
              length2 = x2->high_low_container.size;
    uint32_t neededcap = length1 > length2 ? length2 : length1;
    roaring_bitmap_t *answer = roaring_bitmap_create_with_capacity(neededcap);
    roaring_bitmap_set_copy_on_write(answer, is_cow(x1) || is_cow(x2));

    int pos1 = 0, pos2 = 0;

    while (pos1 < length1 && pos2 < length2) {
        const uint16_t s1 =
            ra_get_key_at_index(&x1->high_low_container, (uint16_t)pos1);
        const uint16_t s2 =
            ra_get_key_at_index(&x2->high_low_container, (uint16_t)pos2);

        if (s1 == s2) {
            uint8_t type1, type2;
            container_t *c1 = ra_get_container_at_index(&x1->high_low_container,
                                                        (uint16_t)pos1, &type1);
            container_t *c2 = ra_get_container_at_index(&x2->high_low_container,
                                                        (uint16_t)pos2, &type2);
            container_t *c = container_and(c1, type1, c2, type2, &result_type);

            if (container_nonzero_cardinality(c, result_type)) {
                ra_append(&answer->high_low_container, s1, c, result_type);
            } else {
                container_free(c, result_type);  // otherwise: memory leak!
            }
            ++pos1;
            ++pos2;
        } else if (s1 < s2) {  // s1 < s2
            pos1 = ra_advance_until(&x1->high_low_container, s2, pos1);
        } else {  // s1 > s2
            pos2 = ra_advance_until(&x2->high_low_container, s1, pos2);
        }
    }
    return answer;
}

/**
 * Compute the union of 'number' bitmaps.
 */
roaring_bitmap_t *roaring_bitmap_or_many(size_t number,
                                         const roaring_bitmap_t **x) {
    if (number == 0) {
        return roaring_bitmap_create();
    }
    if (number == 1) {
        return roaring_bitmap_copy(x[0]);
    }
    roaring_bitmap_t *answer =
        roaring_bitmap_lazy_or(x[0], x[1], LAZY_OR_BITSET_CONVERSION);
    for (size_t i = 2; i < number; i++) {
        roaring_bitmap_lazy_or_inplace(answer, x[i], LAZY_OR_BITSET_CONVERSION);
    }
    roaring_bitmap_repair_after_lazy(answer);
    return answer;
}

/**
 * Compute the xor of 'number' bitmaps.
 */
roaring_bitmap_t *roaring_bitmap_xor_many(size_t number,
                                          const roaring_bitmap_t **x) {
    if (number == 0) {
        return roaring_bitmap_create();
    }
    if (number == 1) {
        return roaring_bitmap_copy(x[0]);
    }
    roaring_bitmap_t *answer = roaring_bitmap_lazy_xor(x[0], x[1]);
    for (size_t i = 2; i < number; i++) {
        roaring_bitmap_lazy_xor_inplace(answer, x[i]);
    }
    roaring_bitmap_repair_after_lazy(answer);
    return answer;
}

// inplace and (modifies its first argument).
void roaring_bitmap_and_inplace(roaring_bitmap_t *x1,
                                const roaring_bitmap_t *x2) {
    if (x1 == x2) return;
    int pos1 = 0, pos2 = 0, intersection_size = 0;
    const int length1 = ra_get_size(&x1->high_low_container);
    const int length2 = ra_get_size(&x2->high_low_container);

    // any skipped-over or newly emptied containers in x1
    // have to be freed.
    while (pos1 < length1 && pos2 < length2) {
        const uint16_t s1 =
            ra_get_key_at_index(&x1->high_low_container, (uint16_t)pos1);
        const uint16_t s2 =
            ra_get_key_at_index(&x2->high_low_container, (uint16_t)pos2);

        if (s1 == s2) {
            uint8_t type1, type2, result_type;
            container_t *c1 = ra_get_container_at_index(&x1->high_low_container,
                                                        (uint16_t)pos1, &type1);
            container_t *c2 = ra_get_container_at_index(&x2->high_low_container,
                                                        (uint16_t)pos2, &type2);

            // We do the computation "in place" only when c1 is not a shared
            // container. Rationale: using a shared container safely with in
            // place computation would require making a copy and then doing the
            // computation in place which is likely less efficient than avoiding
            // in place entirely and always generating a new container.
            container_t *c =
                (type1 == SHARED_CONTAINER_TYPE)
                    ? container_and(c1, type1, c2, type2, &result_type)
                    : container_iand(c1, type1, c2, type2, &result_type);

            if (c != c1) {  // in this instance a new container was created, and
                            // we need to free the old one
                container_free(c1, type1);
            }
            if (container_nonzero_cardinality(c, result_type)) {
                ra_replace_key_and_container_at_index(&x1->high_low_container,
                                                      intersection_size, s1, c,
                                                      result_type);
                intersection_size++;
            } else {
                container_free(c, result_type);
            }
            ++pos1;
            ++pos2;
        } else if (s1 < s2) {
            pos1 = ra_advance_until_freeing(&x1->high_low_container, s2, pos1);
        } else {  // s1 > s2
            pos2 = ra_advance_until(&x2->high_low_container, s1, pos2);
        }
    }

    // if we ended early because x2 ran out, then all remaining in x1 should be
    // freed
    while (pos1 < length1) {
        container_free(x1->high_low_container.containers[pos1],
                       x1->high_low_container.typecodes[pos1]);
        ++pos1;
    }

    // all containers after this have either been copied or freed
    ra_downsize(&x1->high_low_container, intersection_size);
}

roaring_bitmap_t *roaring_bitmap_or(const roaring_bitmap_t *x1,
                                    const roaring_bitmap_t *x2) {
    uint8_t result_type = 0;
    const int length1 = x1->high_low_container.size,
              length2 = x2->high_low_container.size;
    if (0 == length1) {
        return roaring_bitmap_copy(x2);
    }
    if (0 == length2) {
        return roaring_bitmap_copy(x1);
    }
    roaring_bitmap_t *answer =
        roaring_bitmap_create_with_capacity(length1 + length2);
    roaring_bitmap_set_copy_on_write(answer, is_cow(x1) || is_cow(x2));
    int pos1 = 0, pos2 = 0;
    uint8_t type1, type2;
    uint16_t s1 = ra_get_key_at_index(&x1->high_low_container, (uint16_t)pos1);
    uint16_t s2 = ra_get_key_at_index(&x2->high_low_container, (uint16_t)pos2);
    while (true) {
        if (s1 == s2) {
            container_t *c1 = ra_get_container_at_index(&x1->high_low_container,
                                                        (uint16_t)pos1, &type1);
            container_t *c2 = ra_get_container_at_index(&x2->high_low_container,
                                                        (uint16_t)pos2, &type2);
            container_t *c = container_or(c1, type1, c2, type2, &result_type);

            // since we assume that the initial containers are non-empty, the
            // result here
            // can only be non-empty
            ra_append(&answer->high_low_container, s1, c, result_type);
            ++pos1;
            ++pos2;
            if (pos1 == length1) break;
            if (pos2 == length2) break;
            s1 = ra_get_key_at_index(&x1->high_low_container, (uint16_t)pos1);
            s2 = ra_get_key_at_index(&x2->high_low_container, (uint16_t)pos2);

        } else if (s1 < s2) {  // s1 < s2
            container_t *c1 = ra_get_container_at_index(&x1->high_low_container,
                                                        (uint16_t)pos1, &type1);
            // c1 = container_clone(c1, type1);
            c1 = get_copy_of_container(c1, &type1, is_cow(x1));
            if (is_cow(x1)) {
                ra_set_container_at_index(&x1->high_low_container, pos1, c1,
                                          type1);
            }
            ra_append(&answer->high_low_container, s1, c1, type1);
            pos1++;
            if (pos1 == length1) break;
            s1 = ra_get_key_at_index(&x1->high_low_container, (uint16_t)pos1);

        } else {  // s1 > s2
            container_t *c2 = ra_get_container_at_index(&x2->high_low_container,
                                                        (uint16_t)pos2, &type2);
            // c2 = container_clone(c2, type2);
            c2 = get_copy_of_container(c2, &type2, is_cow(x2));
            if (is_cow(x2)) {
                ra_set_container_at_index(&x2->high_low_container, pos2, c2,
                                          type2);
            }
            ra_append(&answer->high_low_container, s2, c2, type2);
            pos2++;
            if (pos2 == length2) break;
            s2 = ra_get_key_at_index(&x2->high_low_container, (uint16_t)pos2);
        }
    }
    if (pos1 == length1) {
        ra_append_copy_range(&answer->high_low_container,
                             &x2->high_low_container, pos2, length2,
                             is_cow(x2));
    } else if (pos2 == length2) {
        ra_append_copy_range(&answer->high_low_container,
                             &x1->high_low_container, pos1, length1,
                             is_cow(x1));
    }
    return answer;
}

// inplace or (modifies its first argument).
void roaring_bitmap_or_inplace(roaring_bitmap_t *x1,
                               const roaring_bitmap_t *x2) {
    uint8_t result_type = 0;
    int length1 = x1->high_low_container.size;
    const int length2 = x2->high_low_container.size;

    if (0 == length2) return;

    if (0 == length1) {
        roaring_bitmap_overwrite(x1, x2);
        return;
    }
    int pos1 = 0, pos2 = 0;
    uint8_t type1, type2;
    uint16_t s1 = ra_get_key_at_index(&x1->high_low_container, (uint16_t)pos1);
    uint16_t s2 = ra_get_key_at_index(&x2->high_low_container, (uint16_t)pos2);
    while (true) {
        if (s1 == s2) {
            container_t *c1 = ra_get_container_at_index(&x1->high_low_container,
                                                        (uint16_t)pos1, &type1);
            if (!container_is_full(c1, type1)) {
                container_t *c2 = ra_get_container_at_index(
                    &x2->high_low_container, (uint16_t)pos2, &type2);
                container_t *c =
                    (type1 == SHARED_CONTAINER_TYPE)
                        ? container_or(c1, type1, c2, type2, &result_type)
                        : container_ior(c1, type1, c2, type2, &result_type);

                if (c != c1) {  // in this instance a new container was created,
                                // and we need to free the old one
                    container_free(c1, type1);
                }
                ra_set_container_at_index(&x1->high_low_container, pos1, c,
                                          result_type);
            }
            ++pos1;
            ++pos2;
            if (pos1 == length1) break;
            if (pos2 == length2) break;
            s1 = ra_get_key_at_index(&x1->high_low_container, (uint16_t)pos1);
            s2 = ra_get_key_at_index(&x2->high_low_container, (uint16_t)pos2);

        } else if (s1 < s2) {  // s1 < s2
            pos1++;
            if (pos1 == length1) break;
            s1 = ra_get_key_at_index(&x1->high_low_container, (uint16_t)pos1);

        } else {  // s1 > s2
            container_t *c2 = ra_get_container_at_index(&x2->high_low_container,
                                                        (uint16_t)pos2, &type2);
            c2 = get_copy_of_container(c2, &type2, is_cow(x2));
            if (is_cow(x2)) {
                ra_set_container_at_index(&x2->high_low_container, pos2, c2,
                                          type2);
            }

            // container_t *c2_clone = container_clone(c2, type2);
            ra_insert_new_key_value_at(&x1->high_low_container, pos1, s2, c2,
                                       type2);
            pos1++;
            length1++;
            pos2++;
            if (pos2 == length2) break;
            s2 = ra_get_key_at_index(&x2->high_low_container, (uint16_t)pos2);
        }
    }
    if (pos1 == length1) {
        ra_append_copy_range(&x1->high_low_container, &x2->high_low_container,
                             pos2, length2, is_cow(x2));
    }
}

roaring_bitmap_t *roaring_bitmap_xor(const roaring_bitmap_t *x1,
                                     const roaring_bitmap_t *x2) {
    uint8_t result_type = 0;
    const int length1 = x1->high_low_container.size,
              length2 = x2->high_low_container.size;
    if (0 == length1) {
        return roaring_bitmap_copy(x2);
    }
    if (0 == length2) {
        return roaring_bitmap_copy(x1);
    }
    roaring_bitmap_t *answer =
        roaring_bitmap_create_with_capacity(length1 + length2);
    roaring_bitmap_set_copy_on_write(answer, is_cow(x1) || is_cow(x2));
    int pos1 = 0, pos2 = 0;
    uint8_t type1, type2;
    uint16_t s1 = ra_get_key_at_index(&x1->high_low_container, (uint16_t)pos1);
    uint16_t s2 = ra_get_key_at_index(&x2->high_low_container, (uint16_t)pos2);
    while (true) {
        if (s1 == s2) {
            container_t *c1 = ra_get_container_at_index(&x1->high_low_container,
                                                        (uint16_t)pos1, &type1);
            container_t *c2 = ra_get_container_at_index(&x2->high_low_container,
                                                        (uint16_t)pos2, &type2);
            container_t *c = container_xor(c1, type1, c2, type2, &result_type);

            if (container_nonzero_cardinality(c, result_type)) {
                ra_append(&answer->high_low_container, s1, c, result_type);
            } else {
                container_free(c, result_type);
            }
            ++pos1;
            ++pos2;
            if (pos1 == length1) break;
            if (pos2 == length2) break;
            s1 = ra_get_key_at_index(&x1->high_low_container, (uint16_t)pos1);
            s2 = ra_get_key_at_index(&x2->high_low_container, (uint16_t)pos2);

        } else if (s1 < s2) {  // s1 < s2
            container_t *c1 = ra_get_container_at_index(&x1->high_low_container,
                                                        (uint16_t)pos1, &type1);
            c1 = get_copy_of_container(c1, &type1, is_cow(x1));
            if (is_cow(x1)) {
                ra_set_container_at_index(&x1->high_low_container, pos1, c1,
                                          type1);
            }
            ra_append(&answer->high_low_container, s1, c1, type1);
            pos1++;
            if (pos1 == length1) break;
            s1 = ra_get_key_at_index(&x1->high_low_container, (uint16_t)pos1);

        } else {  // s1 > s2
            container_t *c2 = ra_get_container_at_index(&x2->high_low_container,
                                                        (uint16_t)pos2, &type2);
            c2 = get_copy_of_container(c2, &type2, is_cow(x2));
            if (is_cow(x2)) {
                ra_set_container_at_index(&x2->high_low_container, pos2, c2,
                                          type2);
            }
            ra_append(&answer->high_low_container, s2, c2, type2);
            pos2++;
            if (pos2 == length2) break;
            s2 = ra_get_key_at_index(&x2->high_low_container, (uint16_t)pos2);
        }
    }
    if (pos1 == length1) {
        ra_append_copy_range(&answer->high_low_container,
                             &x2->high_low_container, pos2, length2,
                             is_cow(x2));
    } else if (pos2 == length2) {
        ra_append_copy_range(&answer->high_low_container,
                             &x1->high_low_container, pos1, length1,
                             is_cow(x1));
    }
    return answer;
}

// inplace xor (modifies its first argument).

void roaring_bitmap_xor_inplace(roaring_bitmap_t *x1,
                                const roaring_bitmap_t *x2) {
    assert(x1 != x2);
    uint8_t result_type = 0;
    int length1 = x1->high_low_container.size;
    const int length2 = x2->high_low_container.size;

    if (0 == length2) return;

    if (0 == length1) {
        roaring_bitmap_overwrite(x1, x2);
        return;
    }

    // XOR can have new containers inserted from x2, but can also
    // lose containers when x1 and x2 are nonempty and identical.

    int pos1 = 0, pos2 = 0;
    uint8_t type1, type2;
    uint16_t s1 = ra_get_key_at_index(&x1->high_low_container, (uint16_t)pos1);
    uint16_t s2 = ra_get_key_at_index(&x2->high_low_container, (uint16_t)pos2);
    while (true) {
        if (s1 == s2) {
            container_t *c1 = ra_get_container_at_index(&x1->high_low_container,
                                                        (uint16_t)pos1, &type1);
            container_t *c2 = ra_get_container_at_index(&x2->high_low_container,
                                                        (uint16_t)pos2, &type2);

            // We do the computation "in place" only when c1 is not a shared
            // container. Rationale: using a shared container safely with in
            // place computation would require making a copy and then doing the
            // computation in place which is likely less efficient than avoiding
            // in place entirely and always generating a new container.

            container_t *c;
            if (type1 == SHARED_CONTAINER_TYPE) {
                c = container_xor(c1, type1, c2, type2, &result_type);
                shared_container_free(CAST_shared(c1));  // so release
            } else {
                c = container_ixor(c1, type1, c2, type2, &result_type);
            }

            if (container_nonzero_cardinality(c, result_type)) {
                ra_set_container_at_index(&x1->high_low_container, pos1, c,
                                          result_type);
                ++pos1;
            } else {
                container_free(c, result_type);
                ra_remove_at_index(&x1->high_low_container, pos1);
                --length1;
            }

            ++pos2;
            if (pos1 == length1) break;
            if (pos2 == length2) break;
            s1 = ra_get_key_at_index(&x1->high_low_container, (uint16_t)pos1);
            s2 = ra_get_key_at_index(&x2->high_low_container, (uint16_t)pos2);

        } else if (s1 < s2) {  // s1 < s2
            pos1++;
            if (pos1 == length1) break;
            s1 = ra_get_key_at_index(&x1->high_low_container, (uint16_t)pos1);

        } else {  // s1 > s2
            container_t *c2 = ra_get_container_at_index(&x2->high_low_container,
                                                        (uint16_t)pos2, &type2);
            c2 = get_copy_of_container(c2, &type2, is_cow(x2));
            if (is_cow(x2)) {
                ra_set_container_at_index(&x2->high_low_container, pos2, c2,
                                          type2);
            }

            ra_insert_new_key_value_at(&x1->high_low_container, pos1, s2, c2,
                                       type2);
            pos1++;
            length1++;
            pos2++;
            if (pos2 == length2) break;
            s2 = ra_get_key_at_index(&x2->high_low_container, (uint16_t)pos2);
        }
    }
    if (pos1 == length1) {
        ra_append_copy_range(&x1->high_low_container, &x2->high_low_container,
                             pos2, length2, is_cow(x2));
    }
}

roaring_bitmap_t *roaring_bitmap_andnot(const roaring_bitmap_t *x1,
                                        const roaring_bitmap_t *x2) {
    uint8_t result_type = 0;
    const int length1 = x1->high_low_container.size,
              length2 = x2->high_low_container.size;
    if (0 == length1) {
        roaring_bitmap_t *empty_bitmap = roaring_bitmap_create();
        roaring_bitmap_set_copy_on_write(empty_bitmap,
                                         is_cow(x1) || is_cow(x2));
        return empty_bitmap;
    }
    if (0 == length2) {
        return roaring_bitmap_copy(x1);
    }
    roaring_bitmap_t *answer = roaring_bitmap_create_with_capacity(length1);
    roaring_bitmap_set_copy_on_write(answer, is_cow(x1) || is_cow(x2));

    int pos1 = 0, pos2 = 0;
    uint8_t type1, type2;
    uint16_t s1 = 0;
    uint16_t s2 = 0;
    while (true) {
        s1 = ra_get_key_at_index(&x1->high_low_container, (uint16_t)pos1);
        s2 = ra_get_key_at_index(&x2->high_low_container, (uint16_t)pos2);

        if (s1 == s2) {
            container_t *c1 = ra_get_container_at_index(&x1->high_low_container,
                                                        (uint16_t)pos1, &type1);
            container_t *c2 = ra_get_container_at_index(&x2->high_low_container,
                                                        (uint16_t)pos2, &type2);
            container_t *c =
                container_andnot(c1, type1, c2, type2, &result_type);

            if (container_nonzero_cardinality(c, result_type)) {
                ra_append(&answer->high_low_container, s1, c, result_type);
            } else {
                container_free(c, result_type);
            }
            ++pos1;
            ++pos2;
            if (pos1 == length1) break;
            if (pos2 == length2) break;
        } else if (s1 < s2) {  // s1 < s2
            const int next_pos1 =
                ra_advance_until(&x1->high_low_container, s2, pos1);
            ra_append_copy_range(&answer->high_low_container,
                                 &x1->high_low_container, pos1, next_pos1,
                                 is_cow(x1));
            // TODO : perhaps some of the copy_on_write should be based on
            // answer rather than x1 (more stringent?).  Many similar cases
            pos1 = next_pos1;
            if (pos1 == length1) break;
        } else {  // s1 > s2
            pos2 = ra_advance_until(&x2->high_low_container, s1, pos2);
            if (pos2 == length2) break;
        }
    }
    if (pos2 == length2) {
        ra_append_copy_range(&answer->high_low_container,
                             &x1->high_low_container, pos1, length1,
                             is_cow(x1));
    }
    return answer;
}

// inplace andnot (modifies its first argument).

void roaring_bitmap_andnot_inplace(roaring_bitmap_t *x1,
                                   const roaring_bitmap_t *x2) {
    assert(x1 != x2);

    uint8_t result_type = 0;
    int length1 = x1->high_low_container.size;
    const int length2 = x2->high_low_container.size;
    int intersection_size = 0;

    if (0 == length2) return;

    if (0 == length1) {
        roaring_bitmap_clear(x1);
        return;
    }

    int pos1 = 0, pos2 = 0;
    uint8_t type1, type2;
    uint16_t s1 = ra_get_key_at_index(&x1->high_low_container, (uint16_t)pos1);
    uint16_t s2 = ra_get_key_at_index(&x2->high_low_container, (uint16_t)pos2);
    while (true) {
        if (s1 == s2) {
            container_t *c1 = ra_get_container_at_index(&x1->high_low_container,
                                                        (uint16_t)pos1, &type1);
            container_t *c2 = ra_get_container_at_index(&x2->high_low_container,
                                                        (uint16_t)pos2, &type2);

            // We do the computation "in place" only when c1 is not a shared
            // container. Rationale: using a shared container safely with in
            // place computation would require making a copy and then doing the
            // computation in place which is likely less efficient than avoiding
            // in place entirely and always generating a new container.

            container_t *c;
            if (type1 == SHARED_CONTAINER_TYPE) {
                c = container_andnot(c1, type1, c2, type2, &result_type);
                shared_container_free(CAST_shared(c1));  // release
            } else {
                c = container_iandnot(c1, type1, c2, type2, &result_type);
            }

            if (container_nonzero_cardinality(c, result_type)) {
                ra_replace_key_and_container_at_index(&x1->high_low_container,
                                                      intersection_size++, s1,
                                                      c, result_type);
            } else {
                container_free(c, result_type);
            }

            ++pos1;
            ++pos2;
            if (pos1 == length1) break;
            if (pos2 == length2) break;
            s1 = ra_get_key_at_index(&x1->high_low_container, (uint16_t)pos1);
            s2 = ra_get_key_at_index(&x2->high_low_container, (uint16_t)pos2);

        } else if (s1 < s2) {  // s1 < s2
            if (pos1 != intersection_size) {
                container_t *c1 = ra_get_container_at_index(
                    &x1->high_low_container, (uint16_t)pos1, &type1);

                ra_replace_key_and_container_at_index(
                    &x1->high_low_container, intersection_size, s1, c1, type1);
            }
            intersection_size++;
            pos1++;
            if (pos1 == length1) break;
            s1 = ra_get_key_at_index(&x1->high_low_container, (uint16_t)pos1);

        } else {  // s1 > s2
            pos2 = ra_advance_until(&x2->high_low_container, s1, pos2);
            if (pos2 == length2) break;
            s2 = ra_get_key_at_index(&x2->high_low_container, (uint16_t)pos2);
        }
    }

    if (pos1 < length1) {
        // all containers between intersection_size and
        // pos1 are junk.  However, they have either been moved
        // (thus still referenced) or involved in an iandnot
        // that will clean up all containers that could not be reused.
        // Thus we should not free the junk containers between
        // intersection_size and pos1.
        if (pos1 > intersection_size) {
            // left slide of remaining items
            ra_copy_range(&x1->high_low_container, pos1, length1,
                          intersection_size);
        }
        // else current placement is fine
        intersection_size += (length1 - pos1);
    }
    ra_downsize(&x1->high_low_container, intersection_size);
}

uint64_t roaring_bitmap_get_cardinality(const roaring_bitmap_t *r) {
    const roaring_array_t *ra = &r->high_low_container;

    uint64_t card = 0;
    for (int i = 0; i < ra->size; ++i)
        card += container_get_cardinality(ra->containers[i], ra->typecodes[i]);
    return card;
}

uint64_t roaring_bitmap_range_cardinality(const roaring_bitmap_t *r,
                                          uint64_t range_start,
                                          uint64_t range_end) {
    if (range_start >= range_end || range_start > (uint64_t)UINT32_MAX + 1) {
        return 0;
    }
    return roaring_bitmap_range_cardinality_closed(r, (uint32_t)range_start,
                                                   (uint32_t)(range_end - 1));
}

uint64_t roaring_bitmap_range_cardinality_closed(const roaring_bitmap_t *r,
                                                 uint32_t range_start,
                                                 uint32_t range_end) {
    const roaring_array_t *ra = &r->high_low_container;

    if (range_start > range_end) {
        return 0;
    }

    // now we have: 0 <= range_start <= range_end <= UINT32_MAX

    uint16_t minhb = (uint16_t)(range_start >> 16);
    uint16_t maxhb = (uint16_t)(range_end >> 16);

    uint64_t card = 0;

    int i = ra_get_index(ra, minhb);
    if (i >= 0) {
        if (minhb == maxhb) {
            card += container_rank(ra->containers[i], ra->typecodes[i],
                                   range_end & 0xffff);
        } else {
            card +=
                container_get_cardinality(ra->containers[i], ra->typecodes[i]);
        }
        if ((range_start & 0xffff) != 0) {
            card -= container_rank(ra->containers[i], ra->typecodes[i],
                                   (range_start & 0xffff) - 1);
        }
        i++;
    } else {
        i = -i - 1;
    }

    for (; i < ra->size; i++) {
        uint16_t key = ra->keys[i];
        if (key < maxhb) {
            card +=
                container_get_cardinality(ra->containers[i], ra->typecodes[i]);
        } else if (key == maxhb) {
            card += container_rank(ra->containers[i], ra->typecodes[i],
                                   range_end & 0xffff);
            break;
        } else {
            break;
        }
    }

    return card;
}

bool roaring_bitmap_is_empty(const roaring_bitmap_t *r) {
    return r->high_low_container.size == 0;
}

void roaring_bitmap_to_uint32_array(const roaring_bitmap_t *r, uint32_t *ans) {
    ra_to_uint32_array(&r->high_low_container, ans);
}

bool roaring_bitmap_range_uint32_array(const roaring_bitmap_t *r, size_t offset,
                                       size_t limit, uint32_t *ans) {
    roaring_uint32_iterator_t it;
    roaring_iterator_init(r, &it);
    roaring_uint32_iterator_skip(&it, offset);
    roaring_uint32_iterator_read(&it, ans, limit);

    // This function always succeeds
    return true;
}

/** convert array and bitmap containers to run containers when it is more
 * efficient;
 * also convert from run containers when more space efficient.  Returns
 * true if the result has at least one run container.
 */
bool roaring_bitmap_run_optimize(roaring_bitmap_t *r) {
    bool answer = false;
    for (int i = 0; i < r->high_low_container.size; i++) {
        uint8_t type_original, type_after;
        ra_unshare_container_at_index(
            &r->high_low_container,
            (uint16_t)i);  // TODO: this introduces extra cloning!
        container_t *c = ra_get_container_at_index(&r->high_low_container,
                                                   (uint16_t)i, &type_original);
        container_t *c1 = convert_run_optimize(c, type_original, &type_after);
        if (type_after == RUN_CONTAINER_TYPE) {
            answer = true;
        }
        ra_set_container_at_index(&r->high_low_container, i, c1, type_after);
    }
    return answer;
}

size_t roaring_bitmap_shrink_to_fit(roaring_bitmap_t *r) {
    size_t answer = 0;
    for (int i = 0; i < r->high_low_container.size; i++) {
        uint8_t type_original;
        container_t *c = ra_get_container_at_index(&r->high_low_container,
                                                   (uint16_t)i, &type_original);
        answer += container_shrink_to_fit(c, type_original);
    }
    answer += ra_shrink_to_fit(&r->high_low_container);
    return answer;
}

/**
 *  Remove run-length encoding even when it is more space efficient
 *  return whether a change was applied
 */
bool roaring_bitmap_remove_run_compression(roaring_bitmap_t *r) {
    bool answer = false;
    for (int i = 0; i < r->high_low_container.size; i++) {
        uint8_t type_original, type_after;
        container_t *c = ra_get_container_at_index(&r->high_low_container,
                                                   (uint16_t)i, &type_original);
        if (get_container_type(c, type_original) == RUN_CONTAINER_TYPE) {
            answer = true;
            if (type_original == SHARED_CONTAINER_TYPE) {
                run_container_t *truec = CAST_run(CAST_shared(c)->container);
                int32_t card = run_container_cardinality(truec);
                container_t *c1 = convert_to_bitset_or_array_container(
                    truec, card, &type_after);
                shared_container_free(CAST_shared(c));  // frees run as needed
                ra_set_container_at_index(&r->high_low_container, i, c1,
                                          type_after);

            } else {
                int32_t card = run_container_cardinality(CAST_run(c));
                container_t *c1 = convert_to_bitset_or_array_container(
                    CAST_run(c), card, &type_after);
                run_container_free(CAST_run(c));
                ra_set_container_at_index(&r->high_low_container, i, c1,
                                          type_after);
            }
        }
    }
    return answer;
}

size_t roaring_bitmap_serialize(const roaring_bitmap_t *r, char *buf) {
    size_t portablesize = roaring_bitmap_portable_size_in_bytes(r);
    uint64_t cardinality = roaring_bitmap_get_cardinality(r);
    uint64_t sizeasarray = cardinality * sizeof(uint32_t) + sizeof(uint32_t);
    if (portablesize < sizeasarray) {
        buf[0] = CROARING_SERIALIZATION_CONTAINER;
        return roaring_bitmap_portable_serialize(r, buf + 1) + 1;
    } else {
        buf[0] = CROARING_SERIALIZATION_ARRAY_UINT32;
        memcpy(buf + 1, &cardinality, sizeof(uint32_t));
        roaring_bitmap_to_uint32_array(
            r, (uint32_t *)(buf + 1 + sizeof(uint32_t)));
        return 1 + (size_t)sizeasarray;
    }
}

size_t roaring_bitmap_size_in_bytes(const roaring_bitmap_t *r) {
    size_t portablesize = roaring_bitmap_portable_size_in_bytes(r);
    uint64_t sizeasarray =
        roaring_bitmap_get_cardinality(r) * sizeof(uint32_t) + sizeof(uint32_t);
    return portablesize < sizeasarray ? portablesize + 1
                                      : (size_t)sizeasarray + 1;
}

size_t roaring_bitmap_portable_size_in_bytes(const roaring_bitmap_t *r) {
    return ra_portable_size_in_bytes(&r->high_low_container);
}

roaring_bitmap_t *roaring_bitmap_portable_deserialize_safe(const char *buf,
                                                           size_t maxbytes) {
    roaring_bitmap_t *ans =
        (roaring_bitmap_t *)roaring_malloc(sizeof(roaring_bitmap_t));
    if (ans == NULL) {
        return NULL;
    }
    size_t bytesread;
    bool is_ok = ra_portable_deserialize(&ans->high_low_container, buf,
                                         maxbytes, &bytesread);
    if (!is_ok) {
        roaring_free(ans);
        return NULL;
    }
    roaring_bitmap_set_copy_on_write(ans, false);
    if (!is_ok) {
        roaring_free(ans);
        return NULL;
    }
    return ans;
}

roaring_bitmap_t *roaring_bitmap_portable_deserialize(const char *buf) {
    return roaring_bitmap_portable_deserialize_safe(buf, SIZE_MAX);
}

size_t roaring_bitmap_portable_deserialize_size(const char *buf,
                                                size_t maxbytes) {
    return ra_portable_deserialize_size(buf, maxbytes);
}

size_t roaring_bitmap_portable_serialize(const roaring_bitmap_t *r, char *buf) {
    return ra_portable_serialize(&r->high_low_container, buf);
}

roaring_bitmap_t *roaring_bitmap_deserialize(const void *buf) {
    const char *bufaschar = (const char *)buf;
    if (bufaschar[0] == CROARING_SERIALIZATION_ARRAY_UINT32) {
        /* This looks like a compressed set of uint32_t elements */
        uint32_t card;

        memcpy(&card, bufaschar + 1, sizeof(uint32_t));

        const uint32_t *elems =
            (const uint32_t *)(bufaschar + 1 + sizeof(uint32_t));

        roaring_bitmap_t *bitmap = roaring_bitmap_create();
        if (bitmap == NULL) {
            return NULL;
        }
        roaring_bulk_context_t context = CROARING_ZERO_INITIALIZER;
        for (uint32_t i = 0; i < card; i++) {
            // elems may not be aligned, read with memcpy
            uint32_t elem;
            memcpy(&elem, elems + i, sizeof(elem));
            roaring_bitmap_add_bulk(bitmap, &context, elem);
        }
        return bitmap;

    } else if (bufaschar[0] == CROARING_SERIALIZATION_CONTAINER) {
        return roaring_bitmap_portable_deserialize(bufaschar + 1);
    } else
        return (NULL);
}

roaring_bitmap_t *roaring_bitmap_deserialize_safe(const void *buf,
                                                  size_t maxbytes) {
    if (maxbytes < 1) {
        return NULL;
    }

    const char *bufaschar = (const char *)buf;
    if (bufaschar[0] == CROARING_SERIALIZATION_ARRAY_UINT32) {
        if (maxbytes < 1 + sizeof(uint32_t)) {
            return NULL;
        }

        /* This looks like a compressed set of uint32_t elements */
        uint32_t card;
        memcpy(&card, bufaschar + 1, sizeof(uint32_t));

        // Check the buffer is big enough to contain card uint32_t elements
        if (maxbytes < 1 + sizeof(uint32_t) + card * sizeof(uint32_t)) {
            return NULL;
        }

        const uint32_t *elems =
            (const uint32_t *)(bufaschar + 1 + sizeof(uint32_t));

        roaring_bitmap_t *bitmap = roaring_bitmap_create();
        if (bitmap == NULL) {
            return NULL;
        }
        roaring_bulk_context_t context = CROARING_ZERO_INITIALIZER;
        for (uint32_t i = 0; i < card; i++) {
            // elems may not be aligned, read with memcpy
            uint32_t elem;
            memcpy((char *)&elem, (char *)(elems + i), sizeof(elem));
            roaring_bitmap_add_bulk(bitmap, &context, elem);
        }
        return bitmap;

    } else if (bufaschar[0] == CROARING_SERIALIZATION_CONTAINER) {
        return roaring_bitmap_portable_deserialize_safe(bufaschar + 1,
                                                        maxbytes - 1);
    } else
        return (NULL);
}

bool roaring_iterate(const roaring_bitmap_t *r, roaring_iterator iterator,
                     void *ptr) {
    const roaring_array_t *ra = &r->high_low_container;

    for (int i = 0; i < ra->size; ++i)
        if (!container_iterate(ra->containers[i], ra->typecodes[i],
                               ((uint32_t)ra->keys[i]) << 16, iterator, ptr)) {
            return false;
        }
    return true;
}

bool roaring_iterate64(const roaring_bitmap_t *r, roaring_iterator64 iterator,
                       uint64_t high_bits, void *ptr) {
    const roaring_array_t *ra = &r->high_low_container;

    for (int i = 0; i < ra->size; ++i)
        if (!container_iterate64(ra->containers[i], ra->typecodes[i],
                                 ((uint32_t)ra->keys[i]) << 16, iterator,
                                 high_bits, ptr)) {
            return false;
        }
    return true;
}

/****
 * begin roaring_uint32_iterator_t
 *****/

/**
 * Partially initializes the iterator. Leaves it in either state:
 * 1. Invalid due to `has_value = false`, or
 * 2. At a container, with the high bits set, `has_value = true`.
 */
CROARING_WARN_UNUSED static bool iter_new_container_partial_init(
    roaring_uint32_iterator_t *newit) {
    newit->current_value = 0;
    if (newit->container_index >= newit->parent->high_low_container.size ||
        newit->container_index < 0) {
        newit->current_value = UINT32_MAX;
        return (newit->has_value = false);
    }
    newit->has_value = true;
    // we precompute container, typecode and highbits so that successive
    // iterators do not have to grab them from odd memory locations
    // and have to worry about the (easily predicted) container_unwrap_shared
    // call.
    newit->container =
        newit->parent->high_low_container.containers[newit->container_index];
    newit->typecode =
        newit->parent->high_low_container.typecodes[newit->container_index];
    newit->highbits =
        ((uint32_t)
             newit->parent->high_low_container.keys[newit->container_index])
        << 16;
    newit->container =
        container_unwrap_shared(newit->container, &(newit->typecode));
    return true;
}

/**
 * Positions the iterator at the first value of the current container that the
 * iterator points at, if available.
 */
CROARING_WARN_UNUSED static bool loadfirstvalue(
    roaring_uint32_iterator_t *newit) {
    if (iter_new_container_partial_init(newit)) {
        uint16_t value = 0;
        newit->container_it =
            container_init_iterator(newit->container, newit->typecode, &value);
        newit->current_value = newit->highbits | value;
    }
    return newit->has_value;
}

/**
 * Positions the iterator at the last value of the current container that the
 * iterator points at, if available.
 */
CROARING_WARN_UNUSED static bool loadlastvalue(
    roaring_uint32_iterator_t *newit) {
    if (iter_new_container_partial_init(newit)) {
        uint16_t value = 0;
        newit->container_it = container_init_iterator_last(
            newit->container, newit->typecode, &value);
        newit->current_value = newit->highbits | value;
    }
    return newit->has_value;
}

/**
 * Positions the iterator at the smallest value that is larger than or equal to
 * `val` within the current container that the iterator points at. Assumes such
 * a value exists within the current container.
 */
CROARING_WARN_UNUSED static bool loadfirstvalue_largeorequal(
    roaring_uint32_iterator_t *newit, uint32_t val) {
    bool partial_init = iter_new_container_partial_init(newit);
    assert(partial_init);
    if (!partial_init) {
        return false;
    }
    uint16_t value = 0;
    newit->container_it =
        container_init_iterator(newit->container, newit->typecode, &value);
    bool found = container_iterator_lower_bound(
        newit->container, newit->typecode, &newit->container_it, &value,
        val & 0xFFFF);
    assert(found);
    if (!found) {
        return false;
    }
    newit->current_value = newit->highbits | value;
    return true;
}

void roaring_iterator_init(const roaring_bitmap_t *r,
                           roaring_uint32_iterator_t *newit) {
    newit->parent = r;
    newit->container_index = 0;
    newit->has_value = loadfirstvalue(newit);
}

void roaring_iterator_init_last(const roaring_bitmap_t *r,
                                roaring_uint32_iterator_t *newit) {
    newit->parent = r;
    newit->container_index = newit->parent->high_low_container.size - 1;
    newit->has_value = loadlastvalue(newit);
}

roaring_uint32_iterator_t *roaring_iterator_create(const roaring_bitmap_t *r) {
    roaring_uint32_iterator_t *newit =
        (roaring_uint32_iterator_t *)roaring_malloc(
            sizeof(roaring_uint32_iterator_t));
    if (newit == NULL) return NULL;
    roaring_iterator_init(r, newit);
    return newit;
}

roaring_uint32_iterator_t *roaring_uint32_iterator_copy(
    const roaring_uint32_iterator_t *it) {
    roaring_uint32_iterator_t *newit =
        (roaring_uint32_iterator_t *)roaring_malloc(
            sizeof(roaring_uint32_iterator_t));
    memcpy(newit, it, sizeof(roaring_uint32_iterator_t));
    return newit;
}

bool roaring_uint32_iterator_move_equalorlarger(roaring_uint32_iterator_t *it,
                                                uint32_t val) {
    uint16_t hb = val >> 16;
    const int i = ra_get_index(&it->parent->high_low_container, hb);
    if (i >= 0) {
        uint32_t lowvalue =
            container_maximum(it->parent->high_low_container.containers[i],
                              it->parent->high_low_container.typecodes[i]);
        uint16_t lb = val & 0xFFFF;
        if (lowvalue < lb) {
            // will have to load first value of next container
            it->container_index = i + 1;
        } else {
            // the value is necessarily within the range of the container
            it->container_index = i;
            it->has_value = loadfirstvalue_largeorequal(it, val);
            return it->has_value;
        }
    } else {
        // there is no matching, so we are going for the next container
        it->container_index = -i - 1;
    }
    it->has_value = loadfirstvalue(it);
    return it->has_value;
}

bool roaring_uint32_iterator_advance(roaring_uint32_iterator_t *it) {
    if (it->container_index >= it->parent->high_low_container.size) {
        return (it->has_value = false);
    }
    if (it->container_index < 0) {
        it->container_index = 0;
        return (it->has_value = loadfirstvalue(it));
    }
    uint16_t low16 = (uint16_t)it->current_value;
    if (container_iterator_next(it->container, it->typecode, &it->container_it,
                                &low16)) {
        it->current_value = it->highbits | low16;
        return (it->has_value = true);
    }
    it->container_index++;
    return (it->has_value = loadfirstvalue(it));
}

bool roaring_uint32_iterator_previous(roaring_uint32_iterator_t *it) {
    if (it->container_index < 0) {
        return (it->has_value = false);
    }
    if (it->container_index >= it->parent->high_low_container.size) {
        it->container_index = it->parent->high_low_container.size - 1;
        return (it->has_value = loadlastvalue(it));
    }
    uint16_t low16 = (uint16_t)it->current_value;
    if (container_iterator_prev(it->container, it->typecode, &it->container_it,
                                &low16)) {
        it->current_value = it->highbits | low16;
        return (it->has_value = true);
    }
    it->container_index--;
    return (it->has_value = loadlastvalue(it));
}

uint32_t roaring_uint32_iterator_read(roaring_uint32_iterator_t *it,
                                      uint32_t *buf, uint32_t count) {
    uint32_t ret = 0;
    while (it->has_value && ret < count) {
        uint32_t consumed;
        uint16_t low16 = (uint16_t)it->current_value;
        bool has_value = container_iterator_read_into_uint32(
            it->container, it->typecode, &it->container_it, it->highbits, buf,
            count - ret, &consumed, &low16);
        ret += consumed;
        buf += consumed;
        if (has_value) {
            it->has_value = true;
            it->current_value = it->highbits | low16;
            // If the container still has values, we must have stopped because
            // we read enough values.
            assert(ret == count);
            return ret;
        }
        it->container_index++;
        it->has_value = loadfirstvalue(it);
    }
    return ret;
}

uint32_t roaring_uint32_iterator_read_backward(roaring_uint32_iterator_t *it,
                                               uint32_t *buf, uint32_t count) {
    uint32_t ret = 0;
    while (it->has_value && ret < count) {
        uint32_t consumed;
        uint16_t low16 = (uint16_t)it->current_value;
        bool has_value = container_iterator_read_backward_into_uint32(
            it->container, it->typecode, &it->container_it, it->highbits, buf,
            count - ret, &consumed, &low16);
        ret += consumed;
        buf += consumed;
        if (has_value) {
            it->has_value = true;
            it->current_value = it->highbits | low16;
            // If the container still has values, we must have stopped because
            // we read enough values.
            assert(ret == count);
            return ret;
        }
        it->container_index--;
        it->has_value = loadlastvalue(it);
    }
    return ret;
}

uint32_t roaring_uint32_iterator_skip(roaring_uint32_iterator_t *it,
                                      uint32_t count) {
    uint32_t ret = 0;
    while (it->has_value && ret < count) {
        uint32_t consumed;
        uint16_t low16 = (uint16_t)it->current_value;
        bool has_value = container_iterator_skip(it->container, it->typecode,
                                                 &it->container_it, count - ret,
                                                 &consumed, &low16);
        ret += consumed;
        if (has_value) {
            it->has_value = true;
            it->current_value = it->highbits | low16;
            // If the container still has values, we must have stopped because
            // we skipped enough values.
            assert(ret == count);
            return ret;
        }
        // We have skipped over all items in the current container, so set
        // ourselves at the first item of the next container.
        // We do NOT need to count another item skipped here.
        it->container_index++;
        it->has_value = loadfirstvalue(it);
    }
    return ret;
}

uint32_t roaring_uint32_iterator_skip_backward(roaring_uint32_iterator_t *it,
                                               uint32_t count) {
    uint32_t ret = 0;
    while (it->has_value && ret < count) {
        uint32_t consumed;
        uint16_t low16 = (uint16_t)it->current_value;
        bool has_value = container_iterator_skip_backward(
            it->container, it->typecode, &it->container_it, count - ret,
            &consumed, &low16);
        ret += consumed;
        if (has_value) {
            it->has_value = true;
            it->current_value = it->highbits | low16;
            return ret;
        }
        // We have skipped over all items in the current container backwards.
        // Moving to the previous container counts as consuming one more skip.
        it->container_index--;
        it->has_value = loadlastvalue(it);
    }
    return ret;
}

size_t roaring_uint32_iterator_read_ranges(roaring_uint32_iterator_t *it,
                                           roaring_uint32_range_closed_t *buf,
                                           size_t count) {
    size_t ret = 0;
    while (it->has_value && ret < count) {
        buf[ret].min = it->current_value;
        for (;;) {
            uint16_t low16 = (uint16_t)it->current_value;
            bool container_has_more;
            uint16_t run_end_low16 = container_iterator_find_run_end(
                it->container, it->typecode, &it->container_it, &low16,
                &container_has_more);
            buf[ret].max = it->highbits | run_end_low16;

            if (container_has_more) {
                it->current_value = it->highbits | low16;
                break;
            }
            // Move to next container
            it->container_index++;
            it->has_value = loadfirstvalue(it);
            // Continue merging only if the run reached the container
            // boundary and the next container starts exactly at max+1.
            if (run_end_low16 != UINT16_MAX || !it->has_value ||
                it->current_value != buf[ret].max + 1) {
                break;
            }
        }
        ret++;
    }
    return ret;
}

size_t roaring_uint32_iterator_read_prev_ranges(
    roaring_uint32_iterator_t *it, roaring_uint32_range_closed_t *buf,
    size_t count) {
    size_t ret = 0;
    while (it->has_value && ret < count) {
        buf[ret].max = it->current_value;
        for (;;) {
            uint16_t low16 = (uint16_t)it->current_value;
            bool container_has_more;
            uint16_t run_start_low16 = container_iterator_find_run_start(
                it->container, it->typecode, &it->container_it, &low16,
                &container_has_more);
            buf[ret].min = it->highbits | run_start_low16;

            if (container_has_more) {
                it->current_value = it->highbits | low16;
                break;
            }
            // Move to previous container
            it->container_index--;
            it->has_value = loadlastvalue(it);
            // Continue merging only if the run reached the container
            // boundary and the previous container ends exactly at min-1.
            if (run_start_low16 != 0 || !it->has_value ||
                it->current_value != buf[ret].min - 1) {
                break;
            }
        }
        ret++;
    }
    return ret;
}

void roaring_uint32_iterator_free(roaring_uint32_iterator_t *it) {
    roaring_free(it);
}

/****
 * end of roaring_uint32_iterator_t
 *****/

bool roaring_bitmap_equals(const roaring_bitmap_t *r1,
                           const roaring_bitmap_t *r2) {
    const roaring_array_t *ra1 = &r1->high_low_container;
    const roaring_array_t *ra2 = &r2->high_low_container;

    if (ra1->size != ra2->size) {
        return false;
    }
    for (int i = 0; i < ra1->size; ++i) {
        if (ra1->keys[i] != ra2->keys[i]) {
            return false;
        }
    }
    for (int i = 0; i < ra1->size; ++i) {
        bool areequal = container_equals(ra1->containers[i], ra1->typecodes[i],
                                         ra2->containers[i], ra2->typecodes[i]);
        if (!areequal) {
            return false;
        }
    }
    return true;
}

bool roaring_bitmap_is_subset(const roaring_bitmap_t *r1,
                              const roaring_bitmap_t *r2) {
    const roaring_array_t *ra1 = &r1->high_low_container;
    const roaring_array_t *ra2 = &r2->high_low_container;

    const int length1 = ra1->size, length2 = ra2->size;

    int pos1 = 0, pos2 = 0;

    while (pos1 < length1 && pos2 < length2) {
        const uint16_t s1 = ra_get_key_at_index(ra1, (uint16_t)pos1);
        const uint16_t s2 = ra_get_key_at_index(ra2, (uint16_t)pos2);

        if (s1 == s2) {
            uint8_t type1, type2;
            container_t *c1 =
                ra_get_container_at_index(ra1, (uint16_t)pos1, &type1);
            container_t *c2 =
                ra_get_container_at_index(ra2, (uint16_t)pos2, &type2);
            if (!container_is_subset(c1, type1, c2, type2)) return false;
            ++pos1;
            ++pos2;
        } else if (s1 < s2) {  // s1 < s2
            return false;
        } else {  // s1 > s2
            pos2 = ra_advance_until(ra2, s1, pos2);
        }
    }
    if (pos1 == length1)
        return true;
    else
        return false;
}

static void insert_flipped_container(roaring_array_t *ans_arr,
                                     const roaring_array_t *x1_arr, uint16_t hb,
                                     uint16_t lb_start, uint16_t lb_end) {
    const int i = ra_get_index(x1_arr, hb);
    const int j = ra_get_index(ans_arr, hb);
    uint8_t ctype_in, ctype_out;
    container_t *flipped_container = NULL;
    if (i >= 0) {
        container_t *container_to_flip =
            ra_get_container_at_index(x1_arr, (uint16_t)i, &ctype_in);
        flipped_container =
            container_not_range(container_to_flip, ctype_in, (uint32_t)lb_start,
                                (uint32_t)(lb_end + 1), &ctype_out);

        if (container_nonzero_cardinality(flipped_container, ctype_out))
            ra_insert_new_key_value_at(ans_arr, -j - 1, hb, flipped_container,
                                       ctype_out);
        else {
            container_free(flipped_container, ctype_out);
        }
    } else {
        flipped_container = container_range_of_ones(
            (uint32_t)lb_start, (uint32_t)(lb_end + 1), &ctype_out);
        ra_insert_new_key_value_at(ans_arr, -j - 1, hb, flipped_container,
                                   ctype_out);
    }
}

static void inplace_flip_container(roaring_array_t *x1_arr, uint16_t hb,
                                   uint16_t lb_start, uint16_t lb_end) {
    const int i = ra_get_index(x1_arr, hb);
    uint8_t ctype_in, ctype_out;
    container_t *flipped_container = NULL;
    if (i >= 0) {
        container_t *container_to_flip =
            ra_get_container_at_index(x1_arr, (uint16_t)i, &ctype_in);
        flipped_container = container_inot_range(
            container_to_flip, ctype_in, (uint32_t)lb_start,
            (uint32_t)(lb_end + 1), &ctype_out);
        // if a new container was created, the old one was already freed
        if (container_nonzero_cardinality(flipped_container, ctype_out)) {
            ra_set_container_at_index(x1_arr, i, flipped_container, ctype_out);
        } else {
            container_free(flipped_container, ctype_out);
            ra_remove_at_index(x1_arr, i);
        }

    } else {
        flipped_container = container_range_of_ones(
            (uint32_t)lb_start, (uint32_t)(lb_end + 1), &ctype_out);
        ra_insert_new_key_value_at(x1_arr, -i - 1, hb, flipped_container,
                                   ctype_out);
    }
}

static void insert_fully_flipped_container(roaring_array_t *ans_arr,
                                           const roaring_array_t *x1_arr,
                                           uint16_t hb) {
    const int i = ra_get_index(x1_arr, hb);
    const int j = ra_get_index(ans_arr, hb);
    uint8_t ctype_in, ctype_out;
    container_t *flipped_container = NULL;
    if (i >= 0) {
        container_t *container_to_flip =
            ra_get_container_at_index(x1_arr, (uint16_t)i, &ctype_in);
        flipped_container =
            container_not(container_to_flip, ctype_in, &ctype_out);
        if (container_nonzero_cardinality(flipped_container, ctype_out))
            ra_insert_new_key_value_at(ans_arr, -j - 1, hb, flipped_container,
                                       ctype_out);
        else {
            container_free(flipped_container, ctype_out);
        }
    } else {
        flipped_container = container_range_of_ones(0U, 0x10000U, &ctype_out);
        ra_insert_new_key_value_at(ans_arr, -j - 1, hb, flipped_container,
                                   ctype_out);
    }
}

static void inplace_fully_flip_container(roaring_array_t *x1_arr, uint16_t hb) {
    const int i = ra_get_index(x1_arr, hb);
    uint8_t ctype_in, ctype_out;
    container_t *flipped_container = NULL;
    if (i >= 0) {
        container_t *container_to_flip =
            ra_get_container_at_index(x1_arr, (uint16_t)i, &ctype_in);
        flipped_container =
            container_inot(container_to_flip, ctype_in, &ctype_out);

        if (container_nonzero_cardinality(flipped_container, ctype_out)) {
            ra_set_container_at_index(x1_arr, i, flipped_container, ctype_out);
        } else {
            container_free(flipped_container, ctype_out);
            ra_remove_at_index(x1_arr, i);
        }

    } else {
        flipped_container = container_range_of_ones(0U, 0x10000U, &ctype_out);
        ra_insert_new_key_value_at(x1_arr, -i - 1, hb, flipped_container,
                                   ctype_out);
    }
}

roaring_bitmap_t *roaring_bitmap_flip(const roaring_bitmap_t *x1,
                                      uint64_t range_start,
                                      uint64_t range_end) {
    if (range_start >= range_end || range_start > (uint64_t)UINT32_MAX + 1) {
        return roaring_bitmap_copy(x1);
    }
    return roaring_bitmap_flip_closed(x1, (uint32_t)range_start,
                                      (uint32_t)(range_end - 1));
}

roaring_bitmap_t *roaring_bitmap_flip_closed(const roaring_bitmap_t *x1,
                                             uint32_t range_start,
                                             uint32_t range_end) {
    if (range_start > range_end) {
        return roaring_bitmap_copy(x1);
    }

    roaring_bitmap_t *ans = roaring_bitmap_create();
    roaring_bitmap_set_copy_on_write(ans, is_cow(x1));

    uint16_t hb_start = (uint16_t)(range_start >> 16);
    const uint16_t lb_start = (uint16_t)range_start;  // & 0xFFFF;
    uint16_t hb_end = (uint16_t)(range_end >> 16);
    const uint16_t lb_end = (uint16_t)range_end;  // & 0xFFFF;

    ra_append_copies_until(&ans->high_low_container, &x1->high_low_container,
                           hb_start, is_cow(x1));
    if (hb_start == hb_end) {
        insert_flipped_container(&ans->high_low_container,
                                 &x1->high_low_container, hb_start, lb_start,
                                 lb_end);
    } else {
        // start and end containers are distinct
        if (lb_start > 0) {
            // handle first (partial) container
            insert_flipped_container(&ans->high_low_container,
                                     &x1->high_low_container, hb_start,
                                     lb_start, 0xFFFF);
            ++hb_start;  // for the full containers.  Can't wrap.
        }

        if (lb_end != 0xFFFF) --hb_end;  // later we'll handle the partial block

        for (uint32_t hb = hb_start; hb <= hb_end; ++hb) {
            insert_fully_flipped_container(&ans->high_low_container,
                                           &x1->high_low_container,
                                           (uint16_t)hb);
        }

        // handle a partial final container
        if (lb_end != 0xFFFF) {
            insert_flipped_container(&ans->high_low_container,
                                     &x1->high_low_container, hb_end + 1, 0,
                                     lb_end);
            ++hb_end;
        }
    }
    ra_append_copies_after(&ans->high_low_container, &x1->high_low_container,
                           hb_end, is_cow(x1));
    return ans;
}

void roaring_bitmap_flip_inplace(roaring_bitmap_t *x1, uint64_t range_start,
                                 uint64_t range_end) {
    if (range_start >= range_end || range_start > (uint64_t)UINT32_MAX + 1) {
        return;
    }
    roaring_bitmap_flip_inplace_closed(x1, (uint32_t)range_start,
                                       (uint32_t)(range_end - 1));
}

void roaring_bitmap_flip_inplace_closed(roaring_bitmap_t *x1,
                                        uint32_t range_start,
                                        uint32_t range_end) {
    if (range_start > range_end) {
        return;  // empty range
    }

    uint16_t hb_start = (uint16_t)(range_start >> 16);
    const uint16_t lb_start = (uint16_t)range_start;
    uint16_t hb_end = (uint16_t)(range_end >> 16);
    const uint16_t lb_end = (uint16_t)range_end;

    if (hb_start == hb_end) {
        inplace_flip_container(&x1->high_low_container, hb_start, lb_start,
                               lb_end);
    } else {
        // start and end containers are distinct
        if (lb_start > 0) {
            // handle first (partial) container
            inplace_flip_container(&x1->high_low_container, hb_start, lb_start,
                                   0xFFFF);
            ++hb_start;  // for the full containers.  Can't wrap.
        }

        if (lb_end != 0xFFFF) --hb_end;

        for (uint32_t hb = hb_start; hb <= hb_end; ++hb) {
            inplace_fully_flip_container(&x1->high_low_container, (uint16_t)hb);
        }
        // handle a partial final container
        if (lb_end != 0xFFFF) {
            inplace_flip_container(&x1->high_low_container, hb_end + 1, 0,
                                   lb_end);
            ++hb_end;
        }
    }
}

static void offset_append_with_merge(roaring_array_t *ra, int k, container_t *c,
                                     uint8_t t) {
    int size = ra_get_size(ra);
    if (size == 0 || ra_get_key_at_index(ra, (uint16_t)(size - 1)) != k) {
        // No merge.
        ra_append(ra, (uint16_t)k, c, t);
        return;
    }

    uint8_t last_t, new_t;
    container_t *last_c, *new_c;

    // NOTE: we don't need to unwrap here, since we added last_c ourselves
    // we have the certainty it's not a shared container.
    // The same applies to c, as it's the result of calling container_offset.
    last_c = ra_get_container_at_index(ra, (uint16_t)(size - 1), &last_t);
    new_c = container_ior(last_c, last_t, c, t, &new_t);

    ra_set_container_at_index(ra, size - 1, new_c, new_t);

    // Comparison of pointers of different origin is UB (or so claim some
    // compiler makers), so we compare their bit representation only.
    if ((uintptr_t)last_c != (uintptr_t)new_c) {
        container_free(last_c, last_t);
    }
    container_free(c, t);
}

// roaring_bitmap_add_offset adds the value 'offset' to each and every value in
// a bitmap, generating a new bitmap in the process. If offset + element is
// outside of the range [0,2^32), that the element will be dropped.
// We need "offset" to be 64 bits because we want to support values
// between -0xFFFFFFFF up to +0xFFFFFFFF.
roaring_bitmap_t *roaring_bitmap_add_offset(const roaring_bitmap_t *bm,
                                            int64_t offset) {
    roaring_bitmap_t *answer;
    roaring_array_t *ans_ra;
    int64_t container_offset;
    uint16_t in_offset;

    const roaring_array_t *bm_ra = &bm->high_low_container;
    int length = bm_ra->size;

    if (offset == 0) {
        return roaring_bitmap_copy(bm);
    }

    container_offset = offset >> 16;
    in_offset = (uint16_t)(offset - container_offset * (1 << 16));

    answer = roaring_bitmap_create();
    bool cow = is_cow(bm);
    roaring_bitmap_set_copy_on_write(answer, cow);

    ans_ra = &answer->high_low_container;

    if (in_offset == 0) {
        ans_ra = &answer->high_low_container;

        for (int i = 0, j = 0; i < length; ++i) {
            int64_t key = ra_get_key_at_index(bm_ra, (uint16_t)i);
            key += container_offset;

            if (key < 0 || key >= (1 << 16)) {
                continue;
            }
            ra_append_copy(ans_ra, bm_ra, (uint16_t)i, cow);
            ans_ra->keys[j++] = (uint16_t)key;
        }
        return answer;
    }

    uint8_t t;
    const container_t *c;
    container_t *lo, *hi, **lo_ptr, **hi_ptr;
    int64_t k;

    for (int i = 0; i < length; ++i) {
        lo = hi = NULL;
        lo_ptr = hi_ptr = NULL;

        k = ra_get_key_at_index(bm_ra, (uint16_t)i) + container_offset;
        if (k >= 0 && k < (1 << 16)) {
            lo_ptr = &lo;
        }
        if (k + 1 >= 0 && k + 1 < (1 << 16)) {
            hi_ptr = &hi;
        }
        if (lo_ptr == NULL && hi_ptr == NULL) {
            continue;
        }
        c = ra_get_container_at_index(bm_ra, (uint16_t)i, &t);
        c = container_unwrap_shared(c, &t);

        container_add_offset(c, t, lo_ptr, hi_ptr, in_offset);
        if (lo != NULL) {
            offset_append_with_merge(ans_ra, (int)k, lo, t);
        }
        if (hi != NULL) {
            ra_append(ans_ra, (uint16_t)(k + 1), hi, t);
        }
        // the `lo` and `hi` container type always keep same as container `c`.
        // in the case of `container_add_offset` on bitset container, `lo` and
        // `hi` may has small cardinality, they must be repaired to array
        // container.
    }

    roaring_bitmap_repair_after_lazy(answer);  // do required type conversions.
    return answer;
}

roaring_bitmap_t *roaring_bitmap_lazy_or(const roaring_bitmap_t *x1,
                                         const roaring_bitmap_t *x2,
                                         const bool bitsetconversion) {
    uint8_t result_type = 0;
    const int length1 = x1->high_low_container.size,
              length2 = x2->high_low_container.size;
    if (0 == length1) {
        return roaring_bitmap_copy(x2);
    }
    if (0 == length2) {
        return roaring_bitmap_copy(x1);
    }
    roaring_bitmap_t *answer =
        roaring_bitmap_create_with_capacity(length1 + length2);
    roaring_bitmap_set_copy_on_write(answer, is_cow(x1) || is_cow(x2));
    int pos1 = 0, pos2 = 0;
    uint8_t type1, type2;
    uint16_t s1 = ra_get_key_at_index(&x1->high_low_container, (uint16_t)pos1);
    uint16_t s2 = ra_get_key_at_index(&x2->high_low_container, (uint16_t)pos2);
    while (true) {
        if (s1 == s2) {
            container_t *c1 = ra_get_container_at_index(&x1->high_low_container,
                                                        (uint16_t)pos1, &type1);
            container_t *c2 = ra_get_container_at_index(&x2->high_low_container,
                                                        (uint16_t)pos2, &type2);
            container_t *c;
            if (bitsetconversion &&
                (get_container_type(c1, type1) != BITSET_CONTAINER_TYPE) &&
                (get_container_type(c2, type2) != BITSET_CONTAINER_TYPE)) {
                container_t *newc1 =
                    container_mutable_unwrap_shared(c1, &type1);
                newc1 = container_to_bitset(newc1, type1);
                type1 = BITSET_CONTAINER_TYPE;
                c = container_lazy_ior(newc1, type1, c2, type2, &result_type);
                if (c != newc1) {  // should not happen
                    container_free(newc1, type1);
                }
            } else {
                c = container_lazy_or(c1, type1, c2, type2, &result_type);
            }
            // since we assume that the initial containers are non-empty,
            // the
            // result here
            // can only be non-empty
            ra_append(&answer->high_low_container, s1, c, result_type);
            ++pos1;
            ++pos2;
            if (pos1 == length1) break;
            if (pos2 == length2) break;
            s1 = ra_get_key_at_index(&x1->high_low_container, (uint16_t)pos1);
            s2 = ra_get_key_at_index(&x2->high_low_container, (uint16_t)pos2);

        } else if (s1 < s2) {  // s1 < s2
            container_t *c1 = ra_get_container_at_index(&x1->high_low_container,
                                                        (uint16_t)pos1, &type1);
            c1 = get_copy_of_container(c1, &type1, is_cow(x1));
            if (is_cow(x1)) {
                ra_set_container_at_index(&x1->high_low_container, pos1, c1,
                                          type1);
            }
            ra_append(&answer->high_low_container, s1, c1, type1);
            pos1++;
            if (pos1 == length1) break;
            s1 = ra_get_key_at_index(&x1->high_low_container, (uint16_t)pos1);

        } else {  // s1 > s2
            container_t *c2 = ra_get_container_at_index(&x2->high_low_container,
                                                        (uint16_t)pos2, &type2);
            c2 = get_copy_of_container(c2, &type2, is_cow(x2));
            if (is_cow(x2)) {
                ra_set_container_at_index(&x2->high_low_container, pos2, c2,
                                          type2);
            }
            ra_append(&answer->high_low_container, s2, c2, type2);
            pos2++;
            if (pos2 == length2) break;
            s2 = ra_get_key_at_index(&x2->high_low_container, (uint16_t)pos2);
        }
    }
    if (pos1 == length1) {
        ra_append_copy_range(&answer->high_low_container,
                             &x2->high_low_container, pos2, length2,
                             is_cow(x2));
    } else if (pos2 == length2) {
        ra_append_copy_range(&answer->high_low_container,
                             &x1->high_low_container, pos1, length1,
                             is_cow(x1));
    }
    return answer;
}

void roaring_bitmap_lazy_or_inplace(roaring_bitmap_t *x1,
                                    const roaring_bitmap_t *x2,
                                    const bool bitsetconversion) {
    uint8_t result_type = 0;
    int length1 = x1->high_low_container.size;
    const int length2 = x2->high_low_container.size;

    if (0 == length2) return;

    if (0 == length1) {
        roaring_bitmap_overwrite(x1, x2);
        return;
    }
    int pos1 = 0, pos2 = 0;
    uint8_t type1, type2;
    uint16_t s1 = ra_get_key_at_index(&x1->high_low_container, (uint16_t)pos1);
    uint16_t s2 = ra_get_key_at_index(&x2->high_low_container, (uint16_t)pos2);
    while (true) {
        if (s1 == s2) {
            container_t *c1 = ra_get_container_at_index(&x1->high_low_container,
                                                        (uint16_t)pos1, &type1);
            if (!container_is_full(c1, type1)) {
                if ((bitsetconversion == false) ||
                    (get_container_type(c1, type1) == BITSET_CONTAINER_TYPE)) {
                    c1 = get_writable_copy_if_shared(c1, &type1);
                } else {
                    // convert to bitset
                    container_t *old_c1 = c1;
                    uint8_t old_type1 = type1;
                    c1 = container_mutable_unwrap_shared(c1, &type1);
                    c1 = container_to_bitset(c1, type1);
                    container_free(old_c1, old_type1);
                    type1 = BITSET_CONTAINER_TYPE;
                }

                container_t *c2 = ra_get_container_at_index(
                    &x2->high_low_container, (uint16_t)pos2, &type2);
                container_t *c =
                    container_lazy_ior(c1, type1, c2, type2, &result_type);

                if (c != c1) {  // in this instance a new container was created,
                                // and we need to free the old one
                    container_free(c1, type1);
                }

                ra_set_container_at_index(&x1->high_low_container, pos1, c,
                                          result_type);
            }
            ++pos1;
            ++pos2;
            if (pos1 == length1) break;
            if (pos2 == length2) break;
            s1 = ra_get_key_at_index(&x1->high_low_container, (uint16_t)pos1);
            s2 = ra_get_key_at_index(&x2->high_low_container, (uint16_t)pos2);

        } else if (s1 < s2) {  // s1 < s2
            pos1++;
            if (pos1 == length1) break;
            s1 = ra_get_key_at_index(&x1->high_low_container, (uint16_t)pos1);

        } else {  // s1 > s2
            container_t *c2 = ra_get_container_at_index(&x2->high_low_container,
                                                        (uint16_t)pos2, &type2);
            // container_t *c2_clone = container_clone(c2, type2);
            c2 = get_copy_of_container(c2, &type2, is_cow(x2));
            if (is_cow(x2)) {
                ra_set_container_at_index(&x2->high_low_container, pos2, c2,
                                          type2);
            }
            ra_insert_new_key_value_at(&x1->high_low_container, pos1, s2, c2,
                                       type2);
            pos1++;
            length1++;
            pos2++;
            if (pos2 == length2) break;
            s2 = ra_get_key_at_index(&x2->high_low_container, (uint16_t)pos2);
        }
    }
    if (pos1 == length1) {
        ra_append_copy_range(&x1->high_low_container, &x2->high_low_container,
                             pos2, length2, is_cow(x2));
    }
}

roaring_bitmap_t *roaring_bitmap_lazy_xor(const roaring_bitmap_t *x1,
                                          const roaring_bitmap_t *x2) {
    uint8_t result_type = 0;
    const int length1 = x1->high_low_container.size,
              length2 = x2->high_low_container.size;
    if (0 == length1) {
        return roaring_bitmap_copy(x2);
    }
    if (0 == length2) {
        return roaring_bitmap_copy(x1);
    }
    roaring_bitmap_t *answer =
        roaring_bitmap_create_with_capacity(length1 + length2);
    roaring_bitmap_set_copy_on_write(answer, is_cow(x1) || is_cow(x2));
    int pos1 = 0, pos2 = 0;
    uint8_t type1, type2;
    uint16_t s1 = ra_get_key_at_index(&x1->high_low_container, (uint16_t)pos1);
    uint16_t s2 = ra_get_key_at_index(&x2->high_low_container, (uint16_t)pos2);
    while (true) {
        if (s1 == s2) {
            container_t *c1 = ra_get_container_at_index(&x1->high_low_container,
                                                        (uint16_t)pos1, &type1);
            container_t *c2 = ra_get_container_at_index(&x2->high_low_container,
                                                        (uint16_t)pos2, &type2);
            container_t *c =
                container_lazy_xor(c1, type1, c2, type2, &result_type);

            if (container_nonzero_cardinality(c, result_type)) {
                ra_append(&answer->high_low_container, s1, c, result_type);
            } else {
                container_free(c, result_type);
            }

            ++pos1;
            ++pos2;
            if (pos1 == length1) break;
            if (pos2 == length2) break;
            s1 = ra_get_key_at_index(&x1->high_low_container, (uint16_t)pos1);
            s2 = ra_get_key_at_index(&x2->high_low_container, (uint16_t)pos2);

        } else if (s1 < s2) {  // s1 < s2
            container_t *c1 = ra_get_container_at_index(&x1->high_low_container,
                                                        (uint16_t)pos1, &type1);
            c1 = get_copy_of_container(c1, &type1, is_cow(x1));
            if (is_cow(x1)) {
                ra_set_container_at_index(&x1->high_low_container, pos1, c1,
                                          type1);
            }
            ra_append(&answer->high_low_container, s1, c1, type1);
            pos1++;
            if (pos1 == length1) break;
            s1 = ra_get_key_at_index(&x1->high_low_container, (uint16_t)pos1);

        } else {  // s1 > s2
            container_t *c2 = ra_get_container_at_index(&x2->high_low_container,
                                                        (uint16_t)pos2, &type2);
            c2 = get_copy_of_container(c2, &type2, is_cow(x2));
            if (is_cow(x2)) {
                ra_set_container_at_index(&x2->high_low_container, pos2, c2,
                                          type2);
            }
            ra_append(&answer->high_low_container, s2, c2, type2);
            pos2++;
            if (pos2 == length2) break;
            s2 = ra_get_key_at_index(&x2->high_low_container, (uint16_t)pos2);
        }
    }
    if (pos1 == length1) {
        ra_append_copy_range(&answer->high_low_container,
                             &x2->high_low_container, pos2, length2,
                             is_cow(x2));
    } else if (pos2 == length2) {
        ra_append_copy_range(&answer->high_low_container,
                             &x1->high_low_container, pos1, length1,
                             is_cow(x1));
    }
    return answer;
}

void roaring_bitmap_lazy_xor_inplace(roaring_bitmap_t *x1,
                                     const roaring_bitmap_t *x2) {
    assert(x1 != x2);
    uint8_t result_type = 0;
    int length1 = x1->high_low_container.size;
    const int length2 = x2->high_low_container.size;

    if (0 == length2) return;

    if (0 == length1) {
        roaring_bitmap_overwrite(x1, x2);
        return;
    }
    int pos1 = 0, pos2 = 0;
    uint8_t type1, type2;
    uint16_t s1 = ra_get_key_at_index(&x1->high_low_container, (uint16_t)pos1);
    uint16_t s2 = ra_get_key_at_index(&x2->high_low_container, (uint16_t)pos2);
    while (true) {
        if (s1 == s2) {
            container_t *c1 = ra_get_container_at_index(&x1->high_low_container,
                                                        (uint16_t)pos1, &type1);
            container_t *c2 = ra_get_container_at_index(&x2->high_low_container,
                                                        (uint16_t)pos2, &type2);

            // We do the computation "in place" only when c1 is not a shared
            // container. Rationale: using a shared container safely with in
            // place computation would require making a copy and then doing the
            // computation in place which is likely less efficient than avoiding
            // in place entirely and always generating a new container.

            container_t *c;
            if (type1 == SHARED_CONTAINER_TYPE) {
                c = container_lazy_xor(c1, type1, c2, type2, &result_type);
                shared_container_free(CAST_shared(c1));  // release
            } else {
                c = container_lazy_ixor(c1, type1, c2, type2, &result_type);
            }

            if (container_nonzero_cardinality(c, result_type)) {
                ra_set_container_at_index(&x1->high_low_container, pos1, c,
                                          result_type);
                ++pos1;
            } else {
                container_free(c, result_type);
                ra_remove_at_index(&x1->high_low_container, pos1);
                --length1;
            }
            ++pos2;
            if (pos1 == length1) break;
            if (pos2 == length2) break;
            s1 = ra_get_key_at_index(&x1->high_low_container, (uint16_t)pos1);
            s2 = ra_get_key_at_index(&x2->high_low_container, (uint16_t)pos2);

        } else if (s1 < s2) {  // s1 < s2
            pos1++;
            if (pos1 == length1) break;
            s1 = ra_get_key_at_index(&x1->high_low_container, (uint16_t)pos1);

        } else {  // s1 > s2
            container_t *c2 = ra_get_container_at_index(&x2->high_low_container,
                                                        (uint16_t)pos2, &type2);
            // container_t *c2_clone = container_clone(c2, type2);
            c2 = get_copy_of_container(c2, &type2, is_cow(x2));
            if (is_cow(x2)) {
                ra_set_container_at_index(&x2->high_low_container, pos2, c2,
                                          type2);
            }
            ra_insert_new_key_value_at(&x1->high_low_container, pos1, s2, c2,
                                       type2);
            pos1++;
            length1++;
            pos2++;
            if (pos2 == length2) break;
            s2 = ra_get_key_at_index(&x2->high_low_container, (uint16_t)pos2);
        }
    }
    if (pos1 == length1) {
        ra_append_copy_range(&x1->high_low_container, &x2->high_low_container,
                             pos2, length2, is_cow(x2));
    }
}

void roaring_bitmap_repair_after_lazy(roaring_bitmap_t *r) {
    roaring_array_t *ra = &r->high_low_container;

    for (int i = 0; i < ra->size; ++i) {
        const uint8_t old_type = ra->typecodes[i];
        container_t *old_c = ra->containers[i];
        uint8_t new_type = old_type;
        container_t *new_c = container_repair_after_lazy(old_c, &new_type);
        ra->containers[i] = new_c;
        ra->typecodes[i] = new_type;
    }
}

/**
 * roaring_bitmap_rank returns the number of integers that are smaller or equal
 * to x.
 */
uint64_t roaring_bitmap_rank(const roaring_bitmap_t *bm, uint32_t x) {
    uint64_t size = 0;
    uint32_t xhigh = x >> 16;
    for (int i = 0; i < bm->high_low_container.size; i++) {
        uint32_t key = bm->high_low_container.keys[i];
        if (xhigh > key) {
            size +=
                container_get_cardinality(bm->high_low_container.containers[i],
                                          bm->high_low_container.typecodes[i]);
        } else if (xhigh == key) {
            return size + container_rank(bm->high_low_container.containers[i],
                                         bm->high_low_container.typecodes[i],
                                         x & 0xFFFF);
        } else {
            return size;
        }
    }
    return size;
}
void roaring_bitmap_rank_many(const roaring_bitmap_t *bm, const uint32_t *begin,
                              const uint32_t *end, uint64_t *ans) {
    uint64_t size = 0;

    int i = 0;
    const uint32_t *iter = begin;
    while (i < bm->high_low_container.size && iter != end) {
        uint32_t x = *iter;
        uint32_t xhigh = x >> 16;
        uint32_t key = bm->high_low_container.keys[i];
        if (xhigh > key) {
            size +=
                container_get_cardinality(bm->high_low_container.containers[i],
                                          bm->high_low_container.typecodes[i]);
            i++;
        } else if (xhigh == key) {
            uint32_t consumed = container_rank_many(
                bm->high_low_container.containers[i],
                bm->high_low_container.typecodes[i], size, iter, end, ans);
            iter += consumed;
            ans += consumed;
        } else {
            *(ans++) = size;
            iter++;
        }
    }
}

/**
 * roaring_bitmap_get_index returns the index of x, if not exsist return -1.
 */
int64_t roaring_bitmap_get_index(const roaring_bitmap_t *bm, uint32_t x) {
    int64_t index = 0;
    const uint16_t xhigh = x >> 16;
    int32_t high_idx = ra_get_index(&bm->high_low_container, xhigh);
    if (high_idx < 0) return -1;

    for (int i = 0; i < bm->high_low_container.size; i++) {
        uint32_t key = bm->high_low_container.keys[i];
        if (xhigh > key) {
            index +=
                container_get_cardinality(bm->high_low_container.containers[i],
                                          bm->high_low_container.typecodes[i]);
        } else if (xhigh == key) {
            int32_t low_idx = container_get_index(
                bm->high_low_container.containers[high_idx],
                bm->high_low_container.typecodes[high_idx], x & 0xFFFF);
            if (low_idx < 0) return -1;
            return index + low_idx;
        } else {
            return -1;
        }
    }
    return index;
}

/**
 * roaring_bitmap_smallest returns the smallest value in the set.
 * Returns UINT32_MAX if the set is empty.
 */
uint32_t roaring_bitmap_minimum(const roaring_bitmap_t *bm) {
    if (bm->high_low_container.size > 0) {
        container_t *c = bm->high_low_container.containers[0];
        uint8_t type = bm->high_low_container.typecodes[0];
        uint32_t key = bm->high_low_container.keys[0];
        uint32_t lowvalue = container_minimum(c, type);
        return lowvalue | (key << 16);
    }
    return UINT32_MAX;
}

/**
 * roaring_bitmap_smallest returns the greatest value in the set.
 * Returns 0 if the set is empty.
 */
uint32_t roaring_bitmap_maximum(const roaring_bitmap_t *bm) {
    if (bm->high_low_container.size > 0) {
        container_t *container =
            bm->high_low_container.containers[bm->high_low_container.size - 1];
        uint8_t typecode =
            bm->high_low_container.typecodes[bm->high_low_container.size - 1];
        uint32_t key =
            bm->high_low_container.keys[bm->high_low_container.size - 1];
        uint32_t lowvalue = container_maximum(container, typecode);
        return lowvalue | (key << 16);
    }
    return 0;
}

bool roaring_bitmap_select(const roaring_bitmap_t *bm, uint32_t rank,
                           uint32_t *element) {
    container_t *container;
    uint8_t typecode;
    uint16_t key;
    uint32_t start_rank = 0;
    int i = 0;
    bool valid = false;
    while (!valid && i < bm->high_low_container.size) {
        container = bm->high_low_container.containers[i];
        typecode = bm->high_low_container.typecodes[i];
        valid =
            container_select(container, typecode, &start_rank, rank, element);
        i++;
    }

    if (valid) {
        key = bm->high_low_container.keys[i - 1];
        *element |= (((uint32_t)key) << 16);  // w/o cast, key promotes signed
        return true;
    } else
        return false;
}

bool roaring_bitmap_intersect(const roaring_bitmap_t *x1,
                              const roaring_bitmap_t *x2) {
    const int length1 = x1->high_low_container.size,
              length2 = x2->high_low_container.size;
    uint64_t answer = 0;
    int pos1 = 0, pos2 = 0;

    while (pos1 < length1 && pos2 < length2) {
        const uint16_t s1 =
            ra_get_key_at_index(&x1->high_low_container, (uint16_t)pos1);
        const uint16_t s2 =
            ra_get_key_at_index(&x2->high_low_container, (uint16_t)pos2);

        if (s1 == s2) {
            uint8_t type1, type2;
            container_t *c1 = ra_get_container_at_index(&x1->high_low_container,
                                                        (uint16_t)pos1, &type1);
            container_t *c2 = ra_get_container_at_index(&x2->high_low_container,
                                                        (uint16_t)pos2, &type2);
            if (container_intersect(c1, type1, c2, type2)) return true;
            ++pos1;
            ++pos2;
        } else if (s1 < s2) {  // s1 < s2
            pos1 = ra_advance_until(&x1->high_low_container, s2, pos1);
        } else {  // s1 > s2
            pos2 = ra_advance_until(&x2->high_low_container, s1, pos2);
        }
    }
    return answer != 0;
}

bool roaring_bitmap_intersect_with_range(const roaring_bitmap_t *bm, uint64_t x,
                                         uint64_t y) {
    if (x >= y) {
        // Empty range.
        return false;
    }
    roaring_uint32_iterator_t it;
    roaring_iterator_init(bm, &it);
    if (!roaring_uint32_iterator_move_equalorlarger(&it, (uint32_t)x)) {
        // No values above x.
        return false;
    }
    if (it.current_value >= y) {
        // No values below y.
        return false;
    }
    return true;
}

uint64_t roaring_bitmap_and_cardinality(const roaring_bitmap_t *x1,
                                        const roaring_bitmap_t *x2) {
    const int length1 = x1->high_low_container.size,
              length2 = x2->high_low_container.size;
    uint64_t answer = 0;
    int pos1 = 0, pos2 = 0;
    while (pos1 < length1 && pos2 < length2) {
        const uint16_t s1 =
            ra_get_key_at_index(&x1->high_low_container, (uint16_t)pos1);
        const uint16_t s2 =
            ra_get_key_at_index(&x2->high_low_container, (uint16_t)pos2);

        if (s1 == s2) {
            uint8_t type1, type2;
            container_t *c1 = ra_get_container_at_index(&x1->high_low_container,
                                                        (uint16_t)pos1, &type1);
            container_t *c2 = ra_get_container_at_index(&x2->high_low_container,
                                                        (uint16_t)pos2, &type2);
            answer += container_and_cardinality(c1, type1, c2, type2);
            ++pos1;
            ++pos2;
        } else if (s1 < s2) {  // s1 < s2
            pos1 = ra_advance_until(&x1->high_low_container, s2, pos1);
        } else {  // s1 > s2
            pos2 = ra_advance_until(&x2->high_low_container, s1, pos2);
        }
    }
    return answer;
}

double roaring_bitmap_jaccard_index(const roaring_bitmap_t *x1,
                                    const roaring_bitmap_t *x2) {
    const uint64_t c1 = roaring_bitmap_get_cardinality(x1);
    const uint64_t c2 = roaring_bitmap_get_cardinality(x2);
    const uint64_t inter = roaring_bitmap_and_cardinality(x1, x2);
    return (double)inter / (double)(c1 + c2 - inter);
}

uint64_t roaring_bitmap_or_cardinality(const roaring_bitmap_t *x1,
                                       const roaring_bitmap_t *x2) {
    const uint64_t c1 = roaring_bitmap_get_cardinality(x1);
    const uint64_t c2 = roaring_bitmap_get_cardinality(x2);
    const uint64_t inter = roaring_bitmap_and_cardinality(x1, x2);
    return c1 + c2 - inter;
}

uint64_t roaring_bitmap_andnot_cardinality(const roaring_bitmap_t *x1,
                                           const roaring_bitmap_t *x2) {
    const uint64_t c1 = roaring_bitmap_get_cardinality(x1);
    const uint64_t inter = roaring_bitmap_and_cardinality(x1, x2);
    return c1 - inter;
}

uint64_t roaring_bitmap_xor_cardinality(const roaring_bitmap_t *x1,
                                        const roaring_bitmap_t *x2) {
    const uint64_t c1 = roaring_bitmap_get_cardinality(x1);
    const uint64_t c2 = roaring_bitmap_get_cardinality(x2);
    const uint64_t inter = roaring_bitmap_and_cardinality(x1, x2);
    return c1 + c2 - 2 * inter;
}

/**
 * Check whether a range of values from range_start (included) to range_end
 * (excluded) is present
 */
bool roaring_bitmap_contains_range(const roaring_bitmap_t *r,
                                   uint64_t range_start, uint64_t range_end) {
    if (range_start >= range_end || range_start > (uint64_t)UINT32_MAX + 1) {
        return true;
    }
    return roaring_bitmap_contains_range_closed(r, (uint32_t)range_start,
                                                (uint32_t)(range_end - 1));
}

/**
 * Check whether a range of values from range_start (included) to range_end
 * (included) is present
 */
bool roaring_bitmap_contains_range_closed(const roaring_bitmap_t *r,
                                          uint32_t range_start,
                                          uint32_t range_end) {
    if (range_start > range_end) {
        return true;
    }  // empty range are always contained!
    if (range_end == range_start) {
        return roaring_bitmap_contains(r, (uint32_t)range_start);
    }
    uint16_t hb_rs = (uint16_t)(range_start >> 16);
    uint16_t hb_re = (uint16_t)(range_end >> 16);
    const int32_t span = hb_re - hb_rs;
    const int32_t hlc_sz = ra_get_size(&r->high_low_container);
    if (hlc_sz < span + 1) {
        return false;
    }
    int32_t is = ra_get_index(&r->high_low_container, hb_rs);
    int32_t ie = ra_get_index(&r->high_low_container, hb_re);
    if ((ie < 0) || (is < 0) || ((ie - is) != span) || ie >= hlc_sz) {
        return false;
    }
    const uint32_t lb_rs = range_start & 0xFFFF;
    const uint32_t lb_re = (range_end & 0xFFFF) + 1;
    uint8_t type;
    container_t *c =
        ra_get_container_at_index(&r->high_low_container, (uint16_t)is, &type);
    if (hb_rs == hb_re) {
        return container_contains_range(c, lb_rs, lb_re, type);
    }
    if (!container_contains_range(c, lb_rs, 1 << 16, type)) {
        return false;
    }
    c = ra_get_container_at_index(&r->high_low_container, (uint16_t)ie, &type);
    if (!container_contains_range(c, 0, lb_re, type)) {
        return false;
    }
    for (int32_t i = is + 1; i < ie; ++i) {
        c = ra_get_container_at_index(&r->high_low_container, (uint16_t)i,
                                      &type);
        if (!container_is_full(c, type)) {
            return false;
        }
    }
    return true;
}

bool roaring_bitmap_is_strict_subset(const roaring_bitmap_t *r1,
                                     const roaring_bitmap_t *r2) {
    return (roaring_bitmap_get_cardinality(r2) >
                roaring_bitmap_get_cardinality(r1) &&
            roaring_bitmap_is_subset(r1, r2));
}

/*
 * FROZEN SERIALIZATION FORMAT DESCRIPTION
 *
 * -- (beginning must be aligned by 32 bytes) --
 * <bitset_data> uint64_t[BITSET_CONTAINER_SIZE_IN_WORDS *
 * num_bitset_containers] <run_data>    rle16_t[total number of rle elements in
 * all run containers] <array_data>  uint16_t[total number of array elements in
 * all array containers] <keys>        uint16_t[num_containers] <counts>
 * uint16_t[num_containers] <typecodes>   uint8_t[num_containers] <header>
 * uint32_t
 *
 * <header> is a 4-byte value which is a bit union of FROZEN_COOKIE (15 bits)
 * and the number of containers (17 bits).
 *
 * <counts> stores number of elements for every container.
 * Its meaning depends on container type.
 * For array and bitset containers, this value is the container cardinality
 * minus one. For run container, it is the number of rle_t elements (n_runs).
 *
 * <bitset_data>,<array_data>,<run_data> are flat arrays of elements of
 * all containers of respective type.
 *
 * <*_data> and <keys> are kept close together because they are not accessed
 * during deserilization. This may reduce IO in case of large mmaped bitmaps.
 * All members have their native alignments during deserilization except
 * <header>, which is not guaranteed to be aligned by 4 bytes.
 */

size_t roaring_bitmap_frozen_size_in_bytes(const roaring_bitmap_t *rb) {
    const roaring_array_t *ra = &rb->high_low_container;
    size_t num_bytes = 0;
    for (int32_t i = 0; i < ra->size; i++) {
        switch (ra->typecodes[i]) {
            case BITSET_CONTAINER_TYPE: {
                num_bytes += BITSET_CONTAINER_SIZE_IN_WORDS * sizeof(uint64_t);
                break;
            }
            case RUN_CONTAINER_TYPE: {
                const run_container_t *rc = const_CAST_run(ra->containers[i]);
                num_bytes += rc->n_runs * sizeof(rle16_t);
                break;
            }
            case ARRAY_CONTAINER_TYPE: {
                const array_container_t *ac =
                    const_CAST_array(ra->containers[i]);
                num_bytes += ac->cardinality * sizeof(uint16_t);
                break;
            }
            default:
                roaring_unreachable;
        }
    }
    num_bytes += (2 + 2 + 1) * ra->size;  // keys, counts, typecodes
    num_bytes += 4;                       // header
    return num_bytes;
}

inline static void *arena_alloc(char **arena, size_t num_bytes) {
    char *res = *arena;
    *arena += num_bytes;
    return res;
}

void roaring_bitmap_frozen_serialize(const roaring_bitmap_t *rb, char *buf) {
    /*
     * Note: we do not require user to supply a specifically aligned buffer.
     * Thus we have to use memcpy() everywhere.
     */

    const roaring_array_t *ra = &rb->high_low_container;

    size_t bitset_zone_size = 0;
    size_t run_zone_size = 0;
    size_t array_zone_size = 0;
    for (int32_t i = 0; i < ra->size; i++) {
        switch (ra->typecodes[i]) {
            case BITSET_CONTAINER_TYPE: {
                bitset_zone_size +=
                    BITSET_CONTAINER_SIZE_IN_WORDS * sizeof(uint64_t);
                break;
            }
            case RUN_CONTAINER_TYPE: {
                const run_container_t *rc = const_CAST_run(ra->containers[i]);
                run_zone_size += rc->n_runs * sizeof(rle16_t);
                break;
            }
            case ARRAY_CONTAINER_TYPE: {
                const array_container_t *ac =
                    const_CAST_array(ra->containers[i]);
                array_zone_size += ac->cardinality * sizeof(uint16_t);
                break;
            }
            default:
                roaring_unreachable;
        }
    }

    uint64_t *bitset_zone = (uint64_t *)arena_alloc(&buf, bitset_zone_size);
    rle16_t *run_zone = (rle16_t *)arena_alloc(&buf, run_zone_size);
    uint16_t *array_zone = (uint16_t *)arena_alloc(&buf, array_zone_size);
    uint16_t *key_zone = (uint16_t *)arena_alloc(&buf, 2 * ra->size);
    uint16_t *count_zone = (uint16_t *)arena_alloc(&buf, 2 * ra->size);
    uint8_t *typecode_zone = (uint8_t *)arena_alloc(&buf, ra->size);
    char *header_zone = (char *)arena_alloc(&buf, 4);

    for (int32_t i = 0; i < ra->size; i++) {
        uint16_t count;
        switch (ra->typecodes[i]) {
            case BITSET_CONTAINER_TYPE: {
                const bitset_container_t *bc =
                    const_CAST_bitset(ra->containers[i]);
                memcpy(bitset_zone, bc->words,
                       BITSET_CONTAINER_SIZE_IN_WORDS * sizeof(uint64_t));
                bitset_zone += BITSET_CONTAINER_SIZE_IN_WORDS;
                if (bc->cardinality != BITSET_UNKNOWN_CARDINALITY) {
                    count = (uint16_t)(bc->cardinality - 1);
                } else {
                    count =
                        (uint16_t)(bitset_container_compute_cardinality(bc) -
                                   1);
                }
                break;
            }
            case RUN_CONTAINER_TYPE: {
                const run_container_t *rc = const_CAST_run(ra->containers[i]);
                size_t num_bytes = rc->n_runs * sizeof(rle16_t);
                memcpy(run_zone, rc->runs, num_bytes);
                run_zone += rc->n_runs;
                count = (uint16_t)rc->n_runs;
                break;
            }
            case ARRAY_CONTAINER_TYPE: {
                const array_container_t *ac =
                    const_CAST_array(ra->containers[i]);
                size_t num_bytes = ac->cardinality * sizeof(uint16_t);
                memcpy(array_zone, ac->array, num_bytes);
                array_zone += ac->cardinality;
                count = (uint16_t)(ac->cardinality - 1);
                break;
            }
            default:
                roaring_unreachable;
        }
        memcpy(&count_zone[i], &count, 2);
    }
    memcpy(key_zone, ra->keys, ra->size * sizeof(uint16_t));
    memcpy(typecode_zone, ra->typecodes, ra->size * sizeof(uint8_t));
    uint32_t header = ((uint32_t)ra->size << 15) | FROZEN_COOKIE;
    memcpy(header_zone, &header, 4);
}

const roaring_bitmap_t *roaring_bitmap_frozen_view(const char *buf,
                                                   size_t length) {
    if ((uintptr_t)buf % 32 != 0) {
        return NULL;
    }

    // cookie and num_containers
    if (length < 4) {
        return NULL;
    }
    uint32_t header;
    memcpy(&header, buf + length - 4, 4);  // header may be misaligned
    if ((header & 0x7FFF) != FROZEN_COOKIE) {
        return NULL;
    }
    int32_t num_containers = (header >> 15);

    // typecodes, counts and keys
    if (length < 4 + (size_t)num_containers * (1 + 2 + 2)) {
        return NULL;
    }
    uint16_t *keys = (uint16_t *)(buf + length - 4 - num_containers * 5);
    uint16_t *counts = (uint16_t *)(buf + length - 4 - num_containers * 3);
    uint8_t *typecodes = (uint8_t *)(buf + length - 4 - num_containers * 1);

    // {bitset,array,run}_zone
    int32_t num_bitset_containers = 0;
    int32_t num_run_containers = 0;
    int32_t num_array_containers = 0;
    size_t bitset_zone_size = 0;
    size_t run_zone_size = 0;
    size_t array_zone_size = 0;
    for (int32_t i = 0; i < num_containers; i++) {
        switch (typecodes[i]) {
            case BITSET_CONTAINER_TYPE:
                num_bitset_containers++;
                bitset_zone_size +=
                    BITSET_CONTAINER_SIZE_IN_WORDS * sizeof(uint64_t);
                break;
            case RUN_CONTAINER_TYPE:
                num_run_containers++;
                run_zone_size += counts[i] * sizeof(rle16_t);
                break;
            case ARRAY_CONTAINER_TYPE:
                num_array_containers++;
                array_zone_size += (counts[i] + UINT32_C(1)) * sizeof(uint16_t);
                break;
            default:
                return NULL;
        }
    }
    if (length != bitset_zone_size + run_zone_size + array_zone_size +
                      5 * num_containers + 4) {
        return NULL;
    }
    uint64_t *bitset_zone = (uint64_t *)(buf);
    rle16_t *run_zone = (rle16_t *)(buf + bitset_zone_size);
    uint16_t *array_zone = (uint16_t *)(buf + bitset_zone_size + run_zone_size);

    size_t alloc_size = 0;
    alloc_size += sizeof(roaring_bitmap_t);
    alloc_size += num_containers * sizeof(container_t *);
    alloc_size += num_bitset_containers * sizeof(bitset_container_t);
    alloc_size += num_run_containers * sizeof(run_container_t);
    alloc_size += num_array_containers * sizeof(array_container_t);

    char *arena = (char *)roaring_malloc(alloc_size);
    if (arena == NULL) {
        return NULL;
    }

    roaring_bitmap_t *rb =
        (roaring_bitmap_t *)arena_alloc(&arena, sizeof(roaring_bitmap_t));
    rb->high_low_container.flags = ROARING_FLAG_FROZEN;
    rb->high_low_container.allocation_size = num_containers;
    rb->high_low_container.size = num_containers;
    rb->high_low_container.keys = (uint16_t *)keys;
    rb->high_low_container.typecodes = (uint8_t *)typecodes;
    rb->high_low_container.containers = (container_t **)arena_alloc(
        &arena, sizeof(container_t *) * num_containers);
    // Ensure offset of high_low_container.containers is known distance used in
    // C++ wrapper. sizeof(roaring_bitmap_t) is used as it is the size of the
    // only allocation that precedes high_low_container.containers. If this is
    // changed (new allocation or changed order), this offset will also need to
    // be changed in the C++ wrapper.
    assert(rb ==
           (roaring_bitmap_t *)((char *)rb->high_low_container.containers -
                                sizeof(roaring_bitmap_t)));
    for (int32_t i = 0; i < num_containers; i++) {
        switch (typecodes[i]) {
            case BITSET_CONTAINER_TYPE: {
                bitset_container_t *bitset = (bitset_container_t *)arena_alloc(
                    &arena, sizeof(bitset_container_t));
                bitset->words = bitset_zone;
                bitset->cardinality = counts[i] + UINT32_C(1);
                rb->high_low_container.containers[i] = bitset;
                bitset_zone += BITSET_CONTAINER_SIZE_IN_WORDS;
                break;
            }
            case RUN_CONTAINER_TYPE: {
                run_container_t *run = (run_container_t *)arena_alloc(
                    &arena, sizeof(run_container_t));
                run->capacity = counts[i];
                run->n_runs = counts[i];
                run->runs = run_zone;
                rb->high_low_container.containers[i] = run;
                run_zone += run->n_runs;
                break;
            }
            case ARRAY_CONTAINER_TYPE: {
                array_container_t *array = (array_container_t *)arena_alloc(
                    &arena, sizeof(array_container_t));
                array->capacity = counts[i] + UINT32_C(1);
                array->cardinality = counts[i] + UINT32_C(1);
                array->array = array_zone;
                rb->high_low_container.containers[i] = array;
                array_zone += counts[i] + UINT32_C(1);
                break;
            }
            default:
                roaring_free(arena);
                return NULL;
        }
    }

    return rb;
}

CROARING_ALLOW_UNALIGNED
roaring_bitmap_t *roaring_bitmap_portable_deserialize_frozen(const char *buf) {
    char *start_of_buf = (char *)buf;
    uint32_t cookie;
    int32_t num_containers;
    uint16_t *descriptive_headers;
    uint32_t *offset_headers = NULL;
    const char *run_flag_bitset = NULL;
    bool hasrun = false;

    // deserialize cookie
    memcpy(&cookie, buf, sizeof(uint32_t));
    buf += sizeof(uint32_t);
    if (cookie == SERIAL_COOKIE_NO_RUNCONTAINER) {
        memcpy(&num_containers, buf, sizeof(int32_t));
        buf += sizeof(int32_t);
        descriptive_headers = (uint16_t *)buf;
        buf += num_containers * 2 * sizeof(uint16_t);
        offset_headers = (uint32_t *)buf;
        buf += num_containers * sizeof(uint32_t);
    } else if ((cookie & 0xFFFF) == SERIAL_COOKIE) {
        num_containers = (cookie >> 16) + 1;
        hasrun = true;
        int32_t run_flag_bitset_size = (num_containers + 7) / 8;
        run_flag_bitset = buf;
        buf += run_flag_bitset_size;
        descriptive_headers = (uint16_t *)buf;
        buf += num_containers * 2 * sizeof(uint16_t);
        if (num_containers >= NO_OFFSET_THRESHOLD) {
            offset_headers = (uint32_t *)buf;
            buf += num_containers * sizeof(uint32_t);
        }
    } else {
        return NULL;
    }

    // calculate total size for allocation
    int32_t num_bitset_containers = 0;
    int32_t num_run_containers = 0;
    int32_t num_array_containers = 0;

    for (int32_t i = 0; i < num_containers; i++) {
        uint16_t tmp;
        memcpy(&tmp, descriptive_headers + 2 * i + 1, sizeof(tmp));
        uint32_t cardinality = tmp + 1;
        bool isbitmap = (cardinality > DEFAULT_MAX_SIZE);
        bool isrun = false;
        if (hasrun) {
            if ((run_flag_bitset[i / 8] & (1 << (i % 8))) != 0) {
                isbitmap = false;
                isrun = true;
            }
        }

        if (isbitmap) {
            num_bitset_containers++;
        } else if (isrun) {
            num_run_containers++;
        } else {
            num_array_containers++;
        }
    }

    size_t alloc_size = 0;
    alloc_size += sizeof(roaring_bitmap_t);
    alloc_size += num_containers * sizeof(container_t *);
    alloc_size += num_bitset_containers * sizeof(bitset_container_t);
    alloc_size += num_run_containers * sizeof(run_container_t);
    alloc_size += num_array_containers * sizeof(array_container_t);
    alloc_size += num_containers * sizeof(uint16_t);  // keys
    alloc_size += num_containers * sizeof(uint8_t);   // typecodes

    // allocate bitmap and construct containers
    char *arena = (char *)roaring_malloc(alloc_size);
    if (arena == NULL) {
        return NULL;
    }

    roaring_bitmap_t *rb =
        (roaring_bitmap_t *)arena_alloc(&arena, sizeof(roaring_bitmap_t));
    rb->high_low_container.flags = ROARING_FLAG_FROZEN;
    rb->high_low_container.allocation_size = num_containers;
    rb->high_low_container.size = num_containers;
    rb->high_low_container.containers = (container_t **)arena_alloc(
        &arena, sizeof(container_t *) * num_containers);

    uint16_t *keys =
        (uint16_t *)arena_alloc(&arena, num_containers * sizeof(uint16_t));
    uint8_t *typecodes =
        (uint8_t *)arena_alloc(&arena, num_containers * sizeof(uint8_t));

    rb->high_low_container.keys = keys;
    rb->high_low_container.typecodes = typecodes;

    for (int32_t i = 0; i < num_containers; i++) {
        uint16_t tmp;
        memcpy(&tmp, descriptive_headers + 2 * i + 1, sizeof(tmp));
        int32_t cardinality = tmp + 1;
        bool isbitmap = (cardinality > DEFAULT_MAX_SIZE);
        bool isrun = false;
        if (hasrun) {
            if ((run_flag_bitset[i / 8] & (1 << (i % 8))) != 0) {
                isbitmap = false;
                isrun = true;
            }
        }

        keys[i] = descriptive_headers[2 * i];

        if (isbitmap) {
            typecodes[i] = BITSET_CONTAINER_TYPE;
            bitset_container_t *c = (bitset_container_t *)arena_alloc(
                &arena, sizeof(bitset_container_t));
            c->cardinality = cardinality;
            if (offset_headers != NULL) {
                c->words = (uint64_t *)(start_of_buf + offset_headers[i]);
            } else {
                c->words = (uint64_t *)buf;
                buf += BITSET_CONTAINER_SIZE_IN_WORDS * sizeof(uint64_t);
            }
            rb->high_low_container.containers[i] = c;
        } else if (isrun) {
            typecodes[i] = RUN_CONTAINER_TYPE;
            run_container_t *c =
                (run_container_t *)arena_alloc(&arena, sizeof(run_container_t));
            c->capacity = cardinality;
            uint16_t n_runs;
            if (offset_headers != NULL) {
                memcpy(&n_runs, start_of_buf + offset_headers[i],
                       sizeof(uint16_t));
                c->n_runs = n_runs;
                c->runs = (rle16_t *)(start_of_buf + offset_headers[i] +
                                      sizeof(uint16_t));
            } else {
                memcpy(&n_runs, buf, sizeof(uint16_t));
                c->n_runs = n_runs;
                buf += sizeof(uint16_t);
                c->runs = (rle16_t *)buf;
                buf += c->n_runs * sizeof(rle16_t);
            }
            rb->high_low_container.containers[i] = c;
        } else {
            typecodes[i] = ARRAY_CONTAINER_TYPE;
            array_container_t *c = (array_container_t *)arena_alloc(
                &arena, sizeof(array_container_t));
            c->cardinality = cardinality;
            c->capacity = cardinality;
            if (offset_headers != NULL) {
                c->array = (uint16_t *)(start_of_buf + offset_headers[i]);
            } else {
                c->array = (uint16_t *)buf;
                buf += cardinality * sizeof(uint16_t);
            }
            rb->high_low_container.containers[i] = c;
        }
    }

    return rb;
}

bool roaring_bitmap_to_bitset(const roaring_bitmap_t *r, bitset_t *bitset) {
    uint32_t max_value = roaring_bitmap_maximum(r);
    size_t new_array_size = (size_t)(max_value / 64 + 1);
    bool resize_ok = bitset_resize(bitset, new_array_size, true);
    if (!resize_ok) {
        return false;
    }
    const roaring_array_t *ra = &r->high_low_container;
    for (int i = 0; i < ra->size; ++i) {
        uint64_t *words = bitset->array + (ra->keys[i] << 10);
        uint8_t type = ra->typecodes[i];
        const container_t *c = ra->containers[i];
        if (type == SHARED_CONTAINER_TYPE) {
            c = container_unwrap_shared(c, &type);
        }
        switch (type) {
            case BITSET_CONTAINER_TYPE: {
                size_t max_word_index = new_array_size - (ra->keys[i] << 10);
                if (max_word_index > 1024) {
                    max_word_index = 1024;
                }
                const bitset_container_t *src = const_CAST_bitset(c);
                memcpy(words, src->words, max_word_index * sizeof(uint64_t));
            } break;
            case ARRAY_CONTAINER_TYPE: {
                const array_container_t *src = const_CAST_array(c);
                bitset_set_list(words, src->array, src->cardinality);
            } break;
            case RUN_CONTAINER_TYPE: {
                const run_container_t *src = const_CAST_run(c);
                for (int32_t rlepos = 0; rlepos < src->n_runs; ++rlepos) {
                    rle16_t rle = src->runs[rlepos];
                    bitset_set_lenrange(words, rle.value, rle.length);
                }
            } break;
            default:
                roaring_unreachable;
        }
    }
    return true;
}

#ifdef __cplusplus
}
}
}  // extern "C" { namespace roaring {
#endif
