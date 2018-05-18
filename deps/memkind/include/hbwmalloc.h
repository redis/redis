/*
 * Copyright (C) 2014 - 2016 Intel Corporation.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice(s),
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice(s),
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER(S) ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO
 * EVENT SHALL THE COPYRIGHT HOLDER(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include <stdlib.h>
/*
 *  Header file for the high bandwidth memory interface.
 *
 *  This file defines the external API's and enumerations for the
 *  hbwmalloc library.  These interfaces define a heap manager that
 *  targets the high bandwidth memory numa nodes.
 *
 *  hbwmalloc.h functionality is considered as stable API (STANDARD API).
 *
 *  Please read hbwmalloc(3) man page for or more details.
 */

/*
 *  Fallback policy.
 *
 *  Policy that determines behavior when there is not enough free high
 *  bandwidth memory to satisfy a user request.  This enum is used with
 *  hbw_get_policy() and hbw_set_policy().
 */
typedef enum {
    /*
     *  If insufficient high bandwidth memory pages are available then
     *  OOM killer will be triggered.
     */
    HBW_POLICY_BIND = 1,
    /*
     *  If insufficient high bandwidth memory pages are available fall
     *  back on standard memory pages.
     */
    HBW_POLICY_PREFERRED = 2,
    /*
     *  Interleave pages across high bandwidth nodes. If insufficient memory
     *  pages are available then OOM killer will be triggered.
     */
    HBW_POLICY_INTERLEAVE = 3
} hbw_policy_t;

/*
 *  Page size selection.
 *
 *  The hbw_posix_memalign_psize() API gives the user the option to
 *  select the page size from this enumerated list.
 */
typedef enum {

    /*
     * The four kilobyte page size option. Note that with transparent huge
     * pages enabled these allocations may be promoted by the operating system
     * to two megabyte pages.
     */
    HBW_PAGESIZE_4KB           = 1,

    /*
     * The two megabyte page size option.
     */
    HBW_PAGESIZE_2MB           = 2,
    /*
     * The one gigabyte page size option. The total size of the allocation must
     * be a multiple of 1GB with this option, otherwise the allocation will
     * fail.
     */
    HBW_PAGESIZE_1GB_STRICT    = 3,

    /*
     * This option allows the user to specify arbitrary sizes backed by one
     * gigabytes pages. Gigabyte pages are allocated even if the size is not a
     * modulo of 1GB. A good example of using this feature with realloc is
     * shown in gb_realloc_example.c
     */
    HBW_PAGESIZE_1GB           = 4,

    /*
     * Helper representing value of the last enum element incremented by 1.
     * Shall not be treated as a valid value for functions taking hbw_pagesize_t
     * as parameter.
     */
    HBW_PAGESIZE_MAX_VALUE
} hbw_pagesize_t;

/*
 * Flags for hbw_verify_ptr function
 */
enum {

    /*
     * This option touches first byte of all pages in address range starting from "addr" to "addr" + "size"
     * by read and write (so the content will be overwitten by the same data as it was read).
     */
    HBW_TOUCH_PAGES      = (1 << 0)
};

/*
 * Returns the current fallback policy when insufficient high bandwidth memory
 * is available.
 */
hbw_policy_t hbw_get_policy(void);

/*
 * Sets the current fallback policy. The policy can be modified only once in
 * the lifetime of an application and before calling hbw_*alloc() or
 * hbw_posix_memalign*() function.
 * Note: If the policy is not set, than HBW_POLICY_PREFERRED will be used by
 * default.
 *
 * Returns:
 *   0: on success
 *   EPERM: if hbw_set_policy () was called more than once
 *   EINVAL: if mode argument was neither HBW_POLICY_PREFERRED, HBW_POLICY_BIND nor HBW_POLICY_INTERLEAVE
 */
int hbw_set_policy(hbw_policy_t mode);

/*
 * Verifies high bandwidth memory availability.
 * Returns:
 *   0: if high bandwidth memory is available
 *   ENODEV: if high-bandwidth memory is unavailable.
 */
int hbw_check_available(void);

/*
 * Verifies if allocated memory fully fall into high bandwidth memory.
 * Returns:
 *   0: if memory in address range from "addr" to "addr" + "size" is allocated in high bandwidth memory
 *   -1: if any region of memory was not allocated in high bandwidth memory
 *   EINVAL: if addr is NULL, size equals 0 or flags contained unsupported bit set
 *   EFAULT: could not verify memory
 */
int hbw_verify_memory_region(void* addr, size_t size, int flags);

/*
 * Allocates size bytes of uninitialized high bandwidth memory.
 * The allocated space is  suitably  aligned (after  possible  pointer
 * coercion) for storage of any type of object. If size is zero then
 * hbw_malloc() returns NULL.
 */
void *hbw_malloc(size_t size);

/*
 * Allocates space for num objects in high bandwidth memory, each size bytes
 * in length.
 * The result is identical to calling hbw_malloc() with an argument of
 * num*size, with the exception that the allocated memory is explicitly
 * initialized to zero bytes.
 * If num or size is 0, then hbw_calloc() returns NULL.
 */
void *hbw_calloc(size_t num, size_t size);

/*
 * Allocates size bytes of high bandwidth memory such that the allocation's
 * base address is an even multiple of alignment, and returns the allocation
 * in the value pointed to by memptr.  The requested alignment must be a power
 * of 2 at least as large as sizeof(void *).
 * Returns:
 *   0: on success
 *   ENOMEM: if there was insufficient memory to satisfy the request
 *   EINVAL: if the alignment parameter was not a power of two, or was less than sizeof(void *)
 */
int hbw_posix_memalign(void **memptr, size_t alignment, size_t size);

/*
 * Allocates size bytes of high bandwidth memory such that the allocation's
 * base address is an even multiple of alignment, and returns the allocation
 * in the value pointed to by memptr. The requested alignment must be a power
 * of 2 at least as large as sizeof(void  *). The memory will be allocated
 * using pages determined by the pagesize variable.
 * Returns:
 *   0: on success
 *   ENOMEM: if there was insufficient memory to satisfy the request
 *   EINVAL: if the alignment parameter was not a power of two, or was less than sizeof(void *)
 */
int hbw_posix_memalign_psize(void **memptr, size_t alignment, size_t size,
                             hbw_pagesize_t pagesize);

/*
 * Changes the size of the previously allocated memory referenced by ptr to
 * size bytes of the specified kind. The contents of the memory are unchanged
 * up to the lesser of the new and old size.
 * If the new size is larger, the contents of the newly allocated portion
 * of the memory are undefined.
 * Upon success, the memory referenced by ptr is freed and a pointer to the
 * newly allocated high bandwidth memory is returned.
 * Note: memkind_realloc() may move the memory allocation, resulting in a
 * different return value than ptr.
 * If ptr is NULL, the hbw_realloc() function behaves identically to
 * hbw_malloc() for the specified size. The address ptr, if not NULL,
 * was returned by a previous call to hbw_malloc(), hbw_calloc(),
 * hbw_realloc(), or hbw_posix_memalign(). Otherwise, or if hbw_free(ptr)
 * was called before, undefined behavior occurs.
 * Note: hbw_realloc() cannot be used with a pointer returned by
 * hbw_posix_memalign_psize().
 */
void *hbw_realloc(void *ptr, size_t size);

/*
 * Causes the allocated memory referenced by ptr to be made
 * available for future allocations. If ptr is NULL, no action occurs.
 * The address ptr, if not NULL, must have been returned by a previous call
 * to hbw_malloc(), hbw_calloc(), hbw_realloc(), hbw_posix_memalign(), or
 * hbw_posix_memalign_psize(). Otherwise, if hbw_free(ptr) was called before,
 * undefined behavior occurs.
 */
void hbw_free(void *ptr);

#ifdef __cplusplus
}
#endif
