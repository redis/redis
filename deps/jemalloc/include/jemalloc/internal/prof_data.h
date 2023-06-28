#ifndef JEMALLOC_INTERNAL_PROF_DATA_H
#define JEMALLOC_INTERNAL_PROF_DATA_H

#include "jemalloc/internal/mutex.h"

extern malloc_mutex_t bt2gctx_mtx;
extern malloc_mutex_t tdatas_mtx;
extern malloc_mutex_t prof_dump_mtx;

extern malloc_mutex_t *gctx_locks;
extern malloc_mutex_t *tdata_locks;

extern size_t prof_unbiased_sz[PROF_SC_NSIZES];
extern size_t prof_shifted_unbiased_cnt[PROF_SC_NSIZES];

void prof_bt_hash(const void *key, size_t r_hash[2]);
bool prof_bt_keycomp(const void *k1, const void *k2);

bool prof_data_init(tsd_t *tsd);
prof_tctx_t *prof_lookup(tsd_t *tsd, prof_bt_t *bt);
char *prof_thread_name_alloc(tsd_t *tsd, const char *thread_name);
int prof_thread_name_set_impl(tsd_t *tsd, const char *thread_name);
void prof_unbias_map_init();
void prof_dump_impl(tsd_t *tsd, write_cb_t *prof_dump_write, void *cbopaque,
    prof_tdata_t *tdata, bool leakcheck);
prof_tdata_t * prof_tdata_init_impl(tsd_t *tsd, uint64_t thr_uid,
    uint64_t thr_discrim, char *thread_name, bool active);
void prof_tdata_detach(tsd_t *tsd, prof_tdata_t *tdata);
void prof_reset(tsd_t *tsd, size_t lg_sample);
void prof_tctx_try_destroy(tsd_t *tsd, prof_tctx_t *tctx);

/* Used in unit tests. */
size_t prof_tdata_count(void);
size_t prof_bt_count(void);
void prof_cnt_all(prof_cnt_t *cnt_all);

#endif /* JEMALLOC_INTERNAL_PROF_DATA_H */
