#ifndef GTID_ZMALLOC_H
#define GTID_ZMALLOC_H
#include <zmalloc.h>
#define GTID_ALLOC_LIB ZMALLOC_LIB
#define gtid_malloc zmalloc
#define gtid_realloc zrealloc
#define gtid_free zfree
#endif

