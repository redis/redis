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
 *
 * resolveEncodingForAdd() is only called after rawAdd() returns -1. It
 * decides which bigger encoding (an OBJ_ENCODING_* value) the object must
 * be converted to before rawAdd() can be retried. It never mutates the
 * object - the caller (t_set.c) is responsible for calling
 * setTypeConvertAndExpand() and then re-dispatching rawAdd() to the new
 * encoding's ops. This keeps the "does this operation require a
 * conversion, and to what" decision and the actual conversion in the
 * caller, rather than having each encoding's rawAdd call back into
 * shared/core code. */
typedef struct {
    int (*rawAdd)(robj *set, char *str, size_t len, int64_t llval, int str_is_sds);
    int (*resolveEncodingForAdd)(robj *set, char *str, size_t len, int64_t llval, int str_is_sds);
    int (*rawRemove)(robj *set, char *str, size_t len, int64_t llval, int str_is_sds);
    int (*isMember)(robj *set, char *str, size_t len, int64_t llval, int str_is_sds);

    void (*iterInit)(setTypeIterator *si);
    void (*iterReset)(setTypeIterator *si);
    int (*iterNext)(setTypeIterator *si, char **str, size_t *len, int64_t *llele);

    void (*randomElement)(robj *set, char **str, size_t *len, int64_t *llele);

    unsigned long (*size)(const robj *set);
    size_t (*allocSize)(const robj *set);

    robj *(*dup)(robj *set);
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
