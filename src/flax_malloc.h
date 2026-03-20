#ifndef FLAX_ALLOC_H
#define FLAX_ALLOC_H
#include "zmalloc.h"
#define flax_malloc zmalloc
#define flax_free zfree
#endif
