/* zmalloc - total amount of allocated memory aware version of malloc()
 *
 * Copyright (c) 2009-2010, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "fmacros.h"
#include "config.h"
#include "solarisfixes.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <assert.h>

#ifdef __linux__
#include <sys/mman.h>
#endif

/* This function provide us access to the original libc free(). This is useful
 * for instance to free results obtained by backtrace_symbols(). We need
 * to define this function before including zmalloc.h that may shadow the
 * free implementation if we use jemalloc or another non standard allocator. */
void zlibc_free(void *ptr) {
    free(ptr);
}

#include <string.h>
#include <pthread.h>
#include "zmalloc.h"
#include "atomicvar.h"

#define UNUSED(x) ((void)(x))

#ifdef HAVE_MALLOC_SIZE
#define PREFIX_SIZE (0)
#define ASSERT_NO_SIZE_OVERFLOW(sz)
#else
#if defined(__sun) || defined(__sparc) || defined(__sparc__)
#define PREFIX_SIZE (sizeof(long long))
#else
#define PREFIX_SIZE (sizeof(size_t))
#endif
#define ASSERT_NO_SIZE_OVERFLOW(sz) assert((sz) + PREFIX_SIZE > (sz))
#endif

/* When using the libc allocator, use a minimum allocation size to match the
 * jemalloc behavior that doesn't return NULL in this case.
 */
#define MALLOC_MIN_SIZE(x) ((x) > 0 ? (x) : sizeof(long))

/* Explicitly override malloc/free etc when using tcmalloc. */
#if defined(USE_TCMALLOC)
#define malloc(size) tc_malloc(size)
#define calloc(count,size) tc_calloc(count,size)
#define realloc(ptr,size) tc_realloc(ptr,size)
#define free(ptr) tc_free(ptr)
#elif defined(USE_JEMALLOC)
#define malloc(size) je_malloc(size)
#define calloc(count,size) je_calloc(count,size)
#define realloc(ptr,size) je_realloc(ptr,size)
#define free(ptr) je_free(ptr)
#define mallocx(size,flags) je_mallocx(size,flags)
#define dallocx(ptr,flags) je_dallocx(ptr,flags)
#endif

#define update_zmalloc_stat_alloc(__n) atomicIncr(used_memory,(__n))
#define update_zmalloc_stat_free(__n) atomicDecr(used_memory,(__n))

static redisAtomic size_t used_memory = 0;

static void zmalloc_default_oom(size_t size) {
    fprintf(stderr, "zmalloc: Out of memory trying to allocate %zu bytes\n",
        size);
    fflush(stderr);
    abort();
}

static void (*zmalloc_oom_handler)(size_t) = zmalloc_default_oom;

/* Try allocating memory, and return NULL if failed.
 * '*usable' is set to the usable size if non NULL. */
void *ztrymalloc_usable(size_t size, size_t *usable) {
    ASSERT_NO_SIZE_OVERFLOW(size);
    void *ptr = malloc(MALLOC_MIN_SIZE(size)+PREFIX_SIZE);

    if (!ptr) return NULL;
#ifdef HAVE_MALLOC_SIZE
    size = zmalloc_size(ptr);
    update_zmalloc_stat_alloc(size);
    if (usable) *usable = size;
    return ptr;
#else
    *((size_t*)ptr) = size;
    update_zmalloc_stat_alloc(size+PREFIX_SIZE);
    if (usable) *usable = size;
    return (char*)ptr+PREFIX_SIZE;
#endif
}

/* Allocate memory or panic */
void *zmalloc(size_t size) {
    void *ptr = ztrymalloc_usable(size, NULL);
    if (!ptr) zmalloc_oom_handler(size);
    return ptr;
}

/* Try allocating memory, and return NULL if failed. */
void *ztrymalloc(size_t size) {
    void *ptr = ztrymalloc_usable(size, NULL);
    return ptr;
}

/* Allocate memory or panic.
 * '*usable' is set to the usable size if non NULL. */
void *zmalloc_usable(size_t size, size_t *usable) {
    void *ptr = ztrymalloc_usable(size, usable);
    if (!ptr) zmalloc_oom_handler(size);
    return ptr;
}

/* Allocation and free functions that bypass the thread cache
 * and go straight to the allocator arena bins.
 * Currently implemented only for jemalloc. Used for online defragmentation. */
#ifdef HAVE_DEFRAG
void *zmalloc_no_tcache(size_t size) {
    ASSERT_NO_SIZE_OVERFLOW(size);
    void *ptr = mallocx(size+PREFIX_SIZE, MALLOCX_TCACHE_NONE);
    if (!ptr) zmalloc_oom_handler(size);
    update_zmalloc_stat_alloc(zmalloc_size(ptr));
    return ptr;
}

void zfree_no_tcache(void *ptr) {
    if (ptr == NULL) return;
    update_zmalloc_stat_free(zmalloc_size(ptr));
    dallocx(ptr, MALLOCX_TCACHE_NONE);
}
#endif

/* Try allocating memory and zero it, and return NULL if failed.
 * '*usable' is set to the usable size if non NULL. */
void *ztrycalloc_usable(size_t size, size_t *usable) {
    ASSERT_NO_SIZE_OVERFLOW(size);
    void *ptr = calloc(1, MALLOC_MIN_SIZE(size)+PREFIX_SIZE);
    if (ptr == NULL) return NULL;

#ifdef HAVE_MALLOC_SIZE
    size = zmalloc_size(ptr);
    update_zmalloc_stat_alloc(size);
    if (usable) *usable = size;
    return ptr;
#else
    *((size_t*)ptr) = size;
    update_zmalloc_stat_alloc(size+PREFIX_SIZE);
    if (usable) *usable = size;
    return (char*)ptr+PREFIX_SIZE;
#endif
}

/* Allocate memory and zero it or panic.
 * We need this wrapper to have a calloc compatible signature */
void *zcalloc_num(size_t num, size_t size) {
    /* Ensure that the arguments to calloc(), when multiplied, do not wrap.
     * Division operations are susceptible to divide-by-zero errors so we also check it. */
    if ((size == 0) || (num > SIZE_MAX/size)) {
        zmalloc_oom_handler(SIZE_MAX);
        return NULL;
    }
    void *ptr = ztrycalloc_usable(num*size, NULL);
    if (!ptr) zmalloc_oom_handler(num*size);
    return ptr;
}

/* Allocate memory and zero it or panic */
void *zcalloc(size_t size) {
    void *ptr = ztrycalloc_usable(size, NULL);
    if (!ptr) zmalloc_oom_handler(size);
    return ptr;
}

/* Try allocating memory, and return NULL if failed. */
void *ztrycalloc(size_t size) {
    void *ptr = ztrycalloc_usable(size, NULL);
    return ptr;
}

/* Allocate memory or panic.
 * '*usable' is set to the usable size if non NULL. */
void *zcalloc_usable(size_t size, size_t *usable) {
    void *ptr = ztrycalloc_usable(size, usable);
    if (!ptr) zmalloc_oom_handler(size);
    return ptr;
}

/* Try reallocating memory, and return NULL if failed.
 * '*usable' is set to the usable size if non NULL. */
void *ztryrealloc_usable(void *ptr, size_t size, size_t *usable) {
    ASSERT_NO_SIZE_OVERFLOW(size);
#ifndef HAVE_MALLOC_SIZE
    void *realptr;
#endif
    size_t oldsize;
    void *newptr;

    /* not allocating anything, just redirect to free. */
    if (size == 0 && ptr != NULL) {
        zfree(ptr);
        if (usable) *usable = 0;
        return NULL;
    }
    /* Not freeing anything, just redirect to malloc. */
    if (ptr == NULL)
        return ztrymalloc_usable(size, usable);

#ifdef HAVE_MALLOC_SIZE
    oldsize = zmalloc_size(ptr);
    newptr = realloc(ptr,size);
    if (newptr == NULL) {
        if (usable) *usable = 0;
        return NULL;
    }

    update_zmalloc_stat_free(oldsize);
    size = zmalloc_size(newptr);
    update_zmalloc_stat_alloc(size);
    if (usable) *usable = size;
    return newptr;
#else
    realptr = (char*)ptr-PREFIX_SIZE;
    oldsize = *((size_t*)realptr);
    newptr = realloc(realptr,size+PREFIX_SIZE);
    if (newptr == NULL) {
        if (usable) *usable = 0;
        return NULL;
    }

    *((size_t*)newptr) = size;
    update_zmalloc_stat_free(oldsize);
    update_zmalloc_stat_alloc(size);
    if (usable) *usable = size;
    return (char*)newptr+PREFIX_SIZE;
#endif
}

/* Reallocate memory and zero it or panic */
void *zrealloc(void *ptr, size_t size) {
    ptr = ztryrealloc_usable(ptr, size, NULL);
    if (!ptr && size != 0) zmalloc_oom_handler(size);
    return ptr;
}

/* Try Reallocating memory, and return NULL if failed. */
void *ztryrealloc(void *ptr, size_t size) {
    ptr = ztryrealloc_usable(ptr, size, NULL);
    return ptr;
}

/* Reallocate memory or panic.
 * '*usable' is set to the usable size if non NULL. */
void *zrealloc_usable(void *ptr, size_t size, size_t *usable) {
    ptr = ztryrealloc_usable(ptr, size, usable);
    if (!ptr && size != 0) zmalloc_oom_handler(size);
    return ptr;
}

/* Provide zmalloc_size() for systems where this function is not provided by
 * malloc itself, given that in that case we store a header with this
 * information as the first bytes of every allocation. */
#ifndef HAVE_MALLOC_SIZE
size_t zmalloc_size(void *ptr) {
    void *realptr = (char*)ptr-PREFIX_SIZE;
    size_t size = *((size_t*)realptr);
    return size+PREFIX_SIZE;
}
size_t zmalloc_usable_size(void *ptr) {
    return zmalloc_size(ptr)-PREFIX_SIZE;
}
#endif

void zfree(void *ptr) {
#ifndef HAVE_MALLOC_SIZE
    void *realptr;
    size_t oldsize;
#endif

    if (ptr == NULL) return;
#ifdef HAVE_MALLOC_SIZE
    update_zmalloc_stat_free(zmalloc_size(ptr));
    free(ptr);
#else
    realptr = (char*)ptr-PREFIX_SIZE;
    oldsize = *((size_t*)realptr);
    update_zmalloc_stat_free(oldsize+PREFIX_SIZE);
    free(realptr);
#endif
}

/* Similar to zfree, '*usable' is set to the usable size being freed. */
void zfree_usable(void *ptr, size_t *usable) {
#ifndef HAVE_MALLOC_SIZE
    void *realptr;
    size_t oldsize;
#endif

    if (ptr == NULL) return;
#ifdef HAVE_MALLOC_SIZE
    update_zmalloc_stat_free(*usable = zmalloc_size(ptr));
    free(ptr);
#else
    realptr = (char*)ptr-PREFIX_SIZE;
    *usable = oldsize = *((size_t*)realptr);
    update_zmalloc_stat_free(oldsize+PREFIX_SIZE);
    free(realptr);
#endif
}

char *zstrdup(const char *s) {
    size_t l = strlen(s)+1;
    char *p = zmalloc(l);

    memcpy(p,s,l);
    return p;
}

size_t zmalloc_used_memory(void) {
    size_t um;
    atomicGet(used_memory,um);
    return um;
}

void zmalloc_set_oom_handler(void (*oom_handler)(size_t)) {
    zmalloc_oom_handler = oom_handler;
}

/* Use 'MADV_DONTNEED' to release memory to operating system quickly.
 * We do that in a fork child process to avoid CoW when the parent modifies
 * these shared pages. */
void zmadvise_dontneed(void *ptr) {
#if defined(USE_JEMALLOC) && defined(__linux__)
    static size_t page_size = 0;
    if (page_size == 0) page_size = sysconf(_SC_PAGESIZE);
    size_t page_size_mask = page_size - 1;

    size_t real_size = zmalloc_size(ptr);
    if (real_size < page_size) return;

    /* We need to align the pointer upwards according to page size, because
     * the memory address is increased upwards and we only can free memory
     * based on page. */
    char *aligned_ptr = (char *)(((size_t)ptr+page_size_mask) & ~page_size_mask);
    real_size -= (aligned_ptr-(char*)ptr);
    if (real_size >= page_size) {
        madvise((void *)aligned_ptr, real_size&~page_size_mask, MADV_DONTNEED);
    }
#else
    (void)(ptr);
#endif
}

/* Get the RSS information in an OS-specific way.
 *
 * WARNING: the function zmalloc_get_rss() is not designed to be fast
 * and may not be called in the busy loops where Redis tries to release
 * memory expiring or swapping out objects.
 *
 * For this kind of "fast RSS reporting" usages use instead the
 * function RedisEstimateRSS() that is a much faster (and less precise)
 * version of the function. */

#if defined(HAVE_PROC_STAT)
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#endif

/* Get the i'th field from "/proc/self/stats" note i is 1 based as appears in the 'proc' man page */
int get_proc_stat_ll(int i, long long *res) {
#if defined(HAVE_PROC_STAT)
    char buf[4096];
    int fd, l;
    char *p, *x;

    if ((fd = open("/proc/self/stat",O_RDONLY)) == -1) return 0;
    if ((l = read(fd,buf,sizeof(buf)-1)) <= 0) {
        close(fd);
        return 0;
    }
    close(fd);
    buf[l] = '\0';
    if (buf[l-1] == '\n') buf[l-1] = '\0';

    /* Skip pid and process name (surrounded with parentheses) */
    p = strrchr(buf, ')');
    if (!p) return 0;
    p++;
    while (*p == ' ') p++;
    if (*p == '\0') return 0;
    i -= 3;
    if (i < 0) return 0;

    while (p && i--) {
        p = strchr(p, ' ');
        if (p) p++;
        else return 0;
    }
    x = strchr(p,' ');
    if (x) *x = '\0';

    *res = strtoll(p,&x,10);
    if (*x != '\0') return 0;
    return 1;
#else
    UNUSED(i);
    UNUSED(res);
    return 0;
#endif
}

#if defined(HAVE_PROC_STAT)
size_t zmalloc_get_rss(void) {
    int page = sysconf(_SC_PAGESIZE);
    long long rss;

    /* RSS is the 24th field in /proc/<pid>/stat */
    if (!get_proc_stat_ll(24, &rss)) return 0;
    rss *= page;
    return rss;
}
#elif defined(HAVE_TASKINFO)
#include <sys/types.h>
#include <sys/sysctl.h>
#include <mach/task.h>
#include <mach/mach_init.h>

size_t zmalloc_get_rss(void) {
    task_t task = MACH_PORT_NULL;
    struct task_basic_info t_info;
    mach_msg_type_number_t t_info_count = TASK_BASIC_INFO_COUNT;

    if (task_for_pid(current_task(), getpid(), &task) != KERN_SUCCESS)
        return 0;
    task_info(task, TASK_BASIC_INFO, (task_info_t)&t_info, &t_info_count);

    return t_info.resident_size;
}
#elif defined(__FreeBSD__) || defined(__DragonFly__)
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/user.h>

size_t zmalloc_get_rss(void) {
    struct kinfo_proc info;
    size_t infolen = sizeof(info);
    int mib[4];
    mib[0] = CTL_KERN;
    mib[1] = KERN_PROC;
    mib[2] = KERN_PROC_PID;
    mib[3] = getpid();

    if (sysctl(mib, 4, &info, &infolen, NULL, 0) == 0)
#if defined(__FreeBSD__)
        return (size_t)info.ki_rssize * getpagesize();
#else
        return (size_t)info.kp_vm_rssize * getpagesize();
#endif

    return 0L;
}
#elif defined(__NetBSD__) || defined(__OpenBSD__)
#include <sys/types.h>
#include <sys/sysctl.h>

#if defined(__OpenBSD__)
#define kinfo_proc2 kinfo_proc
#define KERN_PROC2 KERN_PROC
#define __arraycount(a) (sizeof(a) / sizeof(a[0]))
#endif

size_t zmalloc_get_rss(void) {
    struct kinfo_proc2 info;
    size_t infolen = sizeof(info);
    int mib[6];
    mib[0] = CTL_KERN;
    mib[1] = KERN_PROC2;
    mib[2] = KERN_PROC_PID;
    mib[3] = getpid();
    mib[4] = sizeof(info);
    mib[5] = 1;
    if (sysctl(mib, __arraycount(mib), &info, &infolen, NULL, 0) == 0)
        return (size_t)info.p_vm_rssize * getpagesize();

    return 0L;
}
#elif defined(__HAIKU__)
#include <OS.h>

size_t zmalloc_get_rss(void) {
    area_info info;
    thread_info th;
    size_t rss = 0;
    ssize_t cookie = 0;

    if (get_thread_info(find_thread(0), &th) != B_OK)
        return 0;

    while (get_next_area_info(th.team, &cookie, &info) == B_OK)
        rss += info.ram_size;

    return rss;
}
#elif defined(HAVE_PSINFO)
#include <unistd.h>
#include <sys/procfs.h>
#include <fcntl.h>

size_t zmalloc_get_rss(void) {
    struct prpsinfo info;
    char filename[256];
    int fd;

    snprintf(filename,256,"/proc/%ld/psinfo",(long) getpid());

    if ((fd = open(filename,O_RDONLY)) == -1) return 0;
    if (ioctl(fd, PIOCPSINFO, &info) == -1) {
        close(fd);
	return 0;
    }

    close(fd);
    return info.pr_rssize;
}
#else
size_t zmalloc_get_rss(void) {
    /* If we can't get the RSS in an OS-specific way for this system just
     * return the memory usage we estimated in zmalloc()..
     *
     * Fragmentation will appear to be always 1 (no fragmentation)
     * of course... */
    return zmalloc_used_memory();
}
#endif

#if defined(USE_JEMALLOC)

int zmalloc_get_allocator_info(size_t *allocated,
                               size_t *active,
                               size_t *resident) {
    uint64_t epoch = 1;
    size_t sz;
    *allocated = *resident = *active = 0;
    /* Update the statistics cached by mallctl. */
    sz = sizeof(epoch);
    je_mallctl("epoch", &epoch, &sz, &epoch, sz);
    sz = sizeof(size_t);
    /* Unlike RSS, this does not include RSS from shared libraries and other non
     * heap mappings. */
    je_mallctl("stats.resident", resident, &sz, NULL, 0);
    /* Unlike resident, this doesn't not include the pages jemalloc reserves
     * for re-use (purge will clean that). */
    je_mallctl("stats.active", active, &sz, NULL, 0);
    /* Unlike zmalloc_used_memory, this matches the stats.resident by taking
     * into account all allocations done by this process (not only zmalloc). */
    je_mallctl("stats.allocated", allocated, &sz, NULL, 0);
    return 1;
}

void set_jemalloc_bg_thread(int enable) {
    /* let jemalloc do purging asynchronously, required when there's no traffic 
     * after flushdb */
    char val = !!enable;
    je_mallctl("background_thread", NULL, 0, &val, 1);
}

int jemalloc_purge() {
    /* return all unused (reserved) pages to the OS */
    char tmp[32];
    unsigned narenas = 0;
    size_t sz = sizeof(unsigned);
    if (!je_mallctl("arenas.narenas", &narenas, &sz, NULL, 0)) {
        sprintf(tmp, "arena.%d.purge", narenas);
        if (!je_mallctl(tmp, NULL, 0, NULL, 0))
            return 0;
    }
    return -1;
}

#else

int zmalloc_get_allocator_info(size_t *allocated,
                               size_t *active,
                               size_t *resident) {
    *allocated = *resident = *active = 0;
    return 1;
}

void set_jemalloc_bg_thread(int enable) {
    ((void)(enable));
}

int jemalloc_purge() {
    return 0;
}

#endif

#if defined(__APPLE__)
/* For proc_pidinfo() used later in zmalloc_get_smap_bytes_by_field().
 * Note that this file cannot be included in zmalloc.h because it includes
 * a Darwin queue.h file where there is a "LIST_HEAD" macro (!) defined
 * conficting with Redis user code. */
#include <libproc.h>
#endif

/* Get the sum of the specified field (converted form kb to bytes) in
 * /proc/self/smaps. The field must be specified with trailing ":" as it
 * apperas in the smaps output.
 *
 * If a pid is specified, the information is extracted for such a pid,
 * otherwise if pid is -1 the information is reported is about the
 * current process.
 *
 * Example: zmalloc_get_smap_bytes_by_field("Rss:",-1);
 */
#if defined(HAVE_PROC_SMAPS)
size_t zmalloc_get_smap_bytes_by_field(char *field, long pid) {
    char line[1024];
    size_t bytes = 0;
    int flen = strlen(field);
    FILE *fp;

    if (pid == -1) {
        fp = fopen("/proc/self/smaps","r");
    } else {
        char filename[128];
        snprintf(filename,sizeof(filename),"/proc/%ld/smaps",pid);
        fp = fopen(filename,"r");
    }

    if (!fp) return 0;
    while(fgets(line,sizeof(line),fp) != NULL) {
        if (strncmp(line,field,flen) == 0) {
            char *p = strchr(line,'k');
            if (p) {
                *p = '\0';
                bytes += strtol(line+flen,NULL,10) * 1024;
            }
        }
    }
    fclose(fp);
    return bytes;
}
#else
/* Get sum of the specified field from libproc api call.
 * As there are per page value basis we need to convert
 * them accordingly.
 *
 * Note that AnonHugePages is a no-op as THP feature
 * is not supported in this platform
 */
size_t zmalloc_get_smap_bytes_by_field(char *field, long pid) {
#if defined(__APPLE__)
    struct proc_regioninfo pri;
    if (pid == -1) pid = getpid();
    if (proc_pidinfo(pid, PROC_PIDREGIONINFO, 0, &pri,
                     PROC_PIDREGIONINFO_SIZE) == PROC_PIDREGIONINFO_SIZE)
    {
        int pagesize = getpagesize();
        if (!strcmp(field, "Private_Dirty:")) {
            return (size_t)pri.pri_pages_dirtied * pagesize;
        } else if (!strcmp(field, "Rss:")) {
            return (size_t)pri.pri_pages_resident * pagesize;
        } else if (!strcmp(field, "AnonHugePages:")) {
            return 0;
        }
    }
    return 0;
#endif
    ((void) field);
    ((void) pid);
    return 0;
}
#endif

/* Return the total number bytes in pages marked as Private Dirty.
 *
 * Note: depending on the platform and memory footprint of the process, this
 * call can be slow, exceeding 1000ms!
 */
size_t zmalloc_get_private_dirty(long pid) {
    return zmalloc_get_smap_bytes_by_field("Private_Dirty:",pid);
}

/* Returns the size of physical memory (RAM) in bytes.
 * It looks ugly, but this is the cleanest way to achieve cross platform results.
 * Cleaned up from:
 *
 * http://nadeausoftware.com/articles/2012/09/c_c_tip_how_get_physical_memory_size_system
 *
 * Note that this function:
 * 1) Was released under the following CC attribution license:
 *    http://creativecommons.org/licenses/by/3.0/deed.en_US.
 * 2) Was originally implemented by David Robert Nadeau.
 * 3) Was modified for Redis by Matt Stancliff.
 * 4) This note exists in order to comply with the original license.
 */
size_t zmalloc_get_memory_size(void) {
#if defined(__unix__) || defined(__unix) || defined(unix) || \
    (defined(__APPLE__) && defined(__MACH__))
#if defined(CTL_HW) && (defined(HW_MEMSIZE) || defined(HW_PHYSMEM64))
    int mib[2];
    mib[0] = CTL_HW;
#if defined(HW_MEMSIZE)
    mib[1] = HW_MEMSIZE;            /* OSX. --------------------- */
#elif defined(HW_PHYSMEM64)
    mib[1] = HW_PHYSMEM64;          /* NetBSD, OpenBSD. --------- */
#endif
    int64_t size = 0;               /* 64-bit */
    size_t len = sizeof(size);
    if (sysctl( mib, 2, &size, &len, NULL, 0) == 0)
        return (size_t)size;
    return 0L;          /* Failed? */

#elif defined(_SC_PHYS_PAGES) && defined(_SC_PAGESIZE)
    /* FreeBSD, Linux, OpenBSD, and Solaris. -------------------- */
    return (size_t)sysconf(_SC_PHYS_PAGES) * (size_t)sysconf(_SC_PAGESIZE);

#elif defined(CTL_HW) && (defined(HW_PHYSMEM) || defined(HW_REALMEM))
    /* DragonFly BSD, FreeBSD, NetBSD, OpenBSD, and OSX. -------- */
    int mib[2];
    mib[0] = CTL_HW;
#if defined(HW_REALMEM)
    mib[1] = HW_REALMEM;        /* FreeBSD. ----------------- */
#elif defined(HW_PHYSMEM)
    mib[1] = HW_PHYSMEM;        /* Others. ------------------ */
#endif
    unsigned int size = 0;      /* 32-bit */
    size_t len = sizeof(size);
    if (sysctl(mib, 2, &size, &len, NULL, 0) == 0)
        return (size_t)size;
    return 0L;          /* Failed? */
#else
    return 0L;          /* Unknown method to get the data. */
#endif
#else
    return 0L;          /* Unknown OS. */
#endif
}

#ifdef REDIS_TEST
int zmalloc_test(int argc, char **argv, int flags) {
    void *ptr;

    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    printf("Malloc prefix size: %d\n", (int) PREFIX_SIZE);
    printf("Initial used memory: %zu\n", zmalloc_used_memory());
    ptr = zmalloc(123);
    printf("Allocated 123 bytes; used: %zu\n", zmalloc_used_memory());
    ptr = zrealloc(ptr, 456);
    printf("Reallocated to 456 bytes; used: %zu\n", zmalloc_used_memory());
    zfree(ptr);
    printf("Freed pointer; used: %zu\n", zmalloc_used_memory());
    return 0;
}
#endif
