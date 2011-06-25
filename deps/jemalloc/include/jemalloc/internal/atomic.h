/******************************************************************************/
#ifdef JEMALLOC_H_TYPES

#endif /* JEMALLOC_H_TYPES */
/******************************************************************************/
#ifdef JEMALLOC_H_STRUCTS

#endif /* JEMALLOC_H_STRUCTS */
/******************************************************************************/
#ifdef JEMALLOC_H_EXTERNS

#define	atomic_read_uint64(p)	atomic_add_uint64(p, 0)
#define	atomic_read_uint32(p)	atomic_add_uint32(p, 0)

#if (LG_SIZEOF_PTR == 3)
#  define atomic_read_z(p)						\
    (size_t)atomic_add_uint64((uint64_t *)p, (uint64_t)0)
#  define atomic_add_z(p, x)						\
    (size_t)atomic_add_uint64((uint64_t *)p, (uint64_t)x)
#  define atomic_sub_z(p, x)						\
    (size_t)atomic_sub_uint64((uint64_t *)p, (uint64_t)x)
#elif (LG_SIZEOF_PTR == 2)
#  define atomic_read_z(p)						\
    (size_t)atomic_add_uint32((uint32_t *)p, (uint32_t)0)
#  define atomic_add_z(p, x)						\
    (size_t)atomic_add_uint32((uint32_t *)p, (uint32_t)x)
#  define atomic_sub_z(p, x)						\
    (size_t)atomic_sub_uint32((uint32_t *)p, (uint32_t)x)
#endif

#endif /* JEMALLOC_H_EXTERNS */
/******************************************************************************/
#ifdef JEMALLOC_H_INLINES

#ifndef JEMALLOC_ENABLE_INLINE
uint64_t	atomic_add_uint64(uint64_t *p, uint64_t x);
uint64_t	atomic_sub_uint64(uint64_t *p, uint64_t x);
uint32_t	atomic_add_uint32(uint32_t *p, uint32_t x);
uint32_t	atomic_sub_uint32(uint32_t *p, uint32_t x);
#endif

#if (defined(JEMALLOC_ENABLE_INLINE) || defined(JEMALLOC_ATOMIC_C_))
/******************************************************************************/
/* 64-bit operations. */
#ifdef __GCC_HAVE_SYNC_COMPARE_AND_SWAP_8
JEMALLOC_INLINE uint64_t
atomic_add_uint64(uint64_t *p, uint64_t x)
{

	return (__sync_add_and_fetch(p, x));
}

JEMALLOC_INLINE uint64_t
atomic_sub_uint64(uint64_t *p, uint64_t x)
{

	return (__sync_sub_and_fetch(p, x));
}
#elif (defined(JEMALLOC_OSATOMIC))
JEMALLOC_INLINE uint64_t
atomic_add_uint64(uint64_t *p, uint64_t x)
{

	return (OSAtomicAdd64((int64_t)x, (int64_t *)p));
}

JEMALLOC_INLINE uint64_t
atomic_sub_uint64(uint64_t *p, uint64_t x)
{

	return (OSAtomicAdd64(-((int64_t)x), (int64_t *)p));
}
#elif (defined(__amd64_) || defined(__x86_64__))
JEMALLOC_INLINE uint64_t
atomic_add_uint64(uint64_t *p, uint64_t x)
{

	asm volatile (
	    "lock; xaddq %0, %1;"
	    : "+r" (x), "=m" (*p) /* Outputs. */
	    : "m" (*p) /* Inputs. */
	    );

	return (x);
}

JEMALLOC_INLINE uint64_t
atomic_sub_uint64(uint64_t *p, uint64_t x)
{

	x = (uint64_t)(-(int64_t)x);
	asm volatile (
	    "lock; xaddq %0, %1;"
	    : "+r" (x), "=m" (*p) /* Outputs. */
	    : "m" (*p) /* Inputs. */
	    );

	return (x);
}
#else
#  if (LG_SIZEOF_PTR == 3)
#    error "Missing implementation for 64-bit atomic operations"
#  endif
#endif

/******************************************************************************/
/* 32-bit operations. */
#ifdef __GCC_HAVE_SYNC_COMPARE_AND_SWAP_4
JEMALLOC_INLINE uint32_t
atomic_add_uint32(uint32_t *p, uint32_t x)
{

	return (__sync_add_and_fetch(p, x));
}

JEMALLOC_INLINE uint32_t
atomic_sub_uint32(uint32_t *p, uint32_t x)
{

	return (__sync_sub_and_fetch(p, x));
}
#elif (defined(JEMALLOC_OSATOMIC))
JEMALLOC_INLINE uint32_t
atomic_add_uint32(uint32_t *p, uint32_t x)
{

	return (OSAtomicAdd32((int32_t)x, (int32_t *)p));
}

JEMALLOC_INLINE uint32_t
atomic_sub_uint32(uint32_t *p, uint32_t x)
{

	return (OSAtomicAdd32(-((int32_t)x), (int32_t *)p));
}
#elif (defined(__i386__) || defined(__amd64_) || defined(__x86_64__))
JEMALLOC_INLINE uint32_t
atomic_add_uint32(uint32_t *p, uint32_t x)
{

	asm volatile (
	    "lock; xaddl %0, %1;"
	    : "+r" (x), "=m" (*p) /* Outputs. */
	    : "m" (*p) /* Inputs. */
	    );

	return (x);
}

JEMALLOC_INLINE uint32_t
atomic_sub_uint32(uint32_t *p, uint32_t x)
{

	x = (uint32_t)(-(int32_t)x);
	asm volatile (
	    "lock; xaddl %0, %1;"
	    : "+r" (x), "=m" (*p) /* Outputs. */
	    : "m" (*p) /* Inputs. */
	    );

	return (x);
}
#else
#  error "Missing implementation for 32-bit atomic operations"
#endif
#endif

#endif /* JEMALLOC_H_INLINES */
/******************************************************************************/
