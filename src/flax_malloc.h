#ifndef FLAX_ALLOC_H
#define FLAX_ALLOC_H
#include "zmalloc.h"
#define flax_malloc zmalloc
#define flax_malloc_usable zmalloc_usable
#define flax_free zfree
#define flax_free_usable zfree_usable
#endif
