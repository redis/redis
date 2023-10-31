#ifndef JEMALLOC_INTERNAL_PROF_TYPES_H
#define JEMALLOC_INTERNAL_PROF_TYPES_H

typedef struct prof_bt_s prof_bt_t;
typedef struct prof_cnt_s prof_cnt_t;
typedef struct prof_tctx_s prof_tctx_t;
typedef struct prof_info_s prof_info_t;
typedef struct prof_gctx_s prof_gctx_t;
typedef struct prof_tdata_s prof_tdata_t;
typedef struct prof_recent_s prof_recent_t;

/* Option defaults. */
#ifdef JEMALLOC_PROF
#  define PROF_PREFIX_DEFAULT		"jeprof"
#else
#  define PROF_PREFIX_DEFAULT		""
#endif
#define LG_PROF_SAMPLE_DEFAULT		19
#define LG_PROF_INTERVAL_DEFAULT	-1

/*
 * Hard limit on stack backtrace depth.  The version of prof_backtrace() that
 * is based on __builtin_return_address() necessarily has a hard-coded number
 * of backtrace frame handlers, and should be kept in sync with this setting.
 */
#define PROF_BT_MAX			128

/* Initial hash table size. */
#define PROF_CKH_MINITEMS		64

/* Size of memory buffer to use when writing dump files. */
#ifndef JEMALLOC_PROF
/* Minimize memory bloat for non-prof builds. */
#  define PROF_DUMP_BUFSIZE		1
#elif defined(JEMALLOC_DEBUG)
/* Use a small buffer size in debug build, mainly to facilitate testing. */
#  define PROF_DUMP_BUFSIZE		16
#else
#  define PROF_DUMP_BUFSIZE		65536
#endif

/* Size of size class related tables */
#ifdef JEMALLOC_PROF
#  define PROF_SC_NSIZES		SC_NSIZES
#else
/* Minimize memory bloat for non-prof builds. */
#  define PROF_SC_NSIZES		1
#endif

/* Size of stack-allocated buffer used by prof_printf(). */
#define PROF_PRINTF_BUFSIZE		128

/*
 * Number of mutexes shared among all gctx's.  No space is allocated for these
 * unless profiling is enabled, so it's okay to over-provision.
 */
#define PROF_NCTX_LOCKS			1024

/*
 * Number of mutexes shared among all tdata's.  No space is allocated for these
 * unless profiling is enabled, so it's okay to over-provision.
 */
#define PROF_NTDATA_LOCKS		256

/* Minimize memory bloat for non-prof builds. */
#ifdef JEMALLOC_PROF
#define PROF_DUMP_FILENAME_LEN (PATH_MAX + 1)
#else
#define PROF_DUMP_FILENAME_LEN 1
#endif

/* Default number of recent allocations to record. */
#define PROF_RECENT_ALLOC_MAX_DEFAULT 0

#endif /* JEMALLOC_INTERNAL_PROF_TYPES_H */
