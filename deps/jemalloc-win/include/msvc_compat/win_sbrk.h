
#ifdef _MSC_VER

#if	(defined(_MSC_VER) && (_MSC_VER >= 1600)) || !defined(_MSC_VER)
#include <stdint.h>
#else
#include "msvc_compat/stdint.h"
#endif	// _MSC_VER

/******************************************************************************/
#ifdef JEMALLOC_H_TYPES

#define CEIL(size, to)  (((size) + (to) - 1) & ~((to)-1))
#define FLOOR(size, to) ((size) & ~((to)-1))

#define SBRK_SCALE      0
#define SBRK_FAILURE    NULL

#define MMAP_FAILURE    ((void *)(-1))
#define MUNMAP_FAILURE  ((int)(-1))

#endif /* JEMALLOC_H_TYPES */
/******************************************************************************/
#ifdef JEMALLOC_H_STRUCTS

/* A region list entry */
typedef struct _region_list_entry {
    void *top_allocated;
    void *top_committed;
    void *top_reserved;
    intptr_t reserve_size;
    struct _region_list_entry *previous;
} region_list_entry;

#endif /* JEMALLOC_H_STRUCTS */
/******************************************************************************/
#ifdef JEMALLOC_H_EXTERNS

//#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize critical section */
void csinitialize(CRITICAL_SECTION *cs);

/* Delete critical section */
void csdelete(CRITICAL_SECTION *cs);

/* Enter critical section */
int  csenter(CRITICAL_SECTION *cs);

/* Leave critical section */
int  csleave(CRITICAL_SECTION *cs);

/* Wait for spin lock */
int  slwait(int *sl);

/* Release spin lock */
int  slrelease(int *sl);

/* getpagesize for windows */
long getpagesize(void);

/* getregionsize for windows */
long getregionsize(void);

/* mmap for windows */
void *mmap(void *ptr, long size, long prot, long type, long handle, long arg);

/* munmap for windows */
long munmap(void *ptr, intptr_t size);

#define USE_SIMPLE_WIN_SBRK     0

#if defined(USE_SIMPLE_WIN_SBRK) && (USE_SIMPLE_WIN_SBRK != 0)
#define sbrk    sbrk_simple
#else
#define sbrk    sbrk_win
#endif  /* USE_SIMPLE_WIN_SBRK */

#ifndef JEMALLOC_HAVE_SBRK

void *sbrk(intptr_t increment);

#else  /* !JEMALLOC_HAVE_SBRK */

/* sbrk version for windows */
void *sbrk_win(intptr_t size);

/* sbrk for windows secondary version */
void *sbrk_simple(intptr_t size);

#endif  /* JEMALLOC_HAVE_SBRK */

void vminfo(size_t *free, size_t *reserved, size_t *committed);

int cpuinfo(int whole, unsigned long *kernel, unsigned long *user);

#ifdef __cplusplus
}
#endif

#endif /* JEMALLOC_H_EXTERNS */
/******************************************************************************/
#ifdef JEMALLOC_H_INLINES

//#include <windows.h>

/* Allocate and link a region entry in the region list */
static int region_list_append (region_list_entry **last, void *base_reserved, intptr_t reserve_size) {
    region_list_entry *next = (region_list_entry *)HeapAlloc(GetProcessHeap(), 0, sizeof(region_list_entry));
    if (! next)
        return FALSE;
    next->top_allocated = (char *) base_reserved;
    next->top_committed = (char *) base_reserved;
    next->top_reserved  = (char *) base_reserved + reserve_size;
    next->reserve_size = reserve_size;
    next->previous = *last;
    *last = next;
    return TRUE;
}

/* Free and unlink the last region entry from the region list */
static int region_list_remove (region_list_entry **last) {
    region_list_entry *previous = (*last)->previous;
    if (! HeapFree(GetProcessHeap(), sizeof(region_list_entry), *last))
        return FALSE;
    *last = previous;
    return TRUE;
}

#endif /* JEMALLOC_H_INLINES */
/******************************************************************************/

#endif  /* _MSC_VER */
