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
 * resolveEncodingForAdd is called before inserting a new member. It decides
 * which encoding the member must end up under: the object's current
 * encoding if it fits as-is, or a bigger one (an OBJ_ENCODING_* value) if
 * the object needs to be converted first. It never mutates the object -
 * the caller is responsible for calling setTypeConvertAndExpand() when the
 * returned encoding differs from the current one, then dispatching rawAdd()
 * to the (possibly new) encoding's ops. This keeps the "does this operation
 * require a conversion" decision and the actual conversion in the caller
 * (t_set.c), rather than having each encoding's rawAdd call back into
 * shared/core code. */
typedef struct {
    int (*resolveEncodingForAdd)(robj *set, char *str, size_t len, int64_t llval, int str_is_sds);
    int (*rawAdd)(robj *set, char *str, size_t len, int64_t llval, int str_is_sds);
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

#endif
