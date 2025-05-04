/* Listpack -- A lists of strings serialization format
 * https://github.com/antirez/listpack
 *
 * Copyright (c) 2017-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

/* Allocator selection.
 *
 * This file is used in order to change the Rax allocator at compile time.
 * Just define the following defines to what you want to use. Also add
 * the include of your alternate allocator if needed (not needed in order
 * to use the default libc allocator). */

#ifndef LISTPACK_ALLOC_H
#define LISTPACK_ALLOC_H
#include "zmalloc.h"
/* We use zmalloc_usable/zrealloc_usable instead of zmalloc/zrealloc
 * to ensure the safe invocation of 'zmalloc_usable_size().
 * See comment in zmalloc_usable_size(). */
#define lp_malloc(sz) zmalloc_usable(sz,NULL)
#define lp_realloc(ptr,sz) zrealloc_usable(ptr,sz,NULL)
#define lp_free zfree
#define lp_malloc_size zmalloc_usable_size
#endif
