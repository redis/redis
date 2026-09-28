/*
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Copyright (c) 2024-present, Valkey contributors.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * Portions of this file are available under BSD3 terms; see REDISCONTRIBUTIONS for more information.
 */

#ifndef T_SET_ENCODING_H
#define T_SET_ENCODING_H

#include "server.h"

/* Per-encoding operations for the set type. Each function operates on a robj
 * of type OBJ_SET encoded with whichever encoding the table instance below
 * belongs to (intset, listpack or hash table). This lets t_set.c dispatch to
 * the right implementation without an if/else chain on robj->encoding.
 *
 * rawAdd() attempts to insert a member under the object's *current*
 * encoding. It returns 1 if added, 0 if the member already existed, or -1
 * if the value cannot be added as-is (a configured limit would be
 * exceeded, or the encoding structurally can't represent the value) - in
 * which case it does not mutate the object at all.
 * Note that after_convert is set to 1 when rawAdd is called after a conversion,
 * so it is guaranteed that the item to add doesn't already exist in the set,
 * and therefore there is no need to search for it. Also, for listpack, the
 * listpack may be shrink after adding the new item if too much space was
 * allocated for it during conversion.
 * target_enc is set to the encoding that the caller should convert to if rawAdd returns -1.
 */
typedef struct {
    /* return 0 if the member already exists, 1 if added, -1 if it cannot be added (due to encoding mismatch or size limits)*/
    int (*rawAdd)(robj *set, char *str, size_t len, int64_t llval, int str_is_sds, int after_convert, int *target_enc);
    /* return 0 if the member doesn't exist, 1 if removed */
    int (*rawRemove)(robj *set, char *str, size_t len, int64_t llval, int str_is_sds);
    /* return 1 if the member exists, 0 if not */
    int (*isMember)(robj *set, char *str, size_t len, int64_t llval, int str_is_sds);

    void (*iterInit)(setTypeIterator *si);
    void (*iterReset)(setTypeIterator *si);
    /* return 0 if there is a next element, -1 if not */
    int (*iterNext)(setTypeIterator *si, char **str, size_t *len, int64_t *llele);

    void (*randomElement)(robj *set, char **str, size_t *len, int64_t *llele);

    unsigned long (*size)(const robj *set);
    size_t (*allocSize)(const robj *set);

    robj *(*dup)(robj *set);
    void (*free)(robj *set);

    /* Builds a fresh instance of this encoding from the contents of an
     * existing set, to be assigned to the converted object's ptr by the
     * caller (see setTypeConvertAndExpand() in t_set.c). Iterates 'set'
     * internally (initializing and resetting its own iterator). 'cap' is
     * the number of elements to presize for; encodings that need a
     * different unit internally (e.g. the listpack's byte size estimate)
     * convert it themselves. Returns NULL if 'panic' is false and
     * allocation failed; the encodings that always panic on OOM (i.e.
     * everything but the hash table) ignore 'panic'. NULL for encodings
     * that setTypeConvertAndExpand() never targets (currently intset - see
     * maybeConvertToIntset() in t_set.c instead). */
    void *(*convertFrom)(robj *set, unsigned long cap, int panic);
} setTypeOps;

/* One instance per encoding, defined in the matching t_set_<encoding>.c file. */
extern const setTypeOps setTypeOpsIntset;
extern const setTypeOps setTypeOpsListpack;
extern const setTypeOps setTypeOpsHT;

/* Shrinks a listpack-encoded set's backing listpack down to its actual
 * content size. Exposed (rather than folded into rawAdd) for the one caller
 * in t_set.c that just converted an intset into a listpack using a
 * capacity estimate that can overshoot the listpack's real size. */
void setTypeListpackShrinkToFit(robj *set);

#endif
