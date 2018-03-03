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
#define	atomic_read_p(p)	atomic_add_p(p, NULL)
#define	atomic_read_z(p)	atomic_add_z(p, 0)
#define	atomic_read_u(p)	atomic_add_u(p, 0)

#endif /* JEMALLOC_H_EXTERNS */
/******************************************************************************/
#ifdef JEMALLOC_H_INLINES

/*
 * All arithmetic functions return the arithmetic result of the atomic
 * operation.  Some atomic operation APIs return the value prior to mutation, in
 * which case the following functions must redundantly compute the result so
 * that it can be returned.  These functions are normally inlined, so the extra
 * operations can be optimized away if the return values aren't used by the
 * callers.
 *
 *   <t> atomic_read_<t>(<t> *p) { return (*p); }
 *   <t> atomic_add_<t>(<t> *p, <t> x) { return (*p + x); }
 *   <t> atomic_sub_<t>(<t> *p, <t> x) { return (*p - x); }
 *   bool atomic_cas_<t>(<t> *p, <t> c, <t> s)
 *   {
 *     if (*p != c)
 *       return (true);
 *     *p = s;
 *     return (false);
 *   }
 *   void atomic_write_<t>(<t> *p, <t> x) { *p = x; }
 */

#ifndef JEMALLOC_ENABLE_INLINE
uint64_t	atomic_add_uint64(uint64_t *p, uint64_t x);
uint64_t	atomic_sub_uint64(uint64_t *p, uint64_t x);
bool	atomic_cas_uint64(uint64_t *p, uint64_t c, uint64_t s);
void	atomic_write_uint64(uint64_t *p, uint64_t x);
uint32_t	atomic_add_uint32(uint32_t *p, uint32_t x);
uint32_t	atomic_sub_uint32(uint32_t *p, uint32_t x);
bool	atomic_cas_uint32(uint32_t *p, uint32_t c, uint32_t s);
void	atomic_write_uint32(uint32_t *p, uint32_t x);
void	*atomic_add_p(void **p, void *x);
void	*atomic_sub_p(void **p, void *x);
bool	atomic_cas_p(void **p, void *c, void *s);
void	atomic_write_p(void **p, const void *x);
size_t	atomic_add_z(size_t *p, size_t x);
size_t	atomic_sub_z(size_t *p, size_t x);
bool	atomic_cas_z(size_t *p, size_t c, size_t s);
void	atomic_write_z(size_t *p, size_t x);
unsigned	atomic_add_u(unsigned *p, unsigned x);
unsigned	atomic_sub_u(unsigned *p, unsigned x);
bool	atomic_cas_u(unsigned *p, unsigned c, unsigned s);
void	atomic_write_u(unsigned *p, unsigned x);
#endif

#if (defined(JEMALLOC_ENABLE_INLINE) || defined(JEMALLOC_ATOMIC_C_))
/******************************************************************************/
/* 64-bit operations. */
#if (LG_SIZEOF_PTR == 3 || LG_SIZEOF_INT == 3)
#  if (defined(__amd64__) || defined(__x86_64__))
JEMALLOC_INLINE uint64_t
atomic_add_uint64(uint64_t *p, uint64_t x)
{
	uint64_t t = x;

	asm volatile (
	    "lock; xaddq %0, %1;"
	    : "+r" (t), "=m" (*p) /* Outputs. */
	    : "m" (*p) /* Inputs. */
	    );

	return (t + x);
}

JEMALLOC_INLINE uint64_t
atomic_sub_uint64(uint64_t *p, uint64_t x)
{
	uint64_t t;

	x = (uint64_t)(-(int64_t)x);
	t = x;
	asm volatile (
	    "lock; xaddq %0, %1;"
	    : "+r" (t), "=m" (*p) /* Outputs. */
	    : "m" (*p) /* Inputs. */
	    );

	return (t + x);
}

JEMALLOC_INLINE bool
atomic_cas_uint64(uint64_t *p, uint64_t c, uint64_t s)
{
	uint8_t success;

	asm volatile (
	    "lock; cmpxchgq %4, %0;"
	    "sete %1;"
	    : "=m" (*p), "=a" (success) /* Outputs. */
	    : "m" (*p), "a" (c), "r" (s) /* Inputs. */
	    : "memory" /* Clobbers. */
	    );

	return (!(bool)success);
}

JEMALLOC_INLINE void
atomic_write_uint64(uint64_t *p, uint64_t x)
{

	asm volatile (
	    "xchgq %1, %0;" /* Lock is implied by xchgq. */
	    : "=m" (*p), "+r" (x) /* Outputs. */
	    : "m" (*p) /* Inputs. */
	    : "memory" /* Clobbers. */
	    );
}
#  elif (defined(JEMALLOC_C11ATOMICS))
JEMALLOC_INLINE uint64_t
atomic_add_uint64(uint64_t *p, uint64_t x)
{
	volatile atomic_uint_least64_t *a = (volatile atomic_uint_least64_t *)p;
	return (atomic_fetch_add(a, x) + x);
}

JEMALLOC_INLINE uint64_t
atomic_sub_uint64(uint64_t *p, uint64_t x)
{
	volatile atomic_uint_least64_t *a = (volatile atomic_uint_least64_t *)p;
	return (atomic_fetch_sub(a, x) - x);
}

JEMALLOC_INLINE bool
atomic_cas_uint64(uint64_t *p, uint64_t c, uint64_t s)
{
	volatile atomic_uint_least64_t *a = (volatile atomic_uint_least64_t *)p;
	return (!atomic_compare_exchange_strong(a, &c, s));
}

JEMALLOC_INLINE void
atomic_write_uint64(uint64_t *p, uint64_t x)
{
	volatile atomic_uint_least64_t *a = (volatile atomic_uint_least64_t *)p;
	atomic_store(a, x);
}
#  elif (defined(JEMALLOC_ATOMIC9))
JEMALLOC_INLINE uint64_t
atomic_add_uint64(uint64_t *p, uint64_t x)
{

	/*
	 * atomic_fetchadd_64() doesn't exist, but we only ever use this
	 * function on LP64 systems, so atomic_fetchadd_long() will do.
	 */
	assert(sizeof(uint64_t) == sizeof(unsigned long));

	return (atomic_fetchadd_long(p, (unsigned long)x) + x);
}

JEMALLOC_INLINE uint64_t
atomic_sub_uint64(uint64_t *p, uint64_t x)
{

	assert(sizeof(uint64_t) == sizeof(unsigned long));

	return (atomic_fetchadd_long(p, (unsigned long)(-(long)x)) - x);
}

JEMALLOC_INLINE bool
atomic_cas_uint64(uint64_t *p, uint64_t c, uint64_t s)
{

	assert(sizeof(uint64_t) == sizeof(unsigned long));

	return (!atomic_cmpset_long(p, (unsigned long)c, (unsigned long)s));
}

JEMALLOC_INLINE void
atomic_write_uint64(uint64_t *p, uint64_t x)
{

	assert(sizeof(uint64_t) == sizeof(unsigned long));

	atomic_store_rel_long(p, x);
}
#  elif (defined(JEMALLOC_OSATOMIC))
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

JEMALLOC_INLINE bool
atomic_cas_uint64(uint64_t *p, uint64_t c, uint64_t s)
{

	return (!OSAtomicCompareAndSwap64(c, s, (int64_t *)p));
}

JEMALLOC_INLINE void
atomic_write_uint64(uint64_t *p, uint64_t x)
{
	uint64_t o;

	/*The documented OSAtomic*() API does not expose an atomic exchange. */
	do {
		o = atomic_read_uint64(p);
	} while (atomic_cas_uint64(p, o, x));
}
#  elif (defined(_MSC_VER))
JEMALLOC_INLINE uint64_t
atomic_add_uint64(uint64_t *p, uint64_t x)
{

	return (InterlockedExchangeAdd64(p, x) + x);
}

JEMALLOC_INLINE uint64_t
atomic_sub_uint64(uint64_t *p, uint64_t x)
{

	return (InterlockedExchangeAdd64(p, -((int64_t)x)) - x);
}

JEMALLOC_INLINE bool
atomic_cas_uint64(uint64_t *p, uint64_t c, uint64_t s)
{
	uint64_t o;

	o = InterlockedCompareExchange64(p, s, c);
	return (o != c);
}

JEMALLOC_INLINE void
atomic_write_uint64(uint64_t *p, uint64_t x)
{

	InterlockedExchange64(p, x);
}
#  elif (defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_8) || \
    defined(JE_FORCE_SYNC_COMPARE_AND_SWAP_8))
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

JEMALLOC_INLINE bool
atomic_cas_uint64(uint64_t *p, uint64_t c, uint64_t s)
{

	return (!__sync_bool_compare_and_swap(p, c, s));
}

JEMALLOC_INLINE void
atomic_write_uint64(uint64_t *p, uint64_t x)
{

	__sync_lock_test_and_set(p, x);
}
#  else
#    error "Missing implementation for 64-bit atomic operations"
#  endif
#endif

/******************************************************************************/
/* 32-bit operations. */
#if (defined(__i386__) || defined(__amd64__) || defined(__x86_64__))
JEMALLOC_INLINE uint32_t
atomic_add_uint32(uint32_t *p, uint32_t x)
{
	uint32_t t = x;

	asm volatile (
	    "lock; xaddl %0, %1;"
	    : "+r" (t), "=m" (*p) /* Outputs. */
	    : "m" (*p) /* Inputs. */
	    );

	return (t + x);
}

JEMALLOC_INLINE uint32_t
atomic_sub_uint32(uint32_t *p, uint32_t x)
{
	uint32_t t;

	x = (uint32_t)(-(int32_t)x);
	t = x;
	asm volatile (
	    "lock; xaddl %0, %1;"
	    : "+r" (t), "=m" (*p) /* Outputs. */
	    : "m" (*p) /* Inputs. */
	    );

	return (t + x);
}

JEMALLOC_INLINE bool
atomic_cas_uint32(uint32_t *p, uint32_t c, uint32_t s)
{
	uint8_t success;

	asm volatile (
	    "lock; cmpxchgl %4, %0;"
	    "sete %1;"
	    : "=m" (*p), "=a" (success) /* Outputs. */
	    : "m" (*p), "a" (c), "r" (s) /* Inputs. */
	    : "memory"
	    );

	return (!(bool)success);
}

JEMALLOC_INLINE void
atomic_write_uint32(uint32_t *p, uint32_t x)
{

	asm volatile (
	    "xchgl %1, %0;" /* Lock is implied by xchgl. */
	    : "=m" (*p), "+r" (x) /* Outputs. */
	    : "m" (*p) /* Inputs. */
	    : "memory" /* Clobbers. */
	    );
}
#  elif (defined(JEMALLOC_C11ATOMICS))
JEMALLOC_INLINE uint32_t
atomic_add_uint32(uint32_t *p, uint32_t x)
{
	volatile atomic_uint_least32_t *a = (volatile atomic_uint_least32_t *)p;
	return (atomic_fetch_add(a, x) + x);
}

JEMALLOC_INLINE uint32_t
atomic_sub_uint32(uint32_t *p, uint32_t x)
{
	volatile atomic_uint_least32_t *a = (volatile atomic_uint_least32_t *)p;
	return (atomic_fetch_sub(a, x) - x);
}

JEMALLOC_INLINE bool
atomic_cas_uint32(uint32_t *p, uint32_t c, uint32_t s)
{
	volatile atomic_uint_least32_t *a = (volatile atomic_uint_least32_t *)p;
	return (!atomic_compare_exchange_strong(a, &c, s));
}

JEMALLOC_INLINE void
atomic_write_uint32(uint32_t *p, uint32_t x)
{
	volatile atomic_uint_least32_t *a = (volatile atomic_uint_least32_t *)p;
	atomic_store(a, x);
}
#elif (defined(JEMALLOC_ATOMIC9))
JEMALLOC_INLINE uint32_t
atomic_add_uint32(uint32_t *p, uint32_t x)
{

	return (atomic_fetchadd_32(p, x) + x);
}

JEMALLOC_INLINE uint32_t
atomic_sub_uint32(uint32_t *p, uint32_t x)
{

	return (atomic_fetchadd_32(p, (uint32_t)(-(int32_t)x)) - x);
}

JEMALLOC_INLINE bool
atomic_cas_uint32(uint32_t *p, uint32_t c, uint32_t s)
{

	return (!atomic_cmpset_32(p, c, s));
}

JEMALLOC_INLINE void
atomic_write_uint32(uint32_t *p, uint32_t x)
{

	atomic_store_rel_32(p, x);
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

JEMALLOC_INLINE bool
atomic_cas_uint32(uint32_t *p, uint32_t c, uint32_t s)
{

	return (!OSAtomicCompareAndSwap32(c, s, (int32_t *)p));
}

JEMALLOC_INLINE void
atomic_write_uint32(uint32_t *p, uint32_t x)
{
	uint32_t o;

	/*The documented OSAtomic*() API does not expose an atomic exchange. */
	do {
		o = atomic_read_uint32(p);
	} while (atomic_cas_uint32(p, o, x));
}
#elif (defined(_MSC_VER))
JEMALLOC_INLINE uint32_t
atomic_add_uint32(uint32_t *p, uint32_t x)
{

	return (InterlockedExchangeAdd(p, x) + x);
}

JEMALLOC_INLINE uint32_t
atomic_sub_uint32(uint32_t *p, uint32_t x)
{

	return (InterlockedExchangeAdd(p, -((int32_t)x)) - x);
}

JEMALLOC_INLINE bool
atomic_cas_uint32(uint32_t *p, uint32_t c, uint32_t s)
{
	uint32_t o;

	o = InterlockedCompareExchange(p, s, c);
	return (o != c);
}

JEMALLOC_INLINE void
atomic_write_uint32(uint32_t *p, uint32_t x)
{

	InterlockedExchange(p, x);
}
#elif (defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_4) || \
 defined(JE_FORCE_SYNC_COMPARE_AND_SWAP_4))
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

JEMALLOC_INLINE bool
atomic_cas_uint32(uint32_t *p, uint32_t c, uint32_t s)
{

	return (!__sync_bool_compare_and_swap(p, c, s));
}

JEMALLOC_INLINE void
atomic_write_uint32(uint32_t *p, uint32_t x)
{

	__sync_lock_test_and_set(p, x);
}
#else
#  error "Missing implementation for 32-bit atomic operations"
#endif

/******************************************************************************/
/* Pointer operations. */
JEMALLOC_INLINE void *
atomic_add_p(void **p, void *x)
{

#if (LG_SIZEOF_PTR == 3)
	return ((void *)atomic_add_uint64((uint64_t *)p, (uint64_t)x));
#elif (LG_SIZEOF_PTR == 2)
	return ((void *)atomic_add_uint32((uint32_t *)p, (uint32_t)x));
#endif
}

JEMALLOC_INLINE void *
atomic_sub_p(void **p, void *x)
{

#if (LG_SIZEOF_PTR == 3)
	return ((void *)atomic_add_uint64((uint64_t *)p,
	    (uint64_t)-((int64_t)x)));
#elif (LG_SIZEOF_PTR == 2)
	return ((void *)atomic_add_uint32((uint32_t *)p,
	    (uint32_t)-((int32_t)x)));
#endif
}

JEMALLOC_INLINE bool
atomic_cas_p(void **p, void *c, void *s)
{

#if (LG_SIZEOF_PTR == 3)
	return (atomic_cas_uint64((uint64_t *)p, (uint64_t)c, (uint64_t)s));
#elif (LG_SIZEOF_PTR == 2)
	return (atomic_cas_uint32((uint32_t *)p, (uint32_t)c, (uint32_t)s));
#endif
}

JEMALLOC_INLINE void
atomic_write_p(void **p, const void *x)
{

#if (LG_SIZEOF_PTR == 3)
	atomic_write_uint64((uint64_t *)p, (uint64_t)x);
#elif (LG_SIZEOF_PTR == 2)
	atomic_write_uint32((uint32_t *)p, (uint32_t)x);
#endif
}

/******************************************************************************/
/* size_t operations. */
JEMALLOC_INLINE size_t
atomic_add_z(size_t *p, size_t x)
{

#if (LG_SIZEOF_PTR == 3)
	return ((size_t)atomic_add_uint64((uint64_t *)p, (uint64_t)x));
#elif (LG_SIZEOF_PTR == 2)
	return ((size_t)atomic_add_uint32((uint32_t *)p, (uint32_t)x));
#endif
}

JEMALLOC_INLINE size_t
atomic_sub_z(size_t *p, size_t x)
{

#if (LG_SIZEOF_PTR == 3)
	return ((size_t)atomic_add_uint64((uint64_t *)p,
	    (uint64_t)-((int64_t)x)));
#elif (LG_SIZEOF_PTR == 2)
	return ((size_t)atomic_add_uint32((uint32_t *)p,
	    (uint32_t)-((int32_t)x)));
#endif
}

JEMALLOC_INLINE bool
atomic_cas_z(size_t *p, size_t c, size_t s)
{

#if (LG_SIZEOF_PTR == 3)
	return (atomic_cas_uint64((uint64_t *)p, (uint64_t)c, (uint64_t)s));
#elif (LG_SIZEOF_PTR == 2)
	return (atomic_cas_uint32((uint32_t *)p, (uint32_t)c, (uint32_t)s));
#endif
}

JEMALLOC_INLINE void
atomic_write_z(size_t *p, size_t x)
{

#if (LG_SIZEOF_PTR == 3)
	atomic_write_uint64((uint64_t *)p, (uint64_t)x);
#elif (LG_SIZEOF_PTR == 2)
	atomic_write_uint32((uint32_t *)p, (uint32_t)x);
#endif
}

/******************************************************************************/
/* unsigned operations. */
JEMALLOC_INLINE unsigned
atomic_add_u(unsigned *p, unsigned x)
{

#if (LG_SIZEOF_INT == 3)
	return ((unsigned)atomic_add_uint64((uint64_t *)p, (uint64_t)x));
#elif (LG_SIZEOF_INT == 2)
	return ((unsigned)atomic_add_uint32((uint32_t *)p, (uint32_t)x));
#endif
}

JEMALLOC_INLINE unsigned
atomic_sub_u(unsigned *p, unsigned x)
{

#if (LG_SIZEOF_INT == 3)
	return ((unsigned)atomic_add_uint64((uint64_t *)p,
	    (uint64_t)-((int64_t)x)));
#elif (LG_SIZEOF_INT == 2)
	return ((unsigned)atomic_add_uint32((uint32_t *)p,
	    (uint32_t)-((int32_t)x)));
#endif
}

JEMALLOC_INLINE bool
atomic_cas_u(unsigned *p, unsigned c, unsigned s)
{

#if (LG_SIZEOF_INT == 3)
	return (atomic_cas_uint64((uint64_t *)p, (uint64_t)c, (uint64_t)s));
#elif (LG_SIZEOF_INT == 2)
	return (atomic_cas_uint32((uint32_t *)p, (uint32_t)c, (uint32_t)s));
#endif
}

JEMALLOC_INLINE void
atomic_write_u(unsigned *p, unsigned x)
{

#if (LG_SIZEOF_INT == 3)
	atomic_write_uint64((uint64_t *)p, (uint64_t)x);
#elif (LG_SIZEOF_INT == 2)
	atomic_write_uint32((uint32_t *)p, (uint32_t)x);
#endif
}

/******************************************************************************/
#endif

#endif /* JEMALLOC_H_INLINES */
/******************************************************************************/
