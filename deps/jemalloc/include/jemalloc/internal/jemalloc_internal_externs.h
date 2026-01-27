#ifndef JEMALLOC_INTERNAL_EXTERNS_H
#define JEMALLOC_INTERNAL_EXTERNS_H

#include "jemalloc/internal/atomic.h"
#include "jemalloc/internal/hpa_opts.h"
#include "jemalloc/internal/sec_opts.h"
#include "jemalloc/internal/tsd_types.h"
#include "jemalloc/internal/nstime.h"

/* TSD checks this to set thread local slow state accordingly. */
extern bool malloc_slow;

/* Run-time options. */
extern bool opt_abort;
extern bool opt_abort_conf;
extern bool opt_trust_madvise;
extern bool opt_confirm_conf;
extern bool opt_hpa;
extern hpa_shard_opts_t opt_hpa_opts;
extern sec_opts_t opt_hpa_sec_opts;

extern const char *opt_junk;
extern bool opt_junk_alloc;
extern bool opt_junk_free;
extern void (*junk_free_callback)(void *ptr, size_t size);
extern void (*junk_alloc_callback)(void *ptr, size_t size);
extern bool opt_utrace;
extern bool opt_xmalloc;
extern bool opt_experimental_infallible_new;
extern bool opt_zero;
extern unsigned opt_narenas;
extern zero_realloc_action_t opt_zero_realloc_action;
extern malloc_init_t malloc_init_state;
extern const char *zero_realloc_mode_names[];
extern atomic_zu_t zero_realloc_count;
extern bool opt_cache_oblivious;

/* Escape free-fastpath when ptr & mask == 0 (for sanitization purpose). */
extern uintptr_t san_cache_bin_nonfast_mask;

/* Number of CPUs. */
extern unsigned ncpus;

/* Number of arenas used for automatic multiplexing of threads and arenas. */
extern unsigned narenas_auto;

/* Base index for manual arenas. */
extern unsigned manual_arena_base;

/*
 * Arenas that are used to service external requests.  Not all elements of the
 * arenas array are necessarily used; arenas are created lazily as needed.
 */
extern atomic_p_t arenas[];

void *a0malloc(size_t size);
void a0dalloc(void *ptr);
void *bootstrap_malloc(size_t size);
void *bootstrap_calloc(size_t num, size_t size);
void bootstrap_free(void *ptr);
void arena_set(unsigned ind, arena_t *arena);
unsigned narenas_total_get(void);
arena_t *arena_init(tsdn_t *tsdn, unsigned ind, const arena_config_t *config);
arena_t *arena_choose_hard(tsd_t *tsd, bool internal);
void arena_migrate(tsd_t *tsd, arena_t *oldarena, arena_t *newarena);
void iarena_cleanup(tsd_t *tsd);
void arena_cleanup(tsd_t *tsd);
size_t batch_alloc(void **ptrs, size_t num, size_t size, int flags);
void jemalloc_prefork(void);
void jemalloc_postfork_parent(void);
void jemalloc_postfork_child(void);
void je_sdallocx_noflags(void *ptr, size_t size);
void *malloc_default(size_t size, size_t *usize);

#endif /* JEMALLOC_INTERNAL_EXTERNS_H */
