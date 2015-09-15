/******************************************************************************/
#ifdef JEMALLOC_H_TYPES

#endif /* JEMALLOC_H_TYPES */
/******************************************************************************/
#ifdef JEMALLOC_H_STRUCTS

#endif /* JEMALLOC_H_STRUCTS */
/******************************************************************************/
#ifdef JEMALLOC_H_EXTERNS

/* Huge allocation statistics. */
extern uint64_t		huge_nmalloc;
extern uint64_t		huge_ndalloc;
extern size_t		huge_allocated;

/* Protects chunk-related data structures. */
extern malloc_mutex_t	huge_mtx;

void	*huge_malloc(size_t size, bool zero, dss_prec_t dss_prec);
void	*huge_palloc(size_t size, size_t alignment, bool zero,
    dss_prec_t dss_prec);
bool	huge_ralloc_no_move(void *ptr, size_t oldsize, size_t size,
    size_t extra);
void	*huge_ralloc(void *ptr, size_t oldsize, size_t size, size_t extra,
    size_t alignment, bool zero, bool try_tcache_dalloc, dss_prec_t dss_prec);
#ifdef JEMALLOC_JET
typedef void (huge_dalloc_junk_t)(void *, size_t);
extern huge_dalloc_junk_t *huge_dalloc_junk;
#endif
void	huge_dalloc(void *ptr, bool unmap);
size_t	huge_salloc(const void *ptr);
dss_prec_t	huge_dss_prec_get(arena_t *arena);
prof_ctx_t	*huge_prof_ctx_get(const void *ptr);
void	huge_prof_ctx_set(const void *ptr, prof_ctx_t *ctx);
bool	huge_boot(void);
void	huge_prefork(void);
void	huge_postfork_parent(void);
void	huge_postfork_child(void);

#endif /* JEMALLOC_H_EXTERNS */
/******************************************************************************/
#ifdef JEMALLOC_H_INLINES

#endif /* JEMALLOC_H_INLINES */
/******************************************************************************/
