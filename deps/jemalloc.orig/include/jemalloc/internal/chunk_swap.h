#ifdef JEMALLOC_SWAP
/******************************************************************************/
#ifdef JEMALLOC_H_TYPES

#endif /* JEMALLOC_H_TYPES */
/******************************************************************************/
#ifdef JEMALLOC_H_STRUCTS

#endif /* JEMALLOC_H_STRUCTS */
/******************************************************************************/
#ifdef JEMALLOC_H_EXTERNS

extern malloc_mutex_t	swap_mtx;
extern bool		swap_enabled;
extern bool		swap_prezeroed;
extern size_t		swap_nfds;
extern int		*swap_fds;
#ifdef JEMALLOC_STATS
extern size_t		swap_avail;
#endif

void	*chunk_alloc_swap(size_t size, bool *zero);
bool	chunk_in_swap(void *chunk);
bool	chunk_dealloc_swap(void *chunk, size_t size);
bool	chunk_swap_enable(const int *fds, unsigned nfds, bool prezeroed);
bool	chunk_swap_boot(void);

#endif /* JEMALLOC_H_EXTERNS */
/******************************************************************************/
#ifdef JEMALLOC_H_INLINES

#endif /* JEMALLOC_H_INLINES */
/******************************************************************************/
#endif /* JEMALLOC_SWAP */
