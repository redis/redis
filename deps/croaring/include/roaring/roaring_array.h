/*
 * roaring_array.h
 *
 * This file declares the roaring_array helper structure and the operations
 * used to manage it. A roaring array is the top-level index used by a 32-bit
 * Roaring bitmap: it stores sorted 16-bit high keys alongside the container
 * pointers and type codes associated with each key.
 *
 * In effect, it is the directory that maps each populated 16-bit chunk of the
 * 32-bit value space to the container holding that chunk's low 16-bit values.
 * The functions in this header handle allocation, lookup, insertion,
 * replacement, copying, serialization support, and structural updates on that
 * directory.
 */
#ifndef INCLUDE_ROARING_ARRAY_H
#define INCLUDE_ROARING_ARRAY_H

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include <roaring/array_util.h>
#include <roaring/containers/containers.h>  // get_writable_copy_if_shared()

#ifdef __cplusplus
extern "C" {
namespace roaring {

// Note: in pure C++ code, you should avoid putting `using` in header files
using api::roaring_array_t;

namespace internal {
#endif

enum {
    SERIAL_COOKIE_NO_RUNCONTAINER = 12346,
    SERIAL_COOKIE = 12347,
    FROZEN_COOKIE = 13766,
    NO_OFFSET_THRESHOLD = 4
};

/**
 * Create a new roaring array
 */
roaring_array_t *ra_create(void);

/**
 * Initialize an existing roaring array with the specified capacity (in number
 * of containers)
 */
bool ra_init_with_capacity(roaring_array_t *new_ra, uint32_t cap);

/**
 * Initialize with zero capacity
 */
void ra_init(roaring_array_t *t);

/**
 * Copies this roaring array, we assume that dest is not initialized
 */
bool ra_copy(const roaring_array_t *source, roaring_array_t *dest,
             bool copy_on_write);

/*
 * Shrinks the capacity, returns the number of bytes saved.
 */
int ra_shrink_to_fit(roaring_array_t *ra);

/**
 * Copies this roaring array, we assume that dest is initialized
 */
bool ra_overwrite(const roaring_array_t *source, roaring_array_t *dest,
                  bool copy_on_write);

/**
 * Frees the memory used by a roaring array
 */
void ra_clear(roaring_array_t *r);

/**
 * Frees the memory used by a roaring array, but does not free the containers
 */
void ra_clear_without_containers(roaring_array_t *r);

/**
 * Frees just the containers
 */
void ra_clear_containers(roaring_array_t *ra);

/**
 * Get the index corresponding to a 16-bit key
 */
inline int32_t ra_get_index(const roaring_array_t *ra, uint16_t x) {
    if ((ra->size == 0) || ra->keys[ra->size - 1] == x) return ra->size - 1;
    return binarySearch(ra->keys, (int32_t)ra->size, x);
}

/**
 * Retrieves the container at index i, filling in the typecode
 */
inline container_t *ra_get_container_at_index(const roaring_array_t *ra,
                                              uint16_t i, uint8_t *typecode) {
    *typecode = ra->typecodes[i];
    return ra->containers[i];
}

/**
 * Retrieves the key at index i
 */
inline uint16_t ra_get_key_at_index(const roaring_array_t *ra, uint16_t i) {
    return ra->keys[i];
}

/**
 * Add a new key-value pair at index i
 */
void ra_insert_new_key_value_at(roaring_array_t *ra, int32_t i, uint16_t key,
                                container_t *c, uint8_t typecode);

/**
 * Append a new key-value pair
 */
void ra_append(roaring_array_t *ra, uint16_t key, container_t *c,
               uint8_t typecode);

/**
 * Append a new key-value pair to ra, cloning (in COW sense) a value from sa
 * at index index
 */
void ra_append_copy(roaring_array_t *ra, const roaring_array_t *sa,
                    uint16_t index, bool copy_on_write);

/**
 * Append new key-value pairs to ra, cloning (in COW sense)  values from sa
 * at indexes
 * [start_index, end_index)
 */
void ra_append_copy_range(roaring_array_t *ra, const roaring_array_t *sa,
                          int32_t start_index, int32_t end_index,
                          bool copy_on_write);

/** appends from sa to ra, ending with the greatest key that is
 * is less or equal stopping_key
 */
void ra_append_copies_until(roaring_array_t *ra, const roaring_array_t *sa,
                            uint16_t stopping_key, bool copy_on_write);

/** appends from sa to ra, starting with the smallest key that is
 * is strictly greater than before_start
 */

void ra_append_copies_after(roaring_array_t *ra, const roaring_array_t *sa,
                            uint16_t before_start, bool copy_on_write);

/**
 * Move the key-value pairs to ra from sa at indexes
 * [start_index, end_index), old array should not be freed
 * (use ra_clear_without_containers)
 **/
void ra_append_move_range(roaring_array_t *ra, roaring_array_t *sa,
                          int32_t start_index, int32_t end_index);
/**
 * Append new key-value pairs to ra,  from sa at indexes
 * [start_index, end_index)
 */
void ra_append_range(roaring_array_t *ra, roaring_array_t *sa,
                     int32_t start_index, int32_t end_index,
                     bool copy_on_write);

/**
 * Set the container at the corresponding index using the specified
 * typecode.
 */
inline void ra_set_container_at_index(const roaring_array_t *ra, int32_t i,
                                      container_t *c, uint8_t typecode) {
    assert(i < ra->size);
    ra->containers[i] = c;
    ra->typecodes[i] = typecode;
}

container_t *ra_get_container(roaring_array_t *ra, uint16_t x,
                              uint8_t *typecode);

/**
 * If needed, increase the capacity of the array so that it can fit k values
 * (at
 * least);
 */
bool extend_array(roaring_array_t *ra, int32_t k);

inline int32_t ra_get_size(const roaring_array_t *ra) { return ra->size; }

static inline int32_t ra_advance_until(const roaring_array_t *ra, uint16_t x,
                                       int32_t pos) {
    return advanceUntil(ra->keys, pos, ra->size, x);
}

int32_t ra_advance_until_freeing(roaring_array_t *ra, uint16_t x, int32_t pos);

void ra_downsize(roaring_array_t *ra, int32_t new_length);

inline void ra_replace_key_and_container_at_index(roaring_array_t *ra,
                                                  int32_t i, uint16_t key,
                                                  container_t *c,
                                                  uint8_t typecode) {
    assert(i < ra->size);

    ra->keys[i] = key;
    ra->containers[i] = c;
    ra->typecodes[i] = typecode;
}

// write set bits to an array
void ra_to_uint32_array(const roaring_array_t *ra, uint32_t *ans);

/**
 * write a bitmap to a buffer. This is meant to be compatible with
 * the
 * Java and Go versions. Return the size in bytes of the serialized
 * output (which should be ra_portable_size_in_bytes(ra)).
 */
size_t ra_portable_serialize(const roaring_array_t *ra, char *buf);

/**
 * read a bitmap from a serialized version. This is meant to be compatible
 * with the Java and Go versions.
 * maxbytes  indicates how many bytes available from buf.
 * When the function returns true, roaring_array_t is populated with the data
 * and *readbytes indicates how many bytes were read. In all cases, if the
 * function returns true, then maxbytes >= *readbytes.
 */
bool ra_portable_deserialize(roaring_array_t *ra, const char *buf,
                             const size_t maxbytes, size_t *readbytes);

/**
 * Quickly checks whether there is a serialized bitmap at the pointer,
 * not exceeding size "maxbytes" in bytes. This function does not allocate
 * memory dynamically.
 *
 * This function returns 0 if and only if no valid bitmap is found.
 * Otherwise, it returns how many bytes are occupied by the bitmap data.
 */
size_t ra_portable_deserialize_size(const char *buf, const size_t maxbytes);

/**
 * How many bytes are required to serialize this bitmap (meant to be
 * compatible
 * with Java and Go versions)
 */
size_t ra_portable_size_in_bytes(const roaring_array_t *ra);

/**
 * return true if it contains at least one run container.
 */
bool ra_has_run_container(const roaring_array_t *ra);

/**
 * Size of the header when serializing (meant to be compatible
 * with Java and Go versions)
 */
uint32_t ra_portable_header_size(const roaring_array_t *ra);

/**
 * If the container at the index i is share, unshare it (creating a local
 * copy if needed).
 */
static inline void ra_unshare_container_at_index(roaring_array_t *ra,
                                                 uint16_t i) {
    assert(i < ra->size);
    ra->containers[i] =
        get_writable_copy_if_shared(ra->containers[i], &ra->typecodes[i]);
}

/**
 * remove at index i, sliding over all entries after i
 */
void ra_remove_at_index(roaring_array_t *ra, int32_t i);

/**
 * clears all containers, sets the size at 0 and shrinks the memory usage.
 */
void ra_reset(roaring_array_t *ra);

/**
 * remove at index i, sliding over all entries after i. Free removed container.
 */
void ra_remove_at_index_and_free(roaring_array_t *ra, int32_t i);

/**
 * remove a chunk of indices, sliding over entries after it
 */
// void ra_remove_index_range(roaring_array_t *ra, int32_t begin, int32_t end);

// used in inplace andNot only, to slide left the containers from
// the mutated RoaringBitmap that are after the largest container of
// the argument RoaringBitmap.  It is followed by a call to resize.
//
void ra_copy_range(roaring_array_t *ra, uint32_t begin, uint32_t end,
                   uint32_t new_begin);

/**
 * Shifts rightmost $count containers to the left (distance < 0) or
 * to the right (distance > 0).
 * Allocates memory if necessary.
 * This function doesn't free or create new containers.
 * Caller is responsible for that.
 */
void ra_shift_tail(roaring_array_t *ra, int32_t count, int32_t distance);

#ifdef __cplusplus
}  // namespace internal
}
}  // extern "C" { namespace roaring {
#endif

#endif
