/* Flax -- A flat sorted-array map for uint8_t keys.
 *
 * Copyright (c) 2025-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

/* Allocator selection.
 *
 * This file is used in order to change the Flax allocator at compile time.
 * Just define the following defines to what you want to use. Also add
 * the include of your alternate allocator if needed (not needed in order
 * to use the default libc allocator). */

#ifndef FLAX_ALLOC_H
#define FLAX_ALLOC_H
#include "zmalloc.h"
#define flax_malloc zmalloc
#define flax_malloc_usable zmalloc_usable
#define flax_free zfree
#define flax_free_usable zfree_usable
#endif
