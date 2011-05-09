#ifdef JEMALLOC_DSS
/******************************************************************************/
#ifdef JEMALLOC_H_TYPES

#endif /* JEMALLOC_H_TYPES */
/******************************************************************************/
#ifdef JEMALLOC_H_STRUCTS

#endif /* JEMALLOC_H_STRUCTS */
/******************************************************************************/
#ifdef JEMALLOC_H_EXTERNS

/*
 * Protects sbrk() calls.  This avoids malloc races among threads, though it
 * does not protect against races with threads that call sbrk() directly.
 */
extern malloc_mutex_t	dss_mtx;

void	*chunk_alloc_dss(size_t size, bool *zero);
bool	chunk_in_dss(void *chunk);
bool	chunk_dealloc_dss(void *chunk, size_t size);
bool	chunk_dss_boot(void);

#endif /* JEMALLOC_H_EXTERNS */
/******************************************************************************/
#ifdef JEMALLOC_H_INLINES

#endif /* JEMALLOC_H_INLINES */
/******************************************************************************/
#endif /* JEMALLOC_DSS */
