/* SOBJ -- autonomous shared-object reference handling.
 *
 * Copyright (c) 2017-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#ifndef SOBJ_H
#define SOBJ_H

/* A redisObject (robj) is considered a shared-object (sobj), if it's associated
with an intern pool at creation - usually by calling `sobj_new`.
CURRENTLY ASSUMES OBJECT IS *RedisModuleString* */
typedef struct redisObject sobj;
struct dict;

struct dict *sobj_init(void);
void sobj_release(struct dict *pool);
sobj *sobj_new(const char *init, size_t initlen, struct dict *pool);
void sobj_free_raw(sobj *o);
void sobj_free(sobj *o, struct dict *pool);
sobj *sobj_defrag(sobj *o, struct dict *pool);
sobj *sobj_find(sobj *o, struct dict *pool);

#endif /* SOBJ_H */
