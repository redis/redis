/******************************************************************************/
#ifdef JEMALLOC_H_TYPES

#endif /* JEMALLOC_H_TYPES */
/******************************************************************************/
#ifdef JEMALLOC_H_STRUCTS

#endif /* JEMALLOC_H_STRUCTS */
/******************************************************************************/
#ifdef JEMALLOC_H_EXTERNS

void	*pages_map(void *addr, size_t size);
void	pages_unmap(void *addr, size_t size);
void	*pages_trim(void *addr, size_t alloc_size, size_t leadsize,
    size_t size);
bool	pages_commit(void *addr, size_t size);
bool	pages_decommit(void *addr, size_t size);
bool	pages_purge(void *addr, size_t size);

#endif /* JEMALLOC_H_EXTERNS */
/******************************************************************************/
#ifdef JEMALLOC_H_INLINES

#endif /* JEMALLOC_H_INLINES */
/******************************************************************************/

