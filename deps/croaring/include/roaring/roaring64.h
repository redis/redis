/*
 * roaring64.h
 *
 * This file declares the 64-bit Roaring bitmap API. A roaring64 bitmap stores
 * sets of 64-bit unsigned integers by partitioning the value space by high
 * bits and using Roaring containers for the lower bits inside each partition.
 * This keeps the structure compact while preserving fast membership tests,
 * insertions, iteration, and set operations over large sparse integer sets.
 */
#ifndef ROARING64_H
#define ROARING64_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <roaring/memory.h>
#include <roaring/portability.h>
#include <roaring/roaring.h>
#include <roaring/roaring_types.h>

#ifdef __cplusplus
extern "C" {
namespace roaring {
namespace api {
#endif

typedef struct roaring64_bitmap_s roaring64_bitmap_t;
typedef uint64_t roaring64_leaf_t;
typedef struct roaring64_iterator_s roaring64_iterator_t;

/**
 * A bit of context usable with `roaring64_bitmap_*_bulk()` functions.
 *
 * Should be initialized with `{0}` (or `memset()` to all zeros).
 * Callers should treat it as an opaque type.
 *
 * A context may only be used with a single bitmap (unless re-initialized to
 * zero), and any modification to a bitmap (other than modifications performed
 * with `_bulk()` functions with the context passed) will invalidate any
 * contexts associated with that bitmap.
 */
typedef struct roaring64_bulk_context_s {
    uint8_t high_bytes[6];
    roaring64_leaf_t *leaf;
} roaring64_bulk_context_t;

/**
 * Dynamically allocates a new bitmap (initially empty).
 * Client is responsible for calling `roaring64_bitmap_free()`.
 * The returned pointer may be NULL in case of errors.
 */
roaring64_bitmap_t *roaring64_bitmap_create(void);
void roaring64_bitmap_free(roaring64_bitmap_t *r);

/**
 * Returns a copy of a bitmap.
 * The returned pointer may be NULL in case of errors.
 */
roaring64_bitmap_t *roaring64_bitmap_copy(const roaring64_bitmap_t *r);

/**
 * Copies a bitmap from src to dest. It is assumed that the pointer dest
 * is to an already allocated bitmap. The content of the dest bitmap is
 * freed/deleted.
 *
 * It might be preferable and simpler to call roaring64_bitmap_copy except
 * that roaring64_bitmap_overwrite can save on memory allocations.
 *
 */
void roaring64_bitmap_overwrite(roaring64_bitmap_t *dest,
                                const roaring64_bitmap_t *src);

/**
 * Creates a new bitmap of a pointer to N 64-bit integers.
 */
roaring64_bitmap_t *roaring64_bitmap_of_ptr(size_t n_args,
                                            const uint64_t *vals);

#ifdef __cplusplus
/**
 * Creates a new bitmap which contains all values passed in as arguments.
 *
 * To create a bitmap from a variable number of arguments, use the
 * `roaring64_bitmap_of_ptr` function instead.
 */
// Use an immediately invoked closure, capturing by reference
// (in case __VA_ARGS__ refers to context outside the closure)
// Include a 0 at the beginning of the array to make the array length > 0
// (zero sized arrays are not valid in standard c/c++)
#define roaring64_bitmap_from(...)                                       \
    [&]() {                                                              \
        const uint64_t roaring64_bitmap_from_array[] = {0, __VA_ARGS__}; \
        return roaring64_bitmap_of_ptr(                                  \
            (sizeof(roaring64_bitmap_from_array) /                       \
             sizeof(roaring64_bitmap_from_array[0])) -                   \
                1,                                                       \
            &roaring64_bitmap_from_array[1]);                            \
    }()
#else
/**
 * Creates a new bitmap which contains all values passed in as arguments.
 *
 * To create a bitmap from a variable number of arguments, use the
 * `roaring64_bitmap_of_ptr` function instead.
 */
// While __VA_ARGS__ occurs twice in expansion, one of the times is in a sizeof
// expression, which is an unevaluated context, so it's even safe in the case
// where expressions passed have side effects (roaring64_bitmap_from(my_func(),
// ++i))
// Include a 0 at the beginning of the array to make the array length > 0
// (zero sized arrays are not valid in standard c/c++)
#define roaring64_bitmap_from(...)                                           \
    roaring64_bitmap_of_ptr(                                                 \
        (sizeof((const uint64_t[]){0, __VA_ARGS__}) / sizeof(uint64_t)) - 1, \
        &((const uint64_t[]){0, __VA_ARGS__})[1])
#endif

/**
 * Create a new bitmap by moving containers from a 32 bit roaring bitmap.
 *
 * After calling this function, the original bitmap will be empty, and the
 * returned bitmap will contain all the values from the original bitmap.
 */
roaring64_bitmap_t *roaring64_bitmap_move_from_roaring32(roaring_bitmap_t *r);

/**
 * Create a new bitmap containing all the values in [min, max) that are at a
 * distance k*step from min.
 * The returned pointer may be NULL in case of errors.
 */
roaring64_bitmap_t *roaring64_bitmap_from_range(uint64_t min, uint64_t max,
                                                uint64_t step);

/**
 * Adds the provided value to the bitmap.
 */
void roaring64_bitmap_add(roaring64_bitmap_t *r, uint64_t val);

/**
 * Adds the provided value to the bitmap.
 * Returns true if a new value was added, false if the value already existed.
 */
bool roaring64_bitmap_add_checked(roaring64_bitmap_t *r, uint64_t val);

/**
 * Add an item, using context from a previous insert for faster insertion.
 *
 * `context` will be used to store information between calls to make bulk
 * operations faster. `*context` should be zero-initialized before the first
 * call to this function.
 *
 * Modifying the bitmap in any way (other than `-bulk` suffixed functions)
 * will invalidate the stored context, calling this function with a non-zero
 * context after doing any modification invokes undefined behavior.
 *
 * In order to exploit this optimization, the caller should call this function
 * with values with the same high 48 bits of the value consecutively.
 */
void roaring64_bitmap_add_bulk(roaring64_bitmap_t *r,
                               roaring64_bulk_context_t *context, uint64_t val);

/**
 * Add `n_args` values from `vals`, faster than repeatedly calling
 * `roaring64_bitmap_add()`
 *
 * In order to exploit this optimization, the caller should attempt to keep
 * values with the same high 48 bits of the value as consecutive elements in
 * `vals`.
 */
void roaring64_bitmap_add_many(roaring64_bitmap_t *r, size_t n_args,
                               const uint64_t *vals);

/**
 * Add all values in range [min, max).
 */
void roaring64_bitmap_add_range(roaring64_bitmap_t *r, uint64_t min,
                                uint64_t max);

/**
 * Add all values in range [min, max].
 */
void roaring64_bitmap_add_range_closed(roaring64_bitmap_t *r, uint64_t min,
                                       uint64_t max);

/**
 * Removes a value from the bitmap if present.
 */
void roaring64_bitmap_remove(roaring64_bitmap_t *r, uint64_t val);

/**
 * Removes a value from the bitmap if present, returns true if the value was
 * removed and false if the value was not present.
 */
bool roaring64_bitmap_remove_checked(roaring64_bitmap_t *r, uint64_t val);

/**
 * Remove an item, using context from a previous insert for faster removal.
 *
 * `context` will be used to store information between calls to make bulk
 * operations faster. `*context` should be zero-initialized before the first
 * call to this function.
 *
 * Modifying the bitmap in any way (other than `-bulk` suffixed functions)
 * will invalidate the stored context, calling this function with a non-zero
 * context after doing any modification invokes undefined behavior.
 *
 * In order to exploit this optimization, the caller should call this function
 * with values with the same high 48 bits of the value consecutively.
 */
void roaring64_bitmap_remove_bulk(roaring64_bitmap_t *r,
                                  roaring64_bulk_context_t *context,
                                  uint64_t val);

/**
 * Remove `n_args` values from `vals`, faster than repeatedly calling
 * `roaring64_bitmap_remove()`
 *
 * In order to exploit this optimization, the caller should attempt to keep
 * values with the same high 48 bits of the value as consecutive elements in
 * `vals`.
 */
void roaring64_bitmap_remove_many(roaring64_bitmap_t *r, size_t n_args,
                                  const uint64_t *vals);

/**
 * Remove all values in range [min, max).
 */
void roaring64_bitmap_remove_range(roaring64_bitmap_t *r, uint64_t min,
                                   uint64_t max);

/**
 * Remove all values in range [min, max].
 */
void roaring64_bitmap_remove_range_closed(roaring64_bitmap_t *r, uint64_t min,
                                          uint64_t max);

/**
 * Empties the bitmap.
 */
void roaring64_bitmap_clear(roaring64_bitmap_t *r);

/**
 * Returns true if the provided value is present.
 */
bool roaring64_bitmap_contains(const roaring64_bitmap_t *r, uint64_t val);

/**
 * Returns true if all values in the range [min, max) are present.
 */
bool roaring64_bitmap_contains_range(const roaring64_bitmap_t *r, uint64_t min,
                                     uint64_t max);

/**
 * Check if an item is present using context from a previous insert or search
 * for faster search.
 *
 * `context` will be used to store information between calls to make bulk
 * operations faster. `*context` should be zero-initialized before the first
 * call to this function.
 *
 * Modifying the bitmap in any way (other than `-bulk` suffixed functions)
 * will invalidate the stored context, calling this function with a non-zero
 * context after doing any modification invokes undefined behavior.
 *
 * In order to exploit this optimization, the caller should call this function
 * with values with the same high 48 bits of the value consecutively.
 */
bool roaring64_bitmap_contains_bulk(const roaring64_bitmap_t *r,
                                    roaring64_bulk_context_t *context,
                                    uint64_t val);

/**
 * Selects the element at index 'rank' where the smallest element is at index 0.
 * If the size of the bitmap is strictly greater than rank, then this function
 * returns true and sets element to the element of given rank. Otherwise, it
 * returns false.
 */
bool roaring64_bitmap_select(const roaring64_bitmap_t *r, uint64_t rank,
                             uint64_t *element);

/**
 * Returns the number of integers that are smaller or equal to x. Thus if x is
 * the first element, this function will return 1. If x is smaller than the
 * smallest element, this function will return 0.
 *
 * The indexing convention differs between roaring64_bitmap_select and
 * roaring64_bitmap_rank: roaring_bitmap64_select refers to the smallest value
 * as having index 0, whereas roaring64_bitmap_rank returns 1 when ranking
 * the smallest value.
 */
uint64_t roaring64_bitmap_rank(const roaring64_bitmap_t *r, uint64_t val);

/**
 * Returns true if the given value is in the bitmap, and sets `out_index` to the
 * (0-based) index of the value in the bitmap. Returns false if the value is not
 * in the bitmap.
 */
bool roaring64_bitmap_get_index(const roaring64_bitmap_t *r, uint64_t val,
                                uint64_t *out_index);

/**
 * Returns the number of values in the bitmap.
 */
uint64_t roaring64_bitmap_get_cardinality(const roaring64_bitmap_t *r);

/**
 * Returns the number of elements in the range [min, max).
 */
uint64_t roaring64_bitmap_range_cardinality(const roaring64_bitmap_t *r,
                                            uint64_t min, uint64_t max);

/**
 * Returns the number of elements in the range [min, max]
 */
uint64_t roaring64_bitmap_range_closed_cardinality(const roaring64_bitmap_t *r,
                                                   uint64_t min, uint64_t max);

/**
 * Returns true if the bitmap is empty (cardinality is zero).
 */
bool roaring64_bitmap_is_empty(const roaring64_bitmap_t *r);

/**
 * Returns the smallest value in the set, or UINT64_MAX if the set is empty.
 */
uint64_t roaring64_bitmap_minimum(const roaring64_bitmap_t *r);

/**
 * Returns the largest value in the set, or 0 if empty.
 */
uint64_t roaring64_bitmap_maximum(const roaring64_bitmap_t *r);

/**
 * Remove run-length encoding even when it is more space efficient.
 * Return whether a change was applied.
 */
bool roaring64_bitmap_remove_run_compression(roaring64_bitmap_t *r);

/**
 * Returns true if the result has at least one run container.
 */
bool roaring64_bitmap_run_optimize(roaring64_bitmap_t *r);

/**
 * Shrinks internal arrays to eliminate any unused capacity. Returns the number
 * of bytes freed.
 */
size_t roaring64_bitmap_shrink_to_fit(roaring64_bitmap_t *r);

/**
 *  (For advanced users.)
 * Collect statistics about the bitmap
 */
void roaring64_bitmap_statistics(const roaring64_bitmap_t *r,
                                 roaring64_statistics_t *stat);

/**
 * Perform internal consistency checks.
 *
 * Returns true if the bitmap is consistent. It may be useful to call this
 * after deserializing bitmaps from untrusted sources. If
 * roaring64_bitmap_internal_validate returns true, then the bitmap is
 * consistent and can be trusted not to cause crashes or memory corruption.
 *
 * If reason is non-null, it will be set to a string describing the first
 * inconsistency found if any.
 */
bool roaring64_bitmap_internal_validate(const roaring64_bitmap_t *r,
                                        const char **reason);

/**
 * Return true if the two bitmaps contain the same elements.
 */
bool roaring64_bitmap_equals(const roaring64_bitmap_t *r1,
                             const roaring64_bitmap_t *r2);

/**
 * Return true if all the elements of r1 are also in r2.
 */
bool roaring64_bitmap_is_subset(const roaring64_bitmap_t *r1,
                                const roaring64_bitmap_t *r2);

/**
 * Return true if all the elements of r1 are also in r2, and r2 is strictly
 * greater than r1.
 */
bool roaring64_bitmap_is_strict_subset(const roaring64_bitmap_t *r1,
                                       const roaring64_bitmap_t *r2);

/**
 * Computes the intersection between two bitmaps and returns new bitmap. The
 * caller is responsible for free-ing the result.
 *
 * Performance hint: if you are computing the intersection between several
 * bitmaps, two-by-two, it is best to start with the smallest bitmaps. You may
 * also rely on roaring64_bitmap_and_inplace to avoid creating many temporary
 * bitmaps.
 *
 * The returned pointer may be NULL in case of errors.
 */
roaring64_bitmap_t *roaring64_bitmap_and(const roaring64_bitmap_t *r1,
                                         const roaring64_bitmap_t *r2);

/**
 * Computes the size of the intersection between two bitmaps.
 */
uint64_t roaring64_bitmap_and_cardinality(const roaring64_bitmap_t *r1,
                                          const roaring64_bitmap_t *r2);

/**
 * In-place version of `roaring64_bitmap_and()`, modifies `r1`. `r1` and `r2`
 * are allowed to be equal.
 *
 * Performance hint: if you are computing the intersection between several
 * bitmaps, two-by-two, it is best to start with the smallest bitmaps.
 */
void roaring64_bitmap_and_inplace(roaring64_bitmap_t *r1,
                                  const roaring64_bitmap_t *r2);

/**
 * Check whether two bitmaps intersect.
 */
bool roaring64_bitmap_intersect(const roaring64_bitmap_t *r1,
                                const roaring64_bitmap_t *r2);

/**
 * Check whether a bitmap intersects the range [min, max).
 */
bool roaring64_bitmap_intersect_with_range(const roaring64_bitmap_t *r,
                                           uint64_t min, uint64_t max);

/**
 * Computes the Jaccard index between two bitmaps. (Also known as the Tanimoto
 * distance, or the Jaccard similarity coefficient)
 *
 * The Jaccard index is undefined if both bitmaps are empty.
 */
double roaring64_bitmap_jaccard_index(const roaring64_bitmap_t *r1,
                                      const roaring64_bitmap_t *r2);

/**
 * Computes the union between two bitmaps and returns new bitmap. The caller is
 * responsible for free-ing the result.
 * The returned pointer may be NULL in case of errors.
 */
roaring64_bitmap_t *roaring64_bitmap_or(const roaring64_bitmap_t *r1,
                                        const roaring64_bitmap_t *r2);

/**
 * Computes the size of the union between two bitmaps.
 */
uint64_t roaring64_bitmap_or_cardinality(const roaring64_bitmap_t *r1,
                                         const roaring64_bitmap_t *r2);

/**
 * In-place version of `roaring64_bitmap_or(), modifies `r1`.
 */
void roaring64_bitmap_or_inplace(roaring64_bitmap_t *r1,
                                 const roaring64_bitmap_t *r2);

/**
 * Computes the symmetric difference (xor) between two bitmaps and returns a new
 * bitmap. The caller is responsible for free-ing the result.
 * The returned pointer may be NULL in case of errors.
 */
roaring64_bitmap_t *roaring64_bitmap_xor(const roaring64_bitmap_t *r1,
                                         const roaring64_bitmap_t *r2);

/**
 * Computes the size of the symmetric difference (xor) between two bitmaps.
 */
uint64_t roaring64_bitmap_xor_cardinality(const roaring64_bitmap_t *r1,
                                          const roaring64_bitmap_t *r2);

/**
 * In-place version of `roaring64_bitmap_xor()`, modifies `r1`. `r1` and `r2`
 * are not allowed to be equal (that would result in an empty bitmap).
 */
void roaring64_bitmap_xor_inplace(roaring64_bitmap_t *r1,
                                  const roaring64_bitmap_t *r2);

/**
 * Computes the difference (andnot) between two bitmaps and returns a new
 * bitmap. The caller is responsible for free-ing the result.
 * The returned pointer may be NULL in case of errors.
 */
roaring64_bitmap_t *roaring64_bitmap_andnot(const roaring64_bitmap_t *r1,
                                            const roaring64_bitmap_t *r2);

/**
 * Computes the size of the difference (andnot) between two bitmaps.
 */
uint64_t roaring64_bitmap_andnot_cardinality(const roaring64_bitmap_t *r1,
                                             const roaring64_bitmap_t *r2);

/**
 * In-place version of `roaring64_bitmap_andnot()`, modifies `r1`. `r1` and `r2`
 * are not allowed to be equal (that would result in an empty bitmap).
 */
void roaring64_bitmap_andnot_inplace(roaring64_bitmap_t *r1,
                                     const roaring64_bitmap_t *r2);

/**
 * Compute the negation of the bitmap in the interval [min, max).
 * The number of negated values is `max - min`. Areas outside the range are
 * passed through unchanged.
 * The returned pointer may be NULL in case of errors.
 */
roaring64_bitmap_t *roaring64_bitmap_flip(const roaring64_bitmap_t *r,
                                          uint64_t min, uint64_t max);

/**
 * Compute the negation of the bitmap in the interval [min, max].
 * The number of negated values is `max - min + 1`. Areas outside the range are
 * passed through unchanged.
 * The returned pointer may be NULL in case of errors.
 */
roaring64_bitmap_t *roaring64_bitmap_flip_closed(const roaring64_bitmap_t *r,
                                                 uint64_t min, uint64_t max);

/**
 * In-place version of `roaring64_bitmap_flip`. Compute the negation of the
 * bitmap in the interval [min, max). The number of negated values is `max -
 * min`. Areas outside the range are passed through unchanged.
 */
void roaring64_bitmap_flip_inplace(roaring64_bitmap_t *r, uint64_t min,
                                   uint64_t max);
/**
 * In-place version of `roaring64_bitmap_flip_closed`. Compute the negation of
 * the bitmap in the interval [min, max]. The number of negated values is `max -
 * min + 1`. Areas outside the range are passed through unchanged.
 */
void roaring64_bitmap_flip_closed_inplace(roaring64_bitmap_t *r, uint64_t min,
                                          uint64_t max);
/**
 * Return a copy of the bitmap with all values shifted by offset.
 *
 * If `positive` is true, the shift is added, otherwise subtracted. Values that
 * overflow or underflow uint64_t are dropped. The caller is responsible for
 * freeing the returned bitmap.
 */
roaring64_bitmap_t *roaring64_bitmap_add_offset_signed(
    const roaring64_bitmap_t *r, bool positive, uint64_t offset);

/**
 * Return a copy of the bitmap with all values shifted up by offset.
 *
 * Values that overflow or underflow uint64_t are dropped. The caller is
 * responsible for freeing the returned bitmap.
 */
static inline roaring64_bitmap_t *roaring64_bitmap_add_offset(
    const roaring64_bitmap_t *r, uint64_t offset) {
    return roaring64_bitmap_add_offset_signed(r, true, offset);
}

/**
 * Return a copy of the bitmap with all values shifted down by offset.
 *
 * Values that overflow or underflow uint64_t are dropped. The caller is
 * responsible for freeing the returned bitmap.
 */
static inline roaring64_bitmap_t *roaring64_bitmap_sub_offset(
    const roaring64_bitmap_t *r, uint64_t offset) {
    return roaring64_bitmap_add_offset_signed(r, false, offset);
}

/**
 * How many bytes are required to serialize this bitmap.
 *
 * This is meant to be compatible with other languages:
 * https://github.com/RoaringBitmap/RoaringFormatSpec#extension-for-64-bit-implementations
 */
size_t roaring64_bitmap_portable_size_in_bytes(const roaring64_bitmap_t *r);

/**
 * Write a bitmap to a buffer. The output buffer should refer to at least
 * `roaring64_bitmap_portable_size_in_bytes(r)` bytes of allocated memory.
 *
 * Returns how many bytes were written, which should match
 * `roaring64_bitmap_portable_size_in_bytes(r)`.
 *
 * This is meant to be compatible with other languages:
 * https://github.com/RoaringBitmap/RoaringFormatSpec#extension-for-64-bit-implementations
 *
 * This function is endian-sensitive. If you have a big-endian system (e.g., a
 * mainframe IBM s390x), the data format is going to be big-endian and not
 * compatible with little-endian systems.
 *
 * When serializing data to a file, we recommend that you also use
 * checksums so that, at deserialization, you can be confident
 * that you are recovering the correct data.
 */
size_t roaring64_bitmap_portable_serialize(const roaring64_bitmap_t *r,
                                           char *buf);
/**
 * Check how many bytes would be read (up to maxbytes) at this pointer if there
 * is a valid bitmap, returns zero if there is no valid bitmap.
 *
 * This is meant to be compatible with other languages
 * https://github.com/RoaringBitmap/RoaringFormatSpec#extension-for-64-bit-implementations
 */
size_t roaring64_bitmap_portable_deserialize_size(const char *buf,
                                                  size_t maxbytes);

/**
 * Read a bitmap from a serialized buffer (reading up to maxbytes).
 * In case of failure, NULL is returned.
 *
 * This is meant to be compatible with other languages
 * https://github.com/RoaringBitmap/RoaringFormatSpec#extension-for-64-bit-implementations
 *
 * The function itself is safe in the sense that it will not cause buffer
 * overflows: it will not read beyond the scope of the provided buffer
 * (buf,maxbytes).
 *
 * However, for correct operations, it is assumed that the bitmap
 * read was once serialized from a valid bitmap (i.e., it follows the format
 * specification). If you provided an incorrect input (garbage), then the bitmap
 * read may not be in a valid state and following operations may not lead to
 * sensible results. In particular, the serialized array containers need to be
 * in sorted order, and the run containers should be in sorted non-overlapping
 * order. This is is guaranteed to happen when serializing an existing bitmap,
 * but not for random inputs.
 *
 * If the source is untrusted, you should call
 * roaring64_bitmap_internal_validate to check the validity of the
 * bitmap prior to using it. Only after calling
 * roaring64_bitmap_internal_validate is the bitmap considered safe for use.
 *
 * We also recommend that you use checksums to check that serialized data
 * corresponds to the serialized bitmap. The CRoaring library does not provide
 * checksumming.
 *
 * This function is endian-sensitive. If you have a big-endian system (e.g., a
 * mainframe IBM s390x), the data format is going to be big-endian and not
 * compatible with little-endian systems.
 */
roaring64_bitmap_t *roaring64_bitmap_portable_deserialize_safe(const char *buf,
                                                               size_t maxbytes);

/**
 * Returns the number of bytes required to serialize this bitmap in a "frozen"
 * format. This is not compatible with any other serialization formats.
 *
 * `roaring64_bitmap_shrink_to_fit()` must be called before this method.
 */
size_t roaring64_bitmap_frozen_size_in_bytes(const roaring64_bitmap_t *r);

/**
 * Serializes the bitmap in a "frozen" format. The given buffer must be at least
 * `roaring64_bitmap_frozen_size_in_bytes()` in size. Returns the number of
 * bytes used for serialization.
 *
 * `roaring64_bitmap_shrink_to_fit()` must be called before this method.
 *
 * The frozen format is optimized for speed of (de)serialization, as well as
 * allowing the user to create a bitmap based on a memory mapped file, which is
 * possible because the format mimics the memory layout of the bitmap.
 *
 * Because the format mimics the memory layout of the bitmap, the format is not
 * fixed across releases of Roaring Bitmaps, and may change in future releases.
 *
 * This function is endian-sensitive. If you have a big-endian system (e.g., a
 * mainframe IBM s390x), the data format is going to be big-endian and not
 * compatible with little-endian systems.
 */
size_t roaring64_bitmap_frozen_serialize(const roaring64_bitmap_t *r,
                                         char *buf);

/**
 * Creates a readonly bitmap that is a view of the given buffer. The buffer
 * must be created with `roaring64_bitmap_frozen_serialize()`, and must be
 * aligned by 64 bytes.
 *
 * Returns NULL if deserialization fails.
 *
 * The returned bitmap must only be used in a readonly manner. The bitmap must
 * be freed using `roaring64_bitmap_free()` as normal. The backing buffer must
 * only be freed after the bitmap.
 *
 * This function is endian-sensitive. If you have a big-endian system (e.g., a
 * mainframe IBM s390x), the data format is going to be big-endian and not
 * compatible with little-endian systems.
 */
roaring64_bitmap_t *roaring64_bitmap_frozen_view(const char *buf,
                                                 size_t maxbytes);

/**
 * Iterate over the bitmap elements. The function `iterator` is called once for
 * all the values with `ptr` (can be NULL) as the second parameter of each call.
 *
 * `roaring_iterator64` is simply a pointer to a function that returns a bool
 * and takes `(uint64_t, void*)` as inputs. True means that the iteration should
 * continue, while false means that it should stop.
 *
 * Returns true if the `roaring64_iterator` returned true throughout (so that
 * all data points were necessarily visited).
 *
 * Iteration is ordered from the smallest to the largest elements.
 */
bool roaring64_bitmap_iterate(const roaring64_bitmap_t *r,
                              roaring_iterator64 iterator, void *ptr);

/**
 * Convert the bitmap to a sorted array `out`.
 *
 * Caller is responsible to ensure that there is enough memory allocated, e.g.
 * ```
 * out = malloc(roaring64_bitmap_get_cardinality(bitmap) * sizeof(uint64_t));
 * ```
 */
void roaring64_bitmap_to_uint64_array(const roaring64_bitmap_t *r,
                                      uint64_t *out);

/**
 * Create an iterator object that can be used to iterate through the values.
 * Caller is responsible for calling `roaring64_iterator_free()`.
 *
 * The iterator is initialized. If there is a value, then this iterator points
 * to the first value and `roaring64_iterator_has_value()` returns true. The
 * value can be retrieved with `roaring64_iterator_value()`.
 */
roaring64_iterator_t *roaring64_iterator_create(const roaring64_bitmap_t *r);

/**
 * Create an iterator object that can be used to iterate through the values.
 * Caller is responsible for calling `roaring64_iterator_free()`.
 *
 * The iterator is initialized. If there is a value, then this iterator points
 * to the last value and `roaring64_iterator_has_value()` returns true. The
 * value can be retrieved with `roaring64_iterator_value()`.
 */
roaring64_iterator_t *roaring64_iterator_create_last(
    const roaring64_bitmap_t *r);

/**
 * Re-initializes an existing iterator. Functionally the same as
 * `roaring64_iterator_create` without a allocation.
 */
void roaring64_iterator_reinit(const roaring64_bitmap_t *r,
                               roaring64_iterator_t *it);

/**
 * Re-initializes an existing iterator. Functionally the same as
 * `roaring64_iterator_create_last` without a allocation.
 */
void roaring64_iterator_reinit_last(const roaring64_bitmap_t *r,
                                    roaring64_iterator_t *it);

/**
 * Creates a copy of the iterator. Caller is responsible for calling
 * `roaring64_iterator_free()` on the resulting iterator.
 */
roaring64_iterator_t *roaring64_iterator_copy(const roaring64_iterator_t *it);

/**
 * Free the iterator.
 */
void roaring64_iterator_free(roaring64_iterator_t *it);

/**
 * Returns true if the iterator currently points to a value. If so, calling
 * `roaring64_iterator_value()` returns the value.
 */
bool roaring64_iterator_has_value(const roaring64_iterator_t *it);

/**
 * Returns the value the iterator currently points to. Should only be called if
 * `roaring64_iterator_has_value()` returns true.
 */
uint64_t roaring64_iterator_value(const roaring64_iterator_t *it);

/**
 * Advance the iterator. If there is a new value, then
 * `roaring64_iterator_has_value()` returns true. Values are traversed in
 * increasing order. For convenience, returns the result of
 * `roaring64_iterator_has_value()`.
 *
 * Once this returns false, `roaring64_iterator_advance` should not be called on
 * the iterator again. Calling `roaring64_iterator_previous` is allowed.
 */
bool roaring64_iterator_advance(roaring64_iterator_t *it);

/**
 * Decrement the iterator. If there is a new value, then
 * `roaring64_iterator_has_value()` returns true. Values are traversed in
 * decreasing order. For convenience, returns the result of
 * `roaring64_iterator_has_value()`.
 *
 * Once this returns false, `roaring64_iterator_previous` should not be called
 * on the iterator again. Calling `roaring64_iterator_advance` is allowed.
 */
bool roaring64_iterator_previous(roaring64_iterator_t *it);

/**
 * Move the iterator to the first value greater than or equal to `val`, if it
 * exists at or after the current position of the iterator. If there is a new
 * value, then `roaring64_iterator_has_value()` returns true. Values are
 * traversed in increasing order. For convenience, returns the result of
 * `roaring64_iterator_has_value()`.
 */
bool roaring64_iterator_move_equalorlarger(roaring64_iterator_t *it,
                                           uint64_t val);

/**
 * Reads up to `count` values from the iterator into the given `buf`. Returns
 * the number of elements read. The number of elements read can be smaller than
 * `count`, which means that there are no more elements in the bitmap.
 *
 * This function can be used together with other iterator functions.
 */
uint64_t roaring64_iterator_read(roaring64_iterator_t *it, uint64_t *buf,
                                 uint64_t count);

/**
 * Reads previous ${count} values from iterator into user-supplied ${buf}.
 * Returns the number of read elements.
 * This number can be smaller than ${count}, which means that iterator is
 * drained.
 *
 * Values are written in descending order: buf[0] is the highest (current)
 * value, buf[ret-1] is the lowest value read.
 *
 * This function satisfies semantics of reverse iteration and can be used
 * together with other iterator functions.
 *  - first value is copied from the current iterator value
 *  - after function returns, iterator is positioned at the previous element
 */
uint64_t roaring64_iterator_read_backward(roaring64_iterator_t *it,
                                          uint64_t *buf, uint64_t count);

typedef struct roaring64_range_closed_s {
    uint64_t min;
    uint64_t max;
} roaring64_range_closed_t;

/**
 * Reads next ${count} ranges from iterator into user-supplied ${buf}.
 * A range is defined as a maximal interval of consecutive values.
 * For example, the set {1,2,3,5,6} contains two ranges: [1..3] and [5..6].
 * Each range is represented as a struct {min,max}, both endpoints included.
 * Consecutive values that span internal container boundaries are merged into
 * a single range.
 *
 * Returns the number of read ranges.
 * This number can be smaller than ${count}, which means that the iterator is
 * drained.
 *
 * This function can be used together with other iterator functions.
 *  - first range will start with the current iterator value
 *  - after the function returns, the iterator is positioned at the next element
 *    after the end of the last returned range, or has_value is false if
 *    the bitmap is exhausted.
 */
size_t roaring64_iterator_read_ranges(roaring64_iterator_t *it,
                                      roaring64_range_closed_t *buf,
                                      size_t count);

/**
 * Reads previous ${count} ranges from iterator into user-supplied ${buf}.
 * A range is defined as a maximal interval of consecutive values.
 * For example, the set {1,2,3,5,6} contains two ranges: [1..3] and [5..6].
 * Each range is represented as a struct {min,max}, both endpoints included.
 * Consecutive values that span internal container boundaries are merged into
 * a single range.
 *
 * Returns the number of read ranges.
 * This number can be smaller than ${count}, which means that the iterator is
 * drained.
 *
 * Ranges are returned in reverse order, e.g. the first range returned is the
 * highest range (ending at the current value).
 *
 * This function can be used together with other iterator functions.
 *  - first range will end with the current iterator value
 *  - after the function returns, the iterator is positioned at the element
 *    before the beginning of the last returned range, or has_value is false if
 *    the bitmap is exhausted.
 */
size_t roaring64_iterator_read_prev_ranges(roaring64_iterator_t *it,
                                           roaring64_range_closed_t *buf,
                                           size_t count);

#ifdef __cplusplus
}  // extern "C"
}  // namespace roaring
}  // namespace api
#endif

#endif /* ROARING64_H */
