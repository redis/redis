#ifndef JEMALLOC_INTERNAL_LARGE_EXTERNS_H
#define JEMALLOC_INTERNAL_LARGE_EXTERNS_H

#include "jemalloc/internal/hook.h"

void *large_malloc(tsdn_t *tsdn, arena_t *arena, size_t usize, bool zero);
void *large_palloc(tsdn_t *tsdn, arena_t *arena, size_t usize, size_t alignment,
    bool zero);
bool large_ralloc_no_move(tsdn_t *tsdn, edata_t *edata, size_t usize_min,
    size_t usize_max, bool zero);
void *large_ralloc(tsdn_t *tsdn, arena_t *arena, void *ptr, size_t usize,
    size_t alignment, bool zero, tcache_t *tcache,
    hook_ralloc_args_t *hook_args);

void large_dalloc_prep_locked(tsdn_t *tsdn, edata_t *edata);
void large_dalloc_finish(tsdn_t *tsdn, edata_t *edata);
void large_dalloc(tsdn_t *tsdn, edata_t *edata);
size_t large_salloc(tsdn_t *tsdn, const edata_t *edata);
void large_prof_info_get(tsd_t *tsd, edata_t *edata, prof_info_t *prof_info,
    bool reset_recent);
void large_prof_tctx_reset(edata_t *edata);
void large_prof_info_set(edata_t *edata, prof_tctx_t *tctx, size_t size);

#endif /* JEMALLOC_INTERNAL_LARGE_EXTERNS_H */
