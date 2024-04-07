/* Rax -- A radix tree implementation.
 *
 * Copyright (c) 2017-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of the Redis Source Available License 2.0
 * (RSALv2) or the Server Side Public License v1 (SSPLv1).
 */

/* Allocator selection.
 *
 * This file is used in order to change the Rax allocator at compile time.
 * Just define the following defines to what you want to use. Also add
 * the include of your alternate allocator if needed (not needed in order
 * to use the default libc allocator). */

#ifndef RAX_ALLOC_H
#define RAX_ALLOC_H
#include "zmalloc.h"
#define rax_malloc zmalloc
#define rax_realloc zrealloc
#define rax_free zfree
#endif
