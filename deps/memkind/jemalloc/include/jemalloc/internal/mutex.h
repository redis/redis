/******************************************************************************/
#ifdef JEMALLOC_H_TYPES

typedef struct malloc_mutex_s malloc_mutex_t;

#ifdef _WIN32
#  define MALLOC_MUTEX_INITIALIZER
#elif (defined(JEMALLOC_OS_UNFAIR_LOCK))
#  define MALLOC_MUTEX_INITIALIZER					\
     {OS_UNFAIR_LOCK_INIT, WITNESS_INITIALIZER(WITNESS_RANK_OMIT)}
#elif (defined(JEMALLOC_OSSPIN))
#  define MALLOC_MUTEX_INITIALIZER {0, WITNESS_INITIALIZER(WITNESS_RANK_OMIT)}
#elif (defined(JEMALLOC_MUTEX_INIT_CB))
#  define MALLOC_MUTEX_INITIALIZER					\
    {PTHREAD_MUTEX_INITIALIZER, NULL, WITNESS_INITIALIZER(WITNESS_RANK_OMIT)}
#else
#  if (defined(JEMALLOC_HAVE_PTHREAD_MUTEX_ADAPTIVE_NP) &&		\
       defined(PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP))
#    define MALLOC_MUTEX_TYPE PTHREAD_MUTEX_ADAPTIVE_NP
#    define MALLOC_MUTEX_INITIALIZER					\
       {PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP,				\
        WITNESS_INITIALIZER(WITNESS_RANK_OMIT)}
#  else
#    define MALLOC_MUTEX_TYPE PTHREAD_MUTEX_DEFAULT
#    define MALLOC_MUTEX_INITIALIZER					\
       {PTHREAD_MUTEX_INITIALIZER, WITNESS_INITIALIZER(WITNESS_RANK_OMIT)}
#  endif
#endif

#endif /* JEMALLOC_H_TYPES */
/******************************************************************************/
#ifdef JEMALLOC_H_STRUCTS

struct malloc_mutex_s {
#ifdef _WIN32
#  if _WIN32_WINNT >= 0x0600
	SRWLOCK         	lock;
#  else
	CRITICAL_SECTION	lock;
#  endif
#elif (defined(JEMALLOC_OS_UNFAIR_LOCK))
	os_unfair_lock		lock;
#elif (defined(JEMALLOC_OSSPIN))
	OSSpinLock		lock;
#elif (defined(JEMALLOC_MUTEX_INIT_CB))
	pthread_mutex_t		lock;
	malloc_mutex_t		*postponed_next;
#else
	pthread_mutex_t		lock;
#endif
	witness_t		witness;
};

#endif /* JEMALLOC_H_STRUCTS */
/******************************************************************************/
#ifdef JEMALLOC_H_EXTERNS

#ifdef JEMALLOC_LAZY_LOCK
extern bool isthreaded;
#else
#  undef isthreaded /* Undo private_namespace.h definition. */
#  define isthreaded true
#endif

bool	malloc_mutex_init(malloc_mutex_t *mutex, const char *name,
    witness_rank_t rank);
void	malloc_mutex_prefork(tsdn_t *tsdn, malloc_mutex_t *mutex);
void	malloc_mutex_postfork_parent(tsdn_t *tsdn, malloc_mutex_t *mutex);
void	malloc_mutex_postfork_child(tsdn_t *tsdn, malloc_mutex_t *mutex);
bool	malloc_mutex_boot(void);

#endif /* JEMALLOC_H_EXTERNS */
/******************************************************************************/
#ifdef JEMALLOC_H_INLINES

#ifndef JEMALLOC_ENABLE_INLINE
void	malloc_mutex_lock(tsdn_t *tsdn, malloc_mutex_t *mutex);
void	malloc_mutex_unlock(tsdn_t *tsdn, malloc_mutex_t *mutex);
void	malloc_mutex_assert_owner(tsdn_t *tsdn, malloc_mutex_t *mutex);
void	malloc_mutex_assert_not_owner(tsdn_t *tsdn, malloc_mutex_t *mutex);
#endif

#if (defined(JEMALLOC_ENABLE_INLINE) || defined(JEMALLOC_MUTEX_C_))
JEMALLOC_INLINE void
malloc_mutex_lock(tsdn_t *tsdn, malloc_mutex_t *mutex)
{

	witness_assert_not_owner(tsdn, &mutex->witness);
	if (isthreaded) {
#ifdef _WIN32
#  if _WIN32_WINNT >= 0x0600
		AcquireSRWLockExclusive(&mutex->lock);
#  else
		EnterCriticalSection(&mutex->lock);
#  endif
#elif (defined(JEMALLOC_OS_UNFAIR_LOCK))
		os_unfair_lock_lock(&mutex->lock);
#elif (defined(JEMALLOC_OSSPIN))
		OSSpinLockLock(&mutex->lock);
#else
		pthread_mutex_lock(&mutex->lock);
#endif
	}
	witness_lock(tsdn, &mutex->witness);
}

JEMALLOC_INLINE void
malloc_mutex_unlock(tsdn_t *tsdn, malloc_mutex_t *mutex)
{

	witness_unlock(tsdn, &mutex->witness);
	if (isthreaded) {
#ifdef _WIN32
#  if _WIN32_WINNT >= 0x0600
		ReleaseSRWLockExclusive(&mutex->lock);
#  else
		LeaveCriticalSection(&mutex->lock);
#  endif
#elif (defined(JEMALLOC_OS_UNFAIR_LOCK))
		os_unfair_lock_unlock(&mutex->lock);
#elif (defined(JEMALLOC_OSSPIN))
		OSSpinLockUnlock(&mutex->lock);
#else
		pthread_mutex_unlock(&mutex->lock);
#endif
	}
}

JEMALLOC_INLINE void
malloc_mutex_assert_owner(tsdn_t *tsdn, malloc_mutex_t *mutex)
{

	witness_assert_owner(tsdn, &mutex->witness);
}

JEMALLOC_INLINE void
malloc_mutex_assert_not_owner(tsdn_t *tsdn, malloc_mutex_t *mutex)
{

	witness_assert_not_owner(tsdn, &mutex->witness);
}
#endif

#endif /* JEMALLOC_H_INLINES */
/******************************************************************************/
