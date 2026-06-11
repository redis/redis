#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <roaring/containers/bitset.h>
#include <roaring/containers/containers.h>
#include <roaring/memory.h>
#include <roaring/roaring_array.h>

#ifdef __cplusplus
extern "C" {
namespace roaring {
namespace internal {
#endif

// Convention: [0,ra->size) all elements are initialized
//  [ra->size, ra->allocation_size) is junk and contains nothing needing freeing

extern inline int32_t ra_get_size(const roaring_array_t *ra);
extern inline int32_t ra_get_index(const roaring_array_t *ra, uint16_t x);

extern inline container_t *ra_get_container_at_index(const roaring_array_t *ra,
                                                     uint16_t i,
                                                     uint8_t *typecode);

extern inline void ra_unshare_container_at_index(roaring_array_t *ra,
                                                 uint16_t i);

extern inline void ra_replace_key_and_container_at_index(roaring_array_t *ra,
                                                         int32_t i,
                                                         uint16_t key,
                                                         container_t *c,
                                                         uint8_t typecode);

extern inline void ra_set_container_at_index(const roaring_array_t *ra,
                                             int32_t i, container_t *c,
                                             uint8_t typecode);

static bool realloc_array(roaring_array_t *ra, int32_t new_capacity) {
    //
    // Note: not implemented using C's realloc(), because the memory layout is
    // Struct-of-Arrays vs. Array-of-Structs:
    // https://github.com/RoaringBitmap/CRoaring/issues/256

    if (new_capacity == 0) {
        roaring_free(ra->containers);
        ra->containers = NULL;
        ra->keys = NULL;
        ra->typecodes = NULL;
        ra->allocation_size = 0;
        return true;
    }
    const size_t memoryneeded =
        new_capacity *
        (sizeof(uint16_t) + sizeof(container_t *) + sizeof(uint8_t));
    void *bigalloc = roaring_malloc(memoryneeded);
    if (!bigalloc) return false;
    void *oldbigalloc = ra->containers;
    container_t **newcontainers = (container_t **)bigalloc;
    uint16_t *newkeys = (uint16_t *)(newcontainers + new_capacity);
    uint8_t *newtypecodes = (uint8_t *)(newkeys + new_capacity);
    assert((char *)(newtypecodes + new_capacity) ==
           (char *)bigalloc + memoryneeded);
    if (ra->size > 0) {
        memcpy(newcontainers, ra->containers, sizeof(container_t *) * ra->size);
        memcpy(newkeys, ra->keys, sizeof(uint16_t) * ra->size);
        memcpy(newtypecodes, ra->typecodes, sizeof(uint8_t) * ra->size);
    }
    ra->containers = newcontainers;
    ra->keys = newkeys;
    ra->typecodes = newtypecodes;
    ra->allocation_size = new_capacity;
    roaring_free(oldbigalloc);
    return true;
}

bool ra_init_with_capacity(roaring_array_t *new_ra, uint32_t cap) {
    if (!new_ra) return false;
    ra_init(new_ra);

    // Containers hold 64Ki elements, so 64Ki containers is enough to hold
    // `0x10000 * 0x10000` (all 2^32) elements
    if (cap > 0x10000) {
        cap = 0x10000;
    }

    if (cap > 0) {
        void *bigalloc = roaring_malloc(
            cap * (sizeof(uint16_t) + sizeof(container_t *) + sizeof(uint8_t)));
        if (bigalloc == NULL) return false;
        new_ra->containers = (container_t **)bigalloc;
        new_ra->keys = (uint16_t *)(new_ra->containers + cap);
        new_ra->typecodes = (uint8_t *)(new_ra->keys + cap);
        // Narrowing is safe because of above check
        new_ra->allocation_size = (int32_t)cap;
    }
    return true;
}

int ra_shrink_to_fit(roaring_array_t *ra) {
    int savings = (ra->allocation_size - ra->size) *
                  (sizeof(uint16_t) + sizeof(container_t *) + sizeof(uint8_t));
    if (!realloc_array(ra, ra->size)) {
        return 0;
    }
    ra->allocation_size = ra->size;
    return savings;
}

void ra_init(roaring_array_t *new_ra) {
    if (!new_ra) {
        return;
    }
    new_ra->keys = NULL;
    new_ra->containers = NULL;
    new_ra->typecodes = NULL;

    new_ra->allocation_size = 0;
    new_ra->size = 0;
    new_ra->flags = 0;
}

bool ra_overwrite(const roaring_array_t *source, roaring_array_t *dest,
                  bool copy_on_write) {
    ra_clear_containers(dest);  // we are going to overwrite them
    if (source->size == 0) {    // Note: can't call memcpy(NULL), even w/size
        dest->size = 0;         // <--- This is important.
        return true;            // output was just cleared, so they match
    }
    if (dest->allocation_size < source->size) {
        if (!realloc_array(dest, source->size)) {
            return false;
        }
    }
    dest->size = source->size;
    memcpy(dest->keys, source->keys, dest->size * sizeof(uint16_t));
    // we go through the containers, turning them into shared containers...
    if (copy_on_write) {
        for (int32_t i = 0; i < dest->size; ++i) {
            source->containers[i] = get_copy_of_container(
                source->containers[i], &source->typecodes[i], copy_on_write);
        }
        // we do a shallow copy to the other bitmap
        memcpy(dest->containers, source->containers,
               dest->size * sizeof(container_t *));
        memcpy(dest->typecodes, source->typecodes,
               dest->size * sizeof(uint8_t));
    } else {
        memcpy(dest->typecodes, source->typecodes,
               dest->size * sizeof(uint8_t));
        for (int32_t i = 0; i < dest->size; i++) {
            dest->containers[i] =
                container_clone(source->containers[i], source->typecodes[i]);
            if (dest->containers[i] == NULL) {
                for (int32_t j = 0; j < i; j++) {
                    container_free(dest->containers[j], dest->typecodes[j]);
                }
                ra_clear_without_containers(dest);
                return false;
            }
        }
    }
    return true;
}

void ra_clear_containers(roaring_array_t *ra) {
    for (int32_t i = 0; i < ra->size; ++i) {
        container_free(ra->containers[i], ra->typecodes[i]);
    }
}

void ra_reset(roaring_array_t *ra) {
    ra_clear_containers(ra);
    ra->size = 0;
    ra_shrink_to_fit(ra);
}

void ra_clear_without_containers(roaring_array_t *ra) {
    roaring_free(
        ra->containers);  // keys and typecodes are allocated with containers
    ra->size = 0;
    ra->allocation_size = 0;
    ra->containers = NULL;
    ra->keys = NULL;
    ra->typecodes = NULL;
}

void ra_clear(roaring_array_t *ra) {
    ra_clear_containers(ra);
    ra_clear_without_containers(ra);
}

bool extend_array(roaring_array_t *ra, int32_t k) {
    int32_t desired_size = ra->size + k;
    const int32_t max_containers = 65536;
    assert(desired_size <= max_containers);
    if (desired_size > ra->allocation_size) {
        int32_t new_capacity =
            (ra->size < 1024) ? 2 * desired_size : 5 * desired_size / 4;
        if (new_capacity > max_containers) {
            new_capacity = max_containers;
        }

        return realloc_array(ra, new_capacity);
    }
    return true;
}

void ra_append(roaring_array_t *ra, uint16_t key, container_t *c,
               uint8_t typecode) {
    extend_array(ra, 1);
    const int32_t pos = ra->size;

    ra->keys[pos] = key;
    ra->containers[pos] = c;
    ra->typecodes[pos] = typecode;
    ra->size++;
}

void ra_append_copy(roaring_array_t *ra, const roaring_array_t *sa,
                    uint16_t index, bool copy_on_write) {
    extend_array(ra, 1);
    const int32_t pos = ra->size;

    // old contents is junk that does not need freeing
    ra->keys[pos] = sa->keys[index];
    // the shared container will be in two bitmaps
    if (copy_on_write) {
        sa->containers[index] = get_copy_of_container(
            sa->containers[index], &sa->typecodes[index], copy_on_write);
        ra->containers[pos] = sa->containers[index];
        ra->typecodes[pos] = sa->typecodes[index];
    } else {
        ra->containers[pos] =
            container_clone(sa->containers[index], sa->typecodes[index]);
        ra->typecodes[pos] = sa->typecodes[index];
    }
    ra->size++;
}

void ra_append_copies_until(roaring_array_t *ra, const roaring_array_t *sa,
                            uint16_t stopping_key, bool copy_on_write) {
    for (int32_t i = 0; i < sa->size; ++i) {
        if (sa->keys[i] >= stopping_key) break;
        ra_append_copy(ra, sa, (uint16_t)i, copy_on_write);
    }
}

void ra_append_copy_range(roaring_array_t *ra, const roaring_array_t *sa,
                          int32_t start_index, int32_t end_index,
                          bool copy_on_write) {
    extend_array(ra, end_index - start_index);
    for (int32_t i = start_index; i < end_index; ++i) {
        const int32_t pos = ra->size;
        ra->keys[pos] = sa->keys[i];
        if (copy_on_write) {
            sa->containers[i] = get_copy_of_container(
                sa->containers[i], &sa->typecodes[i], copy_on_write);
            ra->containers[pos] = sa->containers[i];
            ra->typecodes[pos] = sa->typecodes[i];
        } else {
            ra->containers[pos] =
                container_clone(sa->containers[i], sa->typecodes[i]);
            ra->typecodes[pos] = sa->typecodes[i];
        }
        ra->size++;
    }
}

void ra_append_copies_after(roaring_array_t *ra, const roaring_array_t *sa,
                            uint16_t before_start, bool copy_on_write) {
    int start_location = ra_get_index(sa, before_start);
    if (start_location >= 0)
        ++start_location;
    else
        start_location = -start_location - 1;
    ra_append_copy_range(ra, sa, start_location, sa->size, copy_on_write);
}

void ra_append_move_range(roaring_array_t *ra, roaring_array_t *sa,
                          int32_t start_index, int32_t end_index) {
    extend_array(ra, end_index - start_index);

    for (int32_t i = start_index; i < end_index; ++i) {
        const int32_t pos = ra->size;

        ra->keys[pos] = sa->keys[i];
        ra->containers[pos] = sa->containers[i];
        ra->typecodes[pos] = sa->typecodes[i];
        ra->size++;
    }
}

void ra_append_range(roaring_array_t *ra, roaring_array_t *sa,
                     int32_t start_index, int32_t end_index,
                     bool copy_on_write) {
    extend_array(ra, end_index - start_index);

    for (int32_t i = start_index; i < end_index; ++i) {
        const int32_t pos = ra->size;
        ra->keys[pos] = sa->keys[i];
        if (copy_on_write) {
            sa->containers[i] = get_copy_of_container(
                sa->containers[i], &sa->typecodes[i], copy_on_write);
            ra->containers[pos] = sa->containers[i];
            ra->typecodes[pos] = sa->typecodes[i];
        } else {
            ra->containers[pos] =
                container_clone(sa->containers[i], sa->typecodes[i]);
            ra->typecodes[pos] = sa->typecodes[i];
        }
        ra->size++;
    }
}

container_t *ra_get_container(roaring_array_t *ra, uint16_t x,
                              uint8_t *typecode) {
    int i = binarySearch(ra->keys, (int32_t)ra->size, x);
    if (i < 0) return NULL;
    *typecode = ra->typecodes[i];
    return ra->containers[i];
}

extern inline container_t *ra_get_container_at_index(const roaring_array_t *ra,
                                                     uint16_t i,
                                                     uint8_t *typecode);

extern inline uint16_t ra_get_key_at_index(const roaring_array_t *ra,
                                           uint16_t i);

extern inline int32_t ra_get_index(const roaring_array_t *ra, uint16_t x);

extern inline int32_t ra_advance_until(const roaring_array_t *ra, uint16_t x,
                                       int32_t pos);

// everything skipped over is freed
int32_t ra_advance_until_freeing(roaring_array_t *ra, uint16_t x, int32_t pos) {
    while (pos < ra->size && ra->keys[pos] < x) {
        container_free(ra->containers[pos], ra->typecodes[pos]);
        ++pos;
    }
    return pos;
}

void ra_insert_new_key_value_at(roaring_array_t *ra, int32_t i, uint16_t key,
                                container_t *c, uint8_t typecode) {
    extend_array(ra, 1);
    // May be an optimization opportunity with DIY memmove
    memmove(&(ra->keys[i + 1]), &(ra->keys[i]),
            sizeof(uint16_t) * (ra->size - i));
    memmove(&(ra->containers[i + 1]), &(ra->containers[i]),
            sizeof(container_t *) * (ra->size - i));
    memmove(&(ra->typecodes[i + 1]), &(ra->typecodes[i]),
            sizeof(uint8_t) * (ra->size - i));
    ra->keys[i] = key;
    ra->containers[i] = c;
    ra->typecodes[i] = typecode;
    ra->size++;
}

// note: Java routine set things to 0, enabling GC.
// Java called it "resize" but it was always used to downsize.
// Allowing upsize would break the conventions about
// valid containers below ra->size.

void ra_downsize(roaring_array_t *ra, int32_t new_length) {
    assert(new_length <= ra->size);
    ra->size = new_length;
}

void ra_remove_at_index(roaring_array_t *ra, int32_t i) {
    memmove(&(ra->containers[i]), &(ra->containers[i + 1]),
            sizeof(container_t *) * (ra->size - i - 1));
    memmove(&(ra->keys[i]), &(ra->keys[i + 1]),
            sizeof(uint16_t) * (ra->size - i - 1));
    memmove(&(ra->typecodes[i]), &(ra->typecodes[i + 1]),
            sizeof(uint8_t) * (ra->size - i - 1));
    ra->size--;
}

void ra_remove_at_index_and_free(roaring_array_t *ra, int32_t i) {
    container_free(ra->containers[i], ra->typecodes[i]);
    ra_remove_at_index(ra, i);
}

// used in inplace andNot only, to slide left the containers from
// the mutated RoaringBitmap that are after the largest container of
// the argument RoaringBitmap.  In use it should be followed by a call to
// downsize.
//
void ra_copy_range(roaring_array_t *ra, uint32_t begin, uint32_t end,
                   uint32_t new_begin) {
    assert(begin <= end);
    assert(new_begin < begin);

    const int range = end - begin;

    // We ensure to previously have freed overwritten containers
    // that are not copied elsewhere

    memmove(&(ra->containers[new_begin]), &(ra->containers[begin]),
            sizeof(container_t *) * range);
    memmove(&(ra->keys[new_begin]), &(ra->keys[begin]),
            sizeof(uint16_t) * range);
    memmove(&(ra->typecodes[new_begin]), &(ra->typecodes[begin]),
            sizeof(uint8_t) * range);
}

void ra_shift_tail(roaring_array_t *ra, int32_t count, int32_t distance) {
    if (distance > 0) {
        extend_array(ra, distance);
    }
    int32_t srcpos = ra->size - count;
    int32_t dstpos = srcpos + distance;
    memmove(&(ra->keys[dstpos]), &(ra->keys[srcpos]), sizeof(uint16_t) * count);
    memmove(&(ra->containers[dstpos]), &(ra->containers[srcpos]),
            sizeof(container_t *) * count);
    memmove(&(ra->typecodes[dstpos]), &(ra->typecodes[srcpos]),
            sizeof(uint8_t) * count);
    ra->size += distance;
}

void ra_to_uint32_array(const roaring_array_t *ra, uint32_t *ans) {
    size_t ctr = 0;
    for (int32_t i = 0; i < ra->size; ++i) {
        int num_added = container_to_uint32_array(
            ans + ctr, ra->containers[i], ra->typecodes[i],
            ((uint32_t)ra->keys[i]) << 16);
        ctr += num_added;
    }
}

bool ra_has_run_container(const roaring_array_t *ra) {
    for (int32_t k = 0; k < ra->size; ++k) {
        if (get_container_type(ra->containers[k], ra->typecodes[k]) ==
            RUN_CONTAINER_TYPE)
            return true;
    }
    return false;
}

uint32_t ra_portable_header_size(const roaring_array_t *ra) {
    if (ra_has_run_container(ra)) {
        if (ra->size <
            NO_OFFSET_THRESHOLD) {  // for small bitmaps, we omit the offsets
            return 4 + (ra->size + 7) / 8 + 4 * ra->size;
        }
        return 4 + (ra->size + 7) / 8 +
               8 * ra->size;  // - 4 because we pack the size with the cookie
    } else {
        return 4 + 4 + 8 * ra->size;
    }
}

size_t ra_portable_size_in_bytes(const roaring_array_t *ra) {
    size_t count = ra_portable_header_size(ra);

    for (int32_t k = 0; k < ra->size; ++k) {
        count += container_size_in_bytes(ra->containers[k], ra->typecodes[k]);
    }
    return count;
}

// This function is endian-sensitive.
size_t ra_portable_serialize(const roaring_array_t *ra, char *buf) {
    char *initbuf = buf;
    uint32_t startOffset = 0;
    bool hasrun = ra_has_run_container(ra);
    if (hasrun) {
        uint32_t cookie = SERIAL_COOKIE | ((uint32_t)(ra->size - 1) << 16);
        memcpy(buf, &cookie, sizeof(cookie));
        buf += sizeof(cookie);
        uint32_t s = (ra->size + 7) / 8;
        memset(buf, 0, s);
        for (int32_t i = 0; i < ra->size; ++i) {
            if (get_container_type(ra->containers[i], ra->typecodes[i]) ==
                RUN_CONTAINER_TYPE) {
                buf[i / 8] |= 1 << (i % 8);
            }
        }
        buf += s;
        if (ra->size < NO_OFFSET_THRESHOLD) {
            startOffset = 4 + 4 * ra->size + s;
        } else {
            startOffset = 4 + 8 * ra->size + s;
        }
    } else {  // backwards compatibility
        uint32_t cookie = SERIAL_COOKIE_NO_RUNCONTAINER;

        memcpy(buf, &cookie, sizeof(cookie));
        buf += sizeof(cookie);
        memcpy(buf, &ra->size, sizeof(ra->size));
        buf += sizeof(ra->size);

        startOffset = 4 + 4 + 4 * ra->size + 4 * ra->size;
    }
    for (int32_t k = 0; k < ra->size; ++k) {
        memcpy(buf, &ra->keys[k], sizeof(ra->keys[k]));
        buf += sizeof(ra->keys[k]);
        // get_cardinality returns a value in [1,1<<16], subtracting one
        // we get [0,1<<16 - 1] which fits in 16 bits
        uint16_t card = (uint16_t)(container_get_cardinality(ra->containers[k],
                                                             ra->typecodes[k]) -
                                   1);
        memcpy(buf, &card, sizeof(card));
        buf += sizeof(card);
    }
    if ((!hasrun) || (ra->size >= NO_OFFSET_THRESHOLD)) {
        // writing the containers offsets
        for (int32_t k = 0; k < ra->size; k++) {
            memcpy(buf, &startOffset, sizeof(startOffset));
            buf += sizeof(startOffset);
            startOffset =
                startOffset +
                container_size_in_bytes(ra->containers[k], ra->typecodes[k]);
        }
    }
    for (int32_t k = 0; k < ra->size; ++k) {
        buf += container_write(ra->containers[k], ra->typecodes[k], buf);
    }
    return buf - initbuf;
}

// Quickly checks whether there is a serialized bitmap at the pointer,
// not exceeding size "maxbytes" in bytes. This function does not allocate
// memory dynamically.
//
// This function returns 0 if and only if no valid bitmap is found.
// Otherwise, it returns how many bytes are occupied.
//
size_t ra_portable_deserialize_size(const char *buf, const size_t maxbytes) {
    size_t bytestotal = sizeof(int32_t);  // for cookie
    if (bytestotal > maxbytes) return 0;
    uint32_t cookie;
    memcpy(&cookie, buf, sizeof(int32_t));
    buf += sizeof(uint32_t);
    if ((cookie & 0xFFFF) != SERIAL_COOKIE &&
        cookie != SERIAL_COOKIE_NO_RUNCONTAINER) {
        return 0;
    }
    int32_t size;

    if ((cookie & 0xFFFF) == SERIAL_COOKIE)
        size = (cookie >> 16) + 1;
    else {
        bytestotal += sizeof(int32_t);
        if (bytestotal > maxbytes) return 0;
        memcpy(&size, buf, sizeof(int32_t));
        buf += sizeof(uint32_t);
    }
    if (size > (1 << 16) || size < 0) {
        return 0;
    }
    char *bitmapOfRunContainers = NULL;
    bool hasrun = (cookie & 0xFFFF) == SERIAL_COOKIE;
    if (hasrun) {
        int32_t s = (size + 7) / 8;
        bytestotal += s;
        if (bytestotal > maxbytes) return 0;
        bitmapOfRunContainers = (char *)buf;
        buf += s;
    }
    bytestotal += size * 2 * sizeof(uint16_t);
    if (bytestotal > maxbytes) return 0;
    const char *keyscards = buf;
    buf += size * 2 * sizeof(uint16_t);
    if ((!hasrun) || (size >= NO_OFFSET_THRESHOLD)) {
        // skipping the offsets
        bytestotal += size * 4;
        if (bytestotal > maxbytes) return 0;
        buf += size * 4;
    }
    // Reading the containers
    for (int32_t k = 0; k < size; ++k) {
        uint16_t tmp;
        memcpy(&tmp, keyscards + 4 * k + 2, sizeof(tmp));
        uint32_t thiscard = tmp + 1;
        bool isbitmap = (thiscard > DEFAULT_MAX_SIZE);
        bool isrun = false;
        if (hasrun) {
            if ((bitmapOfRunContainers[k / 8] & (1 << (k % 8))) != 0) {
                isbitmap = false;
                isrun = true;
            }
        }
        if (isbitmap) {
            size_t containersize =
                BITSET_CONTAINER_SIZE_IN_WORDS * sizeof(uint64_t);
            bytestotal += containersize;
            if (bytestotal > maxbytes) return 0;
            buf += containersize;
        } else if (isrun) {
            bytestotal += sizeof(uint16_t);
            if (bytestotal > maxbytes) return 0;
            uint16_t n_runs;
            memcpy(&n_runs, buf, sizeof(uint16_t));
            buf += sizeof(uint16_t);
            size_t containersize = n_runs * sizeof(rle16_t);
            bytestotal += containersize;
            if (bytestotal > maxbytes) return 0;
            buf += containersize;
        } else {
            size_t containersize = thiscard * sizeof(uint16_t);
            bytestotal += containersize;
            if (bytestotal > maxbytes) return 0;
            buf += containersize;
        }
    }
    return bytestotal;
}

// This function populates answer from the content of buf (reading up to
// maxbytes bytes). The function returns false if a properly serialized bitmap
// cannot be found. If it returns true, readbytes is populated by how many bytes
// were read, we have that *readbytes <= maxbytes.
//
// This function is endian-sensitive.
bool ra_portable_deserialize(roaring_array_t *answer, const char *buf,
                             const size_t maxbytes, size_t *readbytes) {
    *readbytes = sizeof(int32_t);  // for cookie
    if (*readbytes > maxbytes) {
        // Ran out of bytes while reading first 4 bytes.
        return false;
    }
    uint32_t cookie;
    memcpy(&cookie, buf, sizeof(int32_t));
    buf += sizeof(uint32_t);
    if ((cookie & 0xFFFF) != SERIAL_COOKIE &&
        cookie != SERIAL_COOKIE_NO_RUNCONTAINER) {
        // "I failed to find one of the right cookies.
        return false;
    }
    int32_t size;

    if ((cookie & 0xFFFF) == SERIAL_COOKIE)
        size = (cookie >> 16) + 1;
    else {
        *readbytes += sizeof(int32_t);
        if (*readbytes > maxbytes) {
            // Ran out of bytes while reading second part of the cookie.
            return false;
        }
        memcpy(&size, buf, sizeof(int32_t));
        buf += sizeof(uint32_t);
    }
    if (size < 0) {
        // You cannot have a negative number of containers, the data must be
        // corrupted.
        return false;
    }
    if (size > (1 << 16)) {
        // You cannot have so many containers, the data must be corrupted.
        return false;
    }
    const char *bitmapOfRunContainers = NULL;
    bool hasrun = (cookie & 0xFFFF) == SERIAL_COOKIE;
    if (hasrun) {
        int32_t s = (size + 7) / 8;
        *readbytes += s;
        if (*readbytes > maxbytes) {  // data is corrupted?
            // Ran out of bytes while reading run bitmap.
            return false;
        }
        bitmapOfRunContainers = buf;
        buf += s;
    }
    const char *keyscards = buf;

    *readbytes += size * 2 * sizeof(uint16_t);
    if (*readbytes > maxbytes) {
        // Ran out of bytes while reading key-cardinality array.
        return false;
    }
    buf += size * 2 * sizeof(uint16_t);

    bool is_ok = ra_init_with_capacity(answer, size);
    if (!is_ok) {
        // Failed to allocate memory for roaring array. Bailing out.
        return false;
    }

    for (int32_t k = 0; k < size; ++k) {
        uint16_t tmp;
        memcpy(&tmp, keyscards + 4 * k, sizeof(tmp));
        answer->keys[k] = tmp;
    }
    if ((!hasrun) || (size >= NO_OFFSET_THRESHOLD)) {
        *readbytes += size * 4;
        if (*readbytes > maxbytes) {  // data is corrupted?
            // Ran out of bytes while reading offsets.
            ra_clear(answer);  // we need to clear the containers already
                               // allocated, and the roaring array
            return false;
        }

        // skipping the offsets
        buf += size * 4;
    }
    // Reading the containers
    for (int32_t k = 0; k < size; ++k) {
        uint16_t tmp;
        memcpy(&tmp, keyscards + 4 * k + 2, sizeof(tmp));
        uint32_t thiscard = tmp + 1;
        bool isbitmap = (thiscard > DEFAULT_MAX_SIZE);
        bool isrun = false;
        if (hasrun) {
            if ((bitmapOfRunContainers[k / 8] & (1 << (k % 8))) != 0) {
                isbitmap = false;
                isrun = true;
            }
        }
        if (isbitmap) {
            // we check that the read is allowed
            size_t containersize =
                BITSET_CONTAINER_SIZE_IN_WORDS * sizeof(uint64_t);
            *readbytes += containersize;
            if (*readbytes > maxbytes) {
                // Running out of bytes while reading a bitset container.
                ra_clear(answer);  // we need to clear the containers already
                                   // allocated, and the roaring array
                return false;
            }
            // it is now safe to read
            bitset_container_t *c = bitset_container_create();
            if (c == NULL) {  // memory allocation failure
                // Failed to allocate memory for a bitset container.
                ra_clear(answer);  // we need to clear the containers already
                                   // allocated, and the roaring array
                return false;
            }
            answer->size++;
            buf += bitset_container_read(thiscard, c, buf);
            answer->containers[k] = c;
            answer->typecodes[k] = BITSET_CONTAINER_TYPE;
        } else if (isrun) {
            // we check that the read is allowed
            *readbytes += sizeof(uint16_t);
            if (*readbytes > maxbytes) {
                // Running out of bytes while reading a run container (header).
                ra_clear(answer);  // we need to clear the containers already
                                   // allocated, and the roaring array
                return false;
            }
            uint16_t n_runs;
            memcpy(&n_runs, buf, sizeof(uint16_t));
            size_t containersize = n_runs * sizeof(rle16_t);
            *readbytes += containersize;
            if (*readbytes > maxbytes) {  // data is corrupted?
                // Running out of bytes while reading a run container.
                ra_clear(answer);  // we need to clear the containers already
                                   // allocated, and the roaring array
                return false;
            }
            // it is now safe to read

            run_container_t *c = run_container_create();
            if (c == NULL) {  // memory allocation failure
                // Failed to allocate memory for a run container.
                ra_clear(answer);  // we need to clear the containers already
                                   // allocated, and the roaring array
                return false;
            }
            answer->size++;
            buf += run_container_read(thiscard, c, buf);
            answer->containers[k] = c;
            answer->typecodes[k] = RUN_CONTAINER_TYPE;
        } else {
            // we check that the read is allowed
            size_t containersize = thiscard * sizeof(uint16_t);
            *readbytes += containersize;
            if (*readbytes > maxbytes) {  // data is corrupted?
                // Running out of bytes while reading an array container.
                ra_clear(answer);  // we need to clear the containers already
                                   // allocated, and the roaring array
                return false;
            }
            // it is now safe to read
            array_container_t *c =
                array_container_create_given_capacity(thiscard);
            if (c == NULL) {  // memory allocation failure
                // Failed to allocate memory for an array container.
                ra_clear(answer);  // we need to clear the containers already
                                   // allocated, and the roaring array
                return false;
            }
            answer->size++;
            buf += array_container_read(thiscard, c, buf);
            answer->containers[k] = c;
            answer->typecodes[k] = ARRAY_CONTAINER_TYPE;
        }
    }
    return true;
}

#ifdef __cplusplus
}
}
}  // extern "C" { namespace roaring { namespace internal {
#endif
