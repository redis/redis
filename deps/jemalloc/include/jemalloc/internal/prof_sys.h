#ifndef JEMALLOC_INTERNAL_PROF_SYS_H
#define JEMALLOC_INTERNAL_PROF_SYS_H

extern malloc_mutex_t prof_dump_filename_mtx;
extern base_t *prof_base;

void bt_init(prof_bt_t *bt, void **vec);
void prof_backtrace(tsd_t *tsd, prof_bt_t *bt);
void prof_hooks_init();
void prof_unwind_init();
void prof_sys_thread_name_fetch(tsd_t *tsd);
int prof_getpid(void);
void prof_get_default_filename(tsdn_t *tsdn, char *filename, uint64_t ind);
bool prof_prefix_set(tsdn_t *tsdn, const char *prefix);
void prof_fdump_impl(tsd_t *tsd);
void prof_idump_impl(tsd_t *tsd);
bool prof_mdump_impl(tsd_t *tsd, const char *filename);
void prof_gdump_impl(tsd_t *tsd);

/* Used in unit tests. */
typedef int (prof_sys_thread_name_read_t)(char *buf, size_t limit);
extern prof_sys_thread_name_read_t *JET_MUTABLE prof_sys_thread_name_read;
typedef int (prof_dump_open_file_t)(const char *, int);
extern prof_dump_open_file_t *JET_MUTABLE prof_dump_open_file;
typedef ssize_t (prof_dump_write_file_t)(int, const void *, size_t);
extern prof_dump_write_file_t *JET_MUTABLE prof_dump_write_file;
typedef int (prof_dump_open_maps_t)();
extern prof_dump_open_maps_t *JET_MUTABLE prof_dump_open_maps;

#endif /* JEMALLOC_INTERNAL_PROF_SYS_H */
