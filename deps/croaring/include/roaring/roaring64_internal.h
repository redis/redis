#ifndef ROARING64_INTERNAL_H
#define ROARING64_INTERNAL_H

/* Local Redis patch (not part of upstream CRoaring).
 *
 * Upstream defines `struct roaring64_bitmap_s` privately inside
 * src/roaring64.c. Redis needs the layout to walk every allocation behind a
 * 64-bit bitmap for MEMORY USAGE accounting, fork-child page dismissal and
 * active defragmentation (see src/bitroar.c), exactly like it already
 * does for the 32-bit `roaring_bitmap_t` whose layout upstream does expose.
 * This header moves the struct definition verbatim out of roaring64.c so both
 * files share one definition.
 *
 * This header must be included after a header that defines `container_t`
 * (e.g. <roaring/containers/containers.h>); roaring64.c includes containers.h
 * last on purpose, so this header cannot pull it in itself.
 */

#include <roaring/art/art.h>
#include <roaring/roaring64.h>

#ifdef __cplusplus
extern "C" {
namespace roaring {
namespace api {
#endif

struct roaring64_bitmap_s {
    art_t art;
    uint8_t flags;
    uint64_t first_free;
    uint64_t capacity;
    container_t **containers;
};

/* Leaf type of the ART used to keep the high 48 bits of each entry.
 * Low 8 bits: typecode
 * High 56 bits: container index */
static inline uint8_t roaring64_leaf_typecode(roaring64_leaf_t leaf) {
    return (uint8_t)leaf;
}

static inline uint64_t roaring64_leaf_index(roaring64_leaf_t leaf) {
    return leaf >> 8;
}

#ifdef __cplusplus
}  // namespace api
}  // namespace roaring
}  // extern "C"
#endif

#endif /* ROARING64_INTERNAL_H */
