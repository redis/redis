/******************************************************************************/
#ifdef JEMALLOC_H_TYPES

#endif /* JEMALLOC_H_TYPES */
/******************************************************************************/
#ifdef JEMALLOC_H_STRUCTS

#endif /* JEMALLOC_H_STRUCTS */
/******************************************************************************/
#ifdef JEMALLOC_H_EXTERNS

void	*base_alloc(tsdn_t *tsdn, size_t size);
void	base_stats_get(tsdn_t *tsdn, size_t *allocated, size_t *resident,
    size_t *mapped);
bool	base_boot(void);
void	base_prefork(tsdn_t *tsdn);
void	base_postfork_parent(tsdn_t *tsdn);
void	base_postfork_child(tsdn_t *tsdn);

#endif /* JEMALLOC_H_EXTERNS */
/******************************************************************************/
#ifdef JEMALLOC_H_INLINES

#endif /* JEMALLOC_H_INLINES */
/******************************************************************************/
