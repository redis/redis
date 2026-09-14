#include <roaring/roaring.h>
#include <roaring/roaring_array.h>

#ifdef __cplusplus
using namespace ::roaring::internal;

extern "C" {
namespace roaring {
namespace api {
#endif

struct roaring_pq_element_s {
    uint64_t size;
    bool is_temporary;
    roaring_bitmap_t *bitmap;
};

typedef struct roaring_pq_element_s roaring_pq_element_t;

struct roaring_pq_s {
    roaring_pq_element_t *elements;
    uint64_t size;
};

typedef struct roaring_pq_s roaring_pq_t;

static inline bool compare(roaring_pq_element_t *t1, roaring_pq_element_t *t2) {
    return t1->size < t2->size;
}

static void pq_add(roaring_pq_t *pq, roaring_pq_element_t *t) {
    uint64_t i = pq->size;
    pq->elements[pq->size++] = *t;
    while (i > 0) {
        uint64_t p = (i - 1) >> 1;
        roaring_pq_element_t ap = pq->elements[p];
        if (!compare(t, &ap)) break;
        pq->elements[i] = ap;
        i = p;
    }
    pq->elements[i] = *t;
}

static void pq_free(roaring_pq_t *pq) { roaring_free(pq); }

static void percolate_down(roaring_pq_t *pq, uint32_t i) {
    uint32_t size = (uint32_t)pq->size;
    uint32_t hsize = size >> 1;
    roaring_pq_element_t ai = pq->elements[i];
    while (i < hsize) {
        uint32_t l = (i << 1) + 1;
        uint32_t r = l + 1;
        roaring_pq_element_t bestc = pq->elements[l];
        if (r < size) {
            if (compare(pq->elements + r, &bestc)) {
                l = r;
                bestc = pq->elements[r];
            }
        }
        if (!compare(&bestc, &ai)) {
            break;
        }
        pq->elements[i] = bestc;
        i = l;
    }
    pq->elements[i] = ai;
}

static roaring_pq_t *create_pq(const roaring_bitmap_t **arr, uint32_t length) {
    size_t alloc_size =
        sizeof(roaring_pq_t) + sizeof(roaring_pq_element_t) * length;
    roaring_pq_t *answer = (roaring_pq_t *)roaring_malloc(alloc_size);
    answer->elements = (roaring_pq_element_t *)(answer + 1);
    answer->size = length;
    for (uint32_t i = 0; i < length; i++) {
        answer->elements[i].bitmap = (roaring_bitmap_t *)arr[i];
        answer->elements[i].is_temporary = false;
        answer->elements[i].size =
            roaring_bitmap_portable_size_in_bytes(arr[i]);
    }
    for (int32_t i = (length >> 1); i >= 0; i--) {
        percolate_down(answer, i);
    }
    return answer;
}

static roaring_pq_element_t pq_poll(roaring_pq_t *pq) {
    roaring_pq_element_t ans = *pq->elements;
    if (pq->size > 1) {
        pq->elements[0] = pq->elements[--pq->size];
        percolate_down(pq, 0);
    } else
        --pq->size;
    // memmove(pq->elements,pq->elements+1,(pq->size-1)*sizeof(roaring_pq_element_t));--pq->size;
    return ans;
}

// this function consumes and frees the inputs
static roaring_bitmap_t *lazy_or_from_lazy_inputs(roaring_bitmap_t *x1,
                                                  roaring_bitmap_t *x2) {
    uint8_t result_type = 0;
    const int length1 = ra_get_size(&x1->high_low_container),
              length2 = ra_get_size(&x2->high_low_container);
    if (0 == length1) {
        roaring_bitmap_free(x1);
        return x2;
    }
    if (0 == length2) {
        roaring_bitmap_free(x2);
        return x1;
    }
    uint32_t neededcap = length1 > length2 ? length2 : length1;
    roaring_bitmap_t *answer = roaring_bitmap_create_with_capacity(neededcap);
    int pos1 = 0, pos2 = 0;
    uint8_t type1, type2;
    uint16_t s1 = ra_get_key_at_index(&x1->high_low_container, (uint16_t)pos1);
    uint16_t s2 = ra_get_key_at_index(&x2->high_low_container, (uint16_t)pos2);
    while (true) {
        if (s1 == s2) {
            // todo: unsharing can be inefficient as it may create a clone where
            // none
            // is needed, but it has the benefit of being easy to reason about.

            ra_unshare_container_at_index(&x1->high_low_container,
                                          (uint16_t)pos1);
            container_t *c1 = ra_get_container_at_index(&x1->high_low_container,
                                                        (uint16_t)pos1, &type1);
            assert(type1 != SHARED_CONTAINER_TYPE);

            ra_unshare_container_at_index(&x2->high_low_container,
                                          (uint16_t)pos2);
            container_t *c2 = ra_get_container_at_index(&x2->high_low_container,
                                                        (uint16_t)pos2, &type2);
            assert(type2 != SHARED_CONTAINER_TYPE);

            container_t *c;

            if ((type2 == BITSET_CONTAINER_TYPE) &&
                (type1 != BITSET_CONTAINER_TYPE)) {
                c = container_lazy_ior(c2, type2, c1, type1, &result_type);
                container_free(c1, type1);
                if (c != c2) {
                    container_free(c2, type2);
                }
            } else {
                c = container_lazy_ior(c1, type1, c2, type2, &result_type);
                container_free(c2, type2);
                if (c != c1) {
                    container_free(c1, type1);
                }
            }
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
            ra_append(&answer->high_low_container, s1, c1, type1);
            pos1++;
            if (pos1 == length1) break;
            s1 = ra_get_key_at_index(&x1->high_low_container, (uint16_t)pos1);

        } else {  // s1 > s2
            container_t *c2 = ra_get_container_at_index(&x2->high_low_container,
                                                        (uint16_t)pos2, &type2);
            ra_append(&answer->high_low_container, s2, c2, type2);
            pos2++;
            if (pos2 == length2) break;
            s2 = ra_get_key_at_index(&x2->high_low_container, (uint16_t)pos2);
        }
    }
    if (pos1 == length1) {
        ra_append_move_range(&answer->high_low_container,
                             &x2->high_low_container, pos2, length2);
    } else if (pos2 == length2) {
        ra_append_move_range(&answer->high_low_container,
                             &x1->high_low_container, pos1, length1);
    }
    ra_clear_without_containers(&x1->high_low_container);
    ra_clear_without_containers(&x2->high_low_container);
    roaring_free(x1);
    roaring_free(x2);
    return answer;
}

/**
 * Compute the union of 'number' bitmaps using a heap. This can
 * sometimes be faster than roaring_bitmap_or_many which uses
 * a naive algorithm. Caller is responsible for freeing the
 * result.
 */
roaring_bitmap_t *roaring_bitmap_or_many_heap(uint32_t number,
                                              const roaring_bitmap_t **x) {
    if (number == 0) {
        return roaring_bitmap_create();
    }
    if (number == 1) {
        return roaring_bitmap_copy(x[0]);
    }
    roaring_pq_t *pq = create_pq(x, number);
    while (pq->size > 1) {
        roaring_pq_element_t x1 = pq_poll(pq);
        roaring_pq_element_t x2 = pq_poll(pq);

        if (x1.is_temporary && x2.is_temporary) {
            roaring_bitmap_t *newb =
                lazy_or_from_lazy_inputs(x1.bitmap, x2.bitmap);
            // should normally return a fresh new bitmap *except* that
            // it can return x1.bitmap or x2.bitmap in degenerate cases
            bool temporary = !((newb == x1.bitmap) && (newb == x2.bitmap));
            uint64_t bsize = roaring_bitmap_portable_size_in_bytes(newb);
            roaring_pq_element_t newelement = {
                .size = bsize, .is_temporary = temporary, .bitmap = newb};
            pq_add(pq, &newelement);
        } else if (x2.is_temporary) {
            roaring_bitmap_lazy_or_inplace(x2.bitmap, x1.bitmap, false);
            x2.size = roaring_bitmap_portable_size_in_bytes(x2.bitmap);
            pq_add(pq, &x2);
        } else if (x1.is_temporary) {
            roaring_bitmap_lazy_or_inplace(x1.bitmap, x2.bitmap, false);
            x1.size = roaring_bitmap_portable_size_in_bytes(x1.bitmap);

            pq_add(pq, &x1);
        } else {
            roaring_bitmap_t *newb =
                roaring_bitmap_lazy_or(x1.bitmap, x2.bitmap, false);
            uint64_t bsize = roaring_bitmap_portable_size_in_bytes(newb);
            roaring_pq_element_t newelement = {
                .size = bsize, .is_temporary = true, .bitmap = newb};

            pq_add(pq, &newelement);
        }
    }
    roaring_pq_element_t X = pq_poll(pq);
    roaring_bitmap_t *answer = X.bitmap;
    roaring_bitmap_repair_after_lazy(answer);
    pq_free(pq);
    return answer;
}

#ifdef __cplusplus
}
}
}  // extern "C" { namespace roaring { namespace api {
#endif
