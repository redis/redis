/* The following is the NetBSD libc qsort implementation modified in order to
 * support partial sorting of ranges for Redis.
 *
 * Copyright(C) 2009 Salvatore Sanfilippo. All rights reserved.
 *
 * See the pqsort.c file for the original copyright notice. */

#ifndef __PQSORT_H
#define __PQSORT_H

void
pqsort(void *a, size_t n, size_t es,
    int (*cmp) __P((const void *, const void *)), size_t lrange, size_t rrange);

#endif
