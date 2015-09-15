
#define JEMALLOC_WIN_SBRK_C_

#include "jemalloc/internal/jemalloc_internal.h"

#ifdef _MSC_VER

//#include <windows.h>

/**************************************************************************
 *
 * Article: Emulating UNIX Memory Management Under Microsoft Windows
 * by Joerg Walter
 *
 * Reference: http://www.genesys-e.org/jwalter//mix4win.htm
 *
 **************************************************************************/

static int g_cs_initialized = 0;
static CRITICAL_SECTION g_cs;
static int g_sl = 0;

static long g_pagesize = 0;
static long g_regionsize = 0;

/* Initialize critical section */
void csinitialize(CRITICAL_SECTION *cs)
{
    if (cs != NULL)
        InitializeCriticalSectionAndSpinCount(cs, 0x80000400UL);
}

/* Delete critical section */
void csdelete(CRITICAL_SECTION *cs)
{
    if (cs != NULL)
        DeleteCriticalSection(cs);
}

/* Enter critical section */
int csenter(CRITICAL_SECTION *cs)
{
    EnterCriticalSection(cs);
    return 0;
}

/* Leave critical section */
int csleave(CRITICAL_SECTION *cs)
{
    LeaveCriticalSection(cs);
    return 0;
}

/* Wait for spin lock */
int slwait(int *sl)
{
    while (InterlockedCompareExchange((volatile long *)sl, (long)1, (long)0) != 0) {
        Sleep(0);
    }
    return 0;
}

/* Release spin lock */
int slrelease(int *sl)
{
    InterlockedExchange((volatile long *)sl, 0);
    return 0;
}

/* Utility functions */

/* getpagesize for windows */
long getpagesize(void)
{
    if (! g_pagesize) {
        SYSTEM_INFO system_info;
        GetSystemInfo(&system_info);
        g_pagesize = system_info.dwPageSize;
    }
    return g_pagesize;
}

/* getregionsize for windows */
long getregionsize(void)
{
    if (! g_regionsize) {
        SYSTEM_INFO system_info;
        GetSystemInfo(&system_info);
        g_regionsize = system_info.dwAllocationGranularity;
    }
    return g_regionsize;
}

/* Emulation of mmap/munmap */

/* UNIX mmap/munmap may be emulated straightforward on Windows using VirtualAlloc/VirtualFree. */

/* mmap for windows */
void *mmap(void *ptr, long size, long prot, long type, long handle, long arg)
{
    /* Wait for spin lock */
    slwait(&g_sl);
    /* First time initialization */
    if (! g_pagesize)
        g_pagesize = getpagesize ();
    if (! g_regionsize)
        g_regionsize = getregionsize ();
    /* Allocate this */
    ptr = VirtualAlloc(ptr, size,
        MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE);
    if (! ptr) {
        ptr = MMAP_FAILURE;
        goto mmap_exit;
    }
mmap_exit:
    /* Release spin lock */
    slrelease(&g_sl);
    return ptr;
}

/* munmap for windows */
long munmap(void *ptr, intptr_t size)
{
    int rc = MUNMAP_FAILURE;
    /* Wait for spin lock */
    slwait(&g_sl);
    /* First time initialization */
    if (! g_pagesize)
        g_pagesize = getpagesize ();
    if (! g_regionsize)
        g_regionsize = getregionsize ();
    /* Free this */
    if (! VirtualFree(ptr, 0, MEM_RELEASE))
        goto munmap_exit;
    rc = 0;
munmap_exit:
    /* Release spin lock */
    slrelease(&g_sl);
    return rc;
}

/* Emulation of sbrk */

/* UNIX sbrk has two main tasks: increasing and decreasing a best try contiguous memory arena for malloc/free. */

/* sbrk for windows */
void *sbrk_win(intptr_t size)
{
    static long g_my_pagesize;
    static long g_my_regionsize;
    static region_list_entry *g_last;
    void *result = SBRK_FAILURE;
    /* Wait for spin lock */
    slwait(&g_sl);
    /* First time initialization */
    if (! g_pagesize) {
        g_pagesize = getpagesize();
        g_my_pagesize = g_pagesize << SBRK_SCALE;
    }
    if (! g_regionsize) {
        g_regionsize = getregionsize();
        g_my_regionsize = g_regionsize << SBRK_SCALE;
    }
    if (! g_last) {
        if (! region_list_append(&g_last, 0, 0)) {
            goto sbrk_exit;
        }
    }
    /* Allocation requested? */
    if (size >= 0) {
        /* Allocation size is the requested size */
        intptr_t allocate_size = size;
        /* Compute the size to commit */
        intptr_t to_commit = (char *)g_last->top_allocated + allocate_size - (char *)g_last->top_committed;
        /* Do we reach the commit limit? */
        if (to_commit > 0) {
            /* Round size to commit */
            long commit_size = CEIL(to_commit, g_my_pagesize);
            /* Compute the size to reserve */
            intptr_t to_reserve = (char *)g_last->top_committed + commit_size - (char *)g_last->top_reserved;
            /* Do we reach the reserve limit? */
            if (to_reserve > 0) {
                /* Compute the remaining size to commit in the current region */
                intptr_t remaining_commit_size = (char *)g_last->top_reserved - (char *)g_last->top_committed;
                if (remaining_commit_size > 0) {
                    /* Commit this */
                    void *base_committed = VirtualAlloc(g_last->top_committed, remaining_commit_size,
                                                        MEM_COMMIT, PAGE_READWRITE);
                    /* Check returned pointer for consistency */
                    if (base_committed != g_last->top_committed) {
                        goto sbrk_exit;
                    }
                    /* Adjust the regions commit top */
                    g_last->top_committed = (char *)base_committed + remaining_commit_size;
                } {
                    /* Now we are going to search and reserve. */
                    int contiguous = -1;
                    int found = FALSE;
                    MEMORY_BASIC_INFORMATION memory_info;
                    void *base_reserved;
                    long reserve_size;
                    do {
                        /* Assume contiguous memory */
                        contiguous = TRUE;
                        /* Round size to reserve */
                        reserve_size = CEIL(to_reserve, g_my_regionsize);
                        /* Start with the current region's top */
                        memory_info.BaseAddress = g_last->top_reserved;
                        while (VirtualQuery(memory_info.BaseAddress, &memory_info, sizeof(memory_info))) {
                            /* Region is free, well aligned and big enough: we are done */
                            if (memory_info.State == MEM_FREE &&
                                (unsigned) memory_info.BaseAddress % g_regionsize == 0 &&
                                memory_info.RegionSize >= (unsigned) reserve_size) {
                                found = TRUE;
                                break;
                            }
                            /* From now on we can't get contiguous memory! */
                            contiguous = FALSE;
                            /* Recompute size to reserve */
                            reserve_size = CEIL(allocate_size, g_my_regionsize);
                            memory_info.BaseAddress = (char *)memory_info.BaseAddress + memory_info.RegionSize;
                        }
                        /* Search failed? */
                        if (! found) {
                            goto sbrk_exit;
                        }
                        /* Try to reserve this */
                        base_reserved = VirtualAlloc(memory_info.BaseAddress, reserve_size,
                                                     MEM_RESERVE, PAGE_NOACCESS);
                        if (! base_reserved) {
                            int rc = GetLastError();
                            if (rc != ERROR_INVALID_ADDRESS) {
                                goto sbrk_exit;
                            }
                        }
                        /* A null pointer signals (hopefully) a race condition with another thread. */
                        /* In this case, we try again. */
                    }
                    while (! base_reserved);
                    /* Check returned pointer for consistency */
                    if (memory_info.BaseAddress && base_reserved != memory_info.BaseAddress) {
                        goto sbrk_exit;
                    }
                    /* Did we get contiguous memory? */
                    if (contiguous) {
                        intptr_t start_size = (char *)g_last->top_committed - (char *)g_last->top_allocated;
                        /* Adjust allocation size */
                        allocate_size -= start_size;
                        /* Adjust the regions allocation top */
                        g_last->top_allocated = g_last->top_committed;
                        /* Recompute the size to commit */
                        to_commit = (char *)g_last->top_allocated + allocate_size - (char *)g_last->top_committed;
                        /* Round size to commit */
                        commit_size = CEIL(to_commit, g_my_pagesize);
                    }
                    /* Append the new region to the list */
                    if (! region_list_append(&g_last, base_reserved, reserve_size)) {
                        goto sbrk_exit;
                    }
                    /* Didn't we get contiguous memory? */
                    if (! contiguous) {
                        /* Recompute the size to commit */
                        to_commit = (char *)g_last->top_allocated + allocate_size - (char *)g_last->top_committed;
                        /* Round size to commit */
                        commit_size = CEIL(to_commit, g_my_pagesize);
                    }
                }
            } {
                /* Commit this */
                void *base_committed = VirtualAlloc(g_last->top_committed, commit_size,
                                                    MEM_COMMIT, PAGE_READWRITE);
                /* Check returned pointer for consistency */
                if (base_committed != g_last->top_committed) {
                    goto sbrk_exit;
                }
                /* Adjust the regions commit top */
                g_last->top_committed = (char *)base_committed + commit_size;
            }
        }
        /* Adjust the regions allocation top */
        g_last->top_allocated = (char *)g_last->top_allocated + allocate_size;
        result = (char *)g_last->top_allocated - size;
        /* Deallocation requested? */
    }
    else {
        if (size < 0) {
            intptr_t deallocate_size = -size;
            /* As long as we have a region to release */
            while ((char *)g_last->top_allocated - deallocate_size < (char *)g_last->top_reserved - g_last->reserve_size) {
                /* Get the size to release */
                intptr_t release_size = g_last->reserve_size;
                /* Get the base address */
                void *base_reserved = (char *)g_last->top_reserved - release_size;
                /* Release this */
                int rc = VirtualFree(base_reserved, 0, MEM_RELEASE);
                /* Check returned code for consistency */
                if (! rc) {
                    goto sbrk_exit;
                }
                /* Adjust deallocation size */
                deallocate_size -= (char *)g_last->top_allocated - (char *)base_reserved;
                /* Remove the old region from the list */
                if (! region_list_remove(&g_last)) {
                    goto sbrk_exit;
                }
            } {
                /* Compute the size to decommit */
                intptr_t to_decommit = (char *)g_last->top_committed - ((char *)g_last->top_allocated - deallocate_size);
                if (to_decommit >= g_my_pagesize) {
                    /* Compute the size to decommit */
                    long decommit_size = FLOOR(to_decommit, g_my_pagesize);
                    /*  Compute the base address */
                    void *base_committed = (char *)g_last->top_committed - decommit_size;
                    /* Decommit this */
                    int rc = VirtualFree((char *)base_committed, decommit_size,
                                         MEM_DECOMMIT);
                    /* Check returned code for consistency */
                    if (! rc) {
                        goto sbrk_exit;
                    }
                    /* Adjust deallocation size and regions commit and allocate top */
                    deallocate_size -= (char *)g_last->top_allocated - (char *)base_committed;
                    g_last->top_committed = base_committed;
                    g_last->top_allocated = base_committed;
                }
            }
            /* Adjust regions allocate top */
            g_last->top_allocated = (char *)g_last->top_allocated - deallocate_size;
            /* Check for underflow */
            if ((char *)g_last->top_reserved - g_last->reserve_size > (char *)g_last->top_allocated ||
                g_last->top_allocated > g_last->top_committed) {
                /* Adjust regions allocate top */
                g_last->top_allocated = (char *)g_last->top_reserved - g_last->reserve_size;
                goto sbrk_exit;
            }
            result = g_last->top_allocated;
        }
    }
sbrk_exit:
    /* Release spin lock */
    slrelease(&g_sl);
    return result;
}

/* sbrk for windows secondary version */
void *sbrk_simple(intptr_t size)
{
    static long g_my_pagesize;
    static long g_my_regionsize;
    static region_list_entry *g_last;
    void *result = SBRK_FAILURE;
    /* Wait for spin lock */
    slwait(&g_sl);
    /* First time initialization */
    if (! g_pagesize) {
        g_pagesize = getpagesize();
        g_my_pagesize = g_pagesize << SBRK_SCALE;
    }
    if (! g_regionsize) {
        g_regionsize = getregionsize();
        g_my_regionsize = g_regionsize << SBRK_SCALE;
    }
    if (! g_last) {
        if (! region_list_append(&g_last, 0, 0)) {
            goto sbrk_exit;
        }
    }
    /* Allocation requested? */
    if (size >= 0) {
        /* Allocation size is the requested size */
        intptr_t allocate_size = size;
        /* Compute the size to commit */
        intptr_t to_reserve = (char *)g_last->top_allocated + allocate_size - (char *)g_last->top_reserved;
        /* Do we reach the commit limit? */
        if (to_reserve > 0) {
            /* Now we are going to search and reserve. */
            int contiguous = -1;
            int found = FALSE;
            MEMORY_BASIC_INFORMATION memory_info;
            void *base_reserved;
            intptr_t reserve_size;
            do {
                /* Assume contiguous memory */
                contiguous = TRUE;
                /* Round size to reserve */
                reserve_size = CEIL(to_reserve, g_my_regionsize);
                /* Start with the current region's top */
                memory_info.BaseAddress = g_last->top_reserved;
                while (VirtualQuery(memory_info.BaseAddress, &memory_info, sizeof(memory_info))) {
                    /* Region is free, well aligned and big enough: we are done */
                    if (memory_info.State == MEM_FREE &&
                        (unsigned) memory_info.BaseAddress % g_regionsize == 0 &&
                        memory_info.RegionSize >= (unsigned)reserve_size) {
                        found = TRUE;
                        break;
                    }
                    /* From now on we can't get contiguous memory! */
                    contiguous = FALSE;
                    /* Recompute size to reserve */
                    reserve_size = CEIL(allocate_size, g_my_regionsize);
                    memory_info.BaseAddress = (char *)memory_info.BaseAddress + memory_info.RegionSize;
                }
                /* Search failed? */
                if (! found) {
                    goto sbrk_exit;
                }
                /* Try to reserve this */
                base_reserved = VirtualAlloc(memory_info.BaseAddress, reserve_size,
                                             MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
                if (! base_reserved) {
                    int rc = GetLastError();
                    if (rc != ERROR_INVALID_ADDRESS) {
                        goto sbrk_exit;
                    }
                }
                /* A null pointer signals (hopefully) a race condition with another thread. */
                /* In this case, we try again. */
            }
            while (! base_reserved);
            /* Check returned pointer for consistency */
            if (memory_info.BaseAddress && base_reserved != memory_info.BaseAddress) {
                goto sbrk_exit;
            }
            /* Did we get contiguous memory? */
            if (contiguous) {
                intptr_t start_size = (char *)g_last->top_reserved - (char *)g_last->top_allocated;
                /* Adjust allocation size */
                allocate_size -= start_size;
                /* Adjust the regions allocation top */
                g_last->top_allocated = g_last->top_reserved;
            }
            /* Append the new region to the list */
            if (! region_list_append(&g_last, base_reserved, reserve_size)) {
                goto sbrk_exit;
            }
        }
        /* Adjust the regions allocation top */
        g_last->top_allocated = (char *)g_last->top_allocated + allocate_size;
        result = (char *)g_last->top_allocated - size;
        /* Deallocation requested? */
    }
    else {
        if (size < 0) {
            intptr_t deallocate_size = - size;
            /* As long as we have a region to release */
            while ((char *)g_last->top_allocated - deallocate_size < (char *)g_last->top_reserved - g_last->reserve_size) {
                /* Get the size to release */
                intptr_t release_size = g_last->reserve_size;
                /* Get the base address */
                void *base_reserved = (char *)g_last->top_reserved - release_size;
                /* Release this */
                int rc = VirtualFree(base_reserved, 0,
                                     MEM_RELEASE);
                /* Check returned code for consistency */
                if (! rc) {
                    goto sbrk_exit;
                }
                /* Adjust deallocation size */
                deallocate_size -= (char *)g_last->top_allocated - (char *)base_reserved;
                /* Remove the old region from the list */
                if (! region_list_remove(&g_last)) {
                    goto sbrk_exit;
                }
            }
            /* Adjust regions allocate top */
            g_last->top_allocated = (char *)g_last->top_allocated - deallocate_size;
            /* Check for underflow */
            if ((char *)g_last->top_reserved - g_last->reserve_size > (char *)g_last->top_allocated ||
                g_last->top_allocated > g_last->top_reserved) {
                /* Adjust regions allocate top */
                g_last->top_allocated = (char *)g_last->top_reserved - g_last->reserve_size;
                goto sbrk_exit;
            }
            result = g_last->top_allocated;
        }
    }
sbrk_exit:
    /* Release spin lock */
    slrelease(&g_sl);
    return result;
}

/* Gathering statistics */

/* The following helpers may aid you in gathering memory or CPU statistics on Windows. */

void vminfo(size_t *free, size_t *reserved, size_t *committed)
{
    MEMORY_BASIC_INFORMATION memory_info;
    memory_info.BaseAddress = 0;
    *free = *reserved = *committed = 0;
    while (VirtualQuery(memory_info.BaseAddress, &memory_info, sizeof (memory_info))) {
        switch (memory_info.State) {
        case MEM_FREE:
            *free += memory_info.RegionSize;
            break;
        case MEM_RESERVE:
            *reserved += memory_info.RegionSize;
            break;
        case MEM_COMMIT:
            *committed += memory_info.RegionSize;
            break;
        }
        memory_info.BaseAddress = (char *)memory_info.BaseAddress + memory_info.RegionSize;
    }
}

int cpuinfo(int whole, unsigned long *kernel, unsigned long *user)
{
    if (whole) {
        __int64 creation64, exit64, kernel64, user64;
        int rc = GetProcessTimes(GetCurrentProcess(),
                                  (FILETIME *) &creation64,
                                  (FILETIME *) &exit64,
                                  (FILETIME *) &kernel64,
                                  (FILETIME *) &user64);
        if (! rc) {
            *kernel = 0;
            *user = 0;
            return FALSE;
        }
        *kernel = (unsigned long)(kernel64 / 10000);
        *user = (unsigned long)(user64 / 10000);
        return TRUE;
    }
    else {
        __int64 creation64, exit64, kernel64, user64;
        int rc = GetThreadTimes(GetCurrentThread(),
                                 (FILETIME *) &creation64,
                                 (FILETIME *) &exit64,
                                 (FILETIME *) &kernel64,
                                 (FILETIME *) &user64);
        if (! rc) {
            *kernel = 0;
            *user = 0;
            return FALSE;
        }
        *kernel = (unsigned long)(kernel64 / 10000);
        *user = (unsigned long)(user64 / 10000);
        return TRUE;
    }
}

#endif  /* _MSC_VER */
