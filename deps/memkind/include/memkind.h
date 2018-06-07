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

#include <sys/types.h>

/*
 * Header file for the memkind heap manager.
 * More details in memkind(3) man page.
 *
 * API standards are described in memkind(3) man page.
 */

/* EXPERIMENTAL API */
#define _MEMKIND_BIT(N) (1ull << N)

/* EXPERIMENTAL API */
/** Memory types. */
typedef enum memkind_memtype_t {

    /**
     * Select standard memory, the same as process use.
     */
    MEMKIND_MEMTYPE_DEFAULT = _MEMKIND_BIT(0),

    /**
     * Select high bandwidth memory (HBM).
     * There must be at least two memories with different bandwidth to
     * determine the HBM.
     */
    MEMKIND_MEMTYPE_HIGH_BANDWIDTH = _MEMKIND_BIT(1)

} memkind_memtype_t;

#undef _MEMKIND_BIT

/* EXPERIMENTAL API */
/** Policy */
typedef enum memkind_policy_t {

    /**
     * Allocate local memory.
     * If there is not enough memory to satisfy the request errno is set to
     * ENOMEM and the allocated pointer is set to NULL.
     */
    MEMKIND_POLICY_BIND_LOCAL = 0,

    /**
     * Memory locality is ignored.
     * If there is not enough memory to satisfy the request errno is set to
     * ENOMEM and the allocated pointer is set to NULL.
     */
    MEMKIND_POLICY_BIND_ALL,

    /**
     * Allocate preferred memory that is local.
     * If there is not enough preferred memory to satisfy the request or
     * preferred memory is not available, the allocation will fall back on any
     * other memory.
     */
    MEMKIND_POLICY_PREFERRED_LOCAL,

    /**
     * Interleave allocation across local memory.
     * For n memory types the allocation will be interleaved across all of
     * them.
     */
    MEMKIND_POLICY_INTERLEAVE_LOCAL,

    /**
     * Interleave allocation. Locality is ignored.
     * For n memory types the allocation will be interleaved across all of
     * them.
     */
    MEMKIND_POLICY_INTERLEAVE_ALL,

    /**
     * Max policy value.
     */
    MEMKIND_POLICY_MAX_VALUE

} memkind_policy_t;

/* EXPERIMENTAL API */
/**
 * The bits specify flags and masks.
 * Bits <0,1,2,...,7> are reserved for page size, where page sizes are encoded
 * by base-2 logarithm. If the page size bits are set to zero value, than default
 * page size will be used.
 */
typedef enum memkind_bits_t {

    /** Allocations backed by 2 MB page size. */
    MEMKIND_MASK_PAGE_SIZE_2MB = 21ull, //2^21 = 2MB

} memkind_bits_t;

/* EXPERIMENTAL API */
typedef struct memkind* memkind_t;


/* EXPERIMENTAL API */
enum memkind_const {
    MEMKIND_MAX_KIND = 512,
    MEMKIND_ERROR_MESSAGE_SIZE = 128
};

/* EXPERIMENTAL API */
/** Return codes for success operations and errors. */
enum {
    /** Operation success. */
    MEMKIND_SUCCESS = 0,
    MEMKIND_ERROR_UNAVAILABLE = -1,
    MEMKIND_ERROR_MBIND = -2,
    MEMKIND_ERROR_MMAP  = -3,
    MEMKIND_ERROR_MALLOC = -6,
    MEMKIND_ERROR_ENVIRON = -12,
    /** Invalid argument. */
    MEMKIND_ERROR_INVALID = -13,
    MEMKIND_ERROR_TOOMANY = -15,
    MEMKIND_ERROR_BADOPS = -17,
    MEMKIND_ERROR_HUGETLB = -18,
    /** Requested memory type is not available. */
    MEMKIND_ERROR_MEMTYPE_NOT_AVAILABLE = -20,
    /** Operation failed. */
    MEMKIND_ERROR_OPERATION_FAILED = -21,
    MEMKIND_ERROR_RUNTIME = -255
};

/* Constructors */
/**
 * Create kind that allocates memory with specific memory type,
 * memory binding policy and flags.
 * Parameters:
 *
 *     memtype_flags - determine the memory types to allocate from by
 *     combination of memkind_memtype_t values. This field cannot have zero
 *     value.
 *
 *     policy - specify policy for page binding to memory types selected by
 *     memtype_flags. This field must be set to memkind_policy_t value.
 *     If policy is set to MEMKIND_POLICY_PREFERRED_LOCAL then only one memory
 *     type must be selected. Note: the value cannot be set to
 *     MEMKIND_POLICY_MAX_VALUE.
 *
 *     flags - the field must be set to a combination of memkind_bits_t
 *     values.
 *
 *     kind - pointer to handle in which the created kind is returned.
 *
 * Return codes:
 *
 *     MEMKIND_SUCCESS
 *
 *     MEMKIND_ERROR_MEMTYPE_NOT_AVAILABLE will be returned if any of the
 *     following conditions occurs:
 *      - requested policy is different than MEMKIND_POLICY_PREFERRED_LOCAL,
 *      - could not detect memory of requested type;
 *
 *     MEMKIND_ERROR_INVALID will be returned if any of the
 *     following conditions occurs:
 *      - argument configuration is not supported or not implemented,
 *      - invalid argument value.
 *
 * Note: currently implemented attributes configurations (memtype_flags,
 * policy, flags):
 * {MEMKIND_MEMTYPE_DEFAULT, MEMKIND_POLICY_PREFERRED_LOCAL, 0},
 * {MEMKIND_MEMTYPE_DEFAULT, MEMKIND_POLICY_PREFERRED_LOCAL,
 *  MEMKIND_MASK_PAGE_SIZE_2MB},
 * {MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_BIND_LOCAL, 0},
 * {MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_BIND_LOCAL,
 *  MEMKIND_MASK_PAGE_SIZE_2MB},
 * {MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_PREFERRED_LOCAL, 0}
 * {MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_PREFERRED_LOCAL,
 *  MEMKIND_MASK_PAGE_SIZE_2MB}
 * {MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_INTERLEAVE_ALL, 0}
 * {MEMKIND_MEMTYPE_DEFAULT and MEMKIND_MEMTYPE_HIGH_BANDWIDTH,
 *  MEMKIND_POLICY_INTERLEAVE_ALL, 0}.
 */
int memkind_create_kind(memkind_memtype_t memtype_flags,
                        memkind_policy_t policy,
                        memkind_bits_t flags,
                        memkind_t* kind);

/* Destructors */
/** Destroy the kind object.
 * Parameters:
 *
 *     kind - handle of the kind to destroy.
 *     The kind object needs to be initialized by memkind_create_kind() before it
 *     is destroyed. The function has undefined behavior when the handle is
 *     invalid.
 *     Note: all allocated memory must be freed before kind is destroyed, otherwise
 *     this may cause memory leak.
 *
 * Return codes:
 *
 *     MEMKIND_SUCCESS,
 *
 *     MEMKIND_ERROR_OPERATION_FAILED.
 */
int memkind_destroy_kind(memkind_t kind);


#include "memkind_deprecated.h"

/* EXPERIMENTAL API */
extern memkind_t MEMKIND_DEFAULT;

/* EXPERIMENTAL API */
extern memkind_t MEMKIND_HUGETLB;

/* EXPERIMENTAL API */
extern memkind_t MEMKIND_HBW;

/* EXPERIMENTAL API */
extern memkind_t MEMKIND_HBW_PREFERRED;

/* EXPERIMENTAL API */
extern memkind_t MEMKIND_HBW_HUGETLB;

/* EXPERIMENTAL API */
extern memkind_t MEMKIND_HBW_PREFERRED_HUGETLB;

/* EXPERIMENTAL API */
extern memkind_t MEMKIND_HBW_GBTLB;

/* EXPERIMENTAL API */
extern memkind_t MEMKIND_HBW_PREFERRED_GBTLB;

/* EXPERIMENTAL API */
extern memkind_t MEMKIND_GBTLB;

/* EXPERIMENTAL API */
extern memkind_t MEMKIND_HBW_INTERLEAVE;

/* EXPERIMENTAL API */
extern memkind_t MEMKIND_INTERLEAVE;


/*STANDARD API*/
/* API versioning */
int memkind_get_version();


/* EXPERIMENTAL API */
/* Convert error number into an error message */
void memkind_error_message(int err, char *msg, size_t size);


/* KIND MANAGEMENT INTERFACE */

/* EXPERIMENTAL API */
/* Create a new PMEM (file-backed) kind of given size on top of a temporary file */
int memkind_create_pmem(const char *dir, size_t max_size, memkind_t *kind);

/* EXPERIMENTAL API */
/* returns 0 if memory kind is available else returns error code */
int memkind_check_available(memkind_t kind);

/* HEAP MANAGEMENT INTERFACE */

/* EXPERIMENTAL API */
/* malloc from the numa nodes of the specified kind */
void *memkind_malloc(memkind_t kind, size_t size);

/* EXPERIMENTAL API */
/* calloc from the numa nodes of the specified kind */
void *memkind_calloc(memkind_t kind, size_t num, size_t size);

/* EXPERIMENTAL API */
/* posix_memalign from the numa nodes of the specified kind */
int memkind_posix_memalign(memkind_t kind, void **memptr, size_t alignment,
                           size_t size);

/* EXPERIMENTAL API */
/* realloc from the numa nodes of the specified kind */
void *memkind_realloc(memkind_t kind, void *ptr, size_t size);

/* EXPERIMENTAL API */
/* Free memory allocated with the memkind API */
void memkind_free(memkind_t kind, void *ptr);

#ifdef __cplusplus
}
#endif
