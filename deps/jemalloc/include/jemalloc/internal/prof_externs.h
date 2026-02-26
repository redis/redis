#ifndef JEMALLOC_INTERNAL_PROF_EXTERNS_H
#define JEMALLOC_INTERNAL_PROF_EXTERNS_H

#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/prof_hook.h"

extern bool opt_prof;
extern bool opt_prof_active;
extern bool opt_prof_thread_active_init;
extern size_t opt_lg_prof_sample;    /* Mean bytes between samples. */
extern ssize_t opt_lg_prof_interval; /* lg(prof_interval). */
extern bool opt_prof_gdump;          /* High-water memory dumping. */
extern bool opt_prof_final;          /* Final profile dumping. */
extern bool opt_prof_leak;           /* Dump leak summary at exit. */
extern bool opt_prof_leak_error;     /* Exit with error code if memory leaked */
extern bool opt_prof_accum;          /* Report cumulative bytes. */
extern bool opt_prof_log;            /* Turn logging on at boot. */
extern char opt_prof_prefix[
    /* Minimize memory bloat for non-prof builds. */
#ifdef JEMALLOC_PROF
    PATH_MAX +
#endif
    1];
extern bool opt_prof_unbias;

/* For recording recent allocations */
extern ssize_t opt_prof_recent_alloc_max;

/* Whether to use thread name provided by the system or by mallctl. */
extern bool opt_prof_sys_thread_name;

/* Whether to record per size class counts and request size totals. */
extern bool opt_prof_stats;

/* Accessed via prof_active_[gs]et{_unlocked,}(). */
extern bool prof_active_state;

/* Accessed via prof_gdump_[gs]et{_unlocked,}(). */
extern bool prof_gdump_val;

/* Profile dump interval, measured in bytes allocated. */
extern uint64_t prof_interval;

/*
 * Initialized as opt_lg_prof_sample, and potentially modified during profiling
 * resets.
 */
extern size_t lg_prof_sample;

extern bool prof_booted;

void prof_backtrace_hook_set(prof_backtrace_hook_t hook);
prof_backtrace_hook_t prof_backtrace_hook_get();

void prof_dump_hook_set(prof_dump_hook_t hook);
prof_dump_hook_t prof_dump_hook_get();

/* Functions only accessed in prof_inlines.h */
prof_tdata_t *prof_tdata_init(tsd_t *tsd);
prof_tdata_t *prof_tdata_reinit(tsd_t *tsd, prof_tdata_t *tdata);

void prof_alloc_rollback(tsd_t *tsd, prof_tctx_t *tctx);
void prof_malloc_sample_object(tsd_t *tsd, const void *ptr, size_t size,
    size_t usize, prof_tctx_t *tctx);
void prof_free_sampled_object(tsd_t *tsd, size_t usize, prof_info_t *prof_info);
prof_tctx_t *prof_tctx_create(tsd_t *tsd);
void prof_idump(tsdn_t *tsdn);
bool prof_mdump(tsd_t *tsd, const char *filename);
void prof_gdump(tsdn_t *tsdn);

void prof_tdata_cleanup(tsd_t *tsd);
bool prof_active_get(tsdn_t *tsdn);
bool prof_active_set(tsdn_t *tsdn, bool active);
const char *prof_thread_name_get(tsd_t *tsd);
int prof_thread_name_set(tsd_t *tsd, const char *thread_name);
bool prof_thread_active_get(tsd_t *tsd);
bool prof_thread_active_set(tsd_t *tsd, bool active);
bool prof_thread_active_init_get(tsdn_t *tsdn);
bool prof_thread_active_init_set(tsdn_t *tsdn, bool active_init);
bool prof_gdump_get(tsdn_t *tsdn);
bool prof_gdump_set(tsdn_t *tsdn, bool active);
void prof_boot0(void);
void prof_boot1(void);
bool prof_boot2(tsd_t *tsd, base_t *base);
void prof_prefork0(tsdn_t *tsdn);
void prof_prefork1(tsdn_t *tsdn);
void prof_postfork_parent(tsdn_t *tsdn);
void prof_postfork_child(tsdn_t *tsdn);

/* Only accessed by thread event. */
uint64_t prof_sample_new_event_wait(tsd_t *tsd);
uint64_t prof_sample_postponed_event_wait(tsd_t *tsd);
void prof_sample_event_handler(tsd_t *tsd, uint64_t elapsed);

#endif /* JEMALLOC_INTERNAL_PROF_EXTERNS_H */
