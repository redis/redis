#ifndef JEMALLOC_INTERNAL_SEC_H
#define JEMALLOC_INTERNAL_SEC_H

#include "jemalloc/internal/atomic.h"
#include "jemalloc/internal/pai.h"

/*
 * Small extent cache.
 *
 * This includes some utilities to cache small extents.  We have a per-pszind
 * bin with its own list of extents of that size.  We don't try to do any
 * coalescing of extents (since it would in general require cross-shard locks or
 * knowledge of the underlying PAI implementation).
 */

/*
 * For now, this is just one field; eventually, we'll probably want to get more
 * fine-grained data out (like per-size class statistics).
 */
typedef struct sec_stats_s sec_stats_t;
struct sec_stats_s {
	/* Sum of bytes_cur across all shards. */
	size_t bytes;
};

static inline void
sec_stats_accum(sec_stats_t *dst, sec_stats_t *src) {
	dst->bytes += src->bytes;
}

/* A collections of free extents, all of the same size. */
typedef struct sec_bin_s sec_bin_t;
struct sec_bin_s {
	/*
	 * When we fail to fulfill an allocation, we do a batch-alloc on the
	 * underlying allocator to fill extra items, as well.  We drop the SEC
	 * lock while doing so, to allow operations on other bins to succeed.
	 * That introduces the possibility of other threads also trying to
	 * allocate out of this bin, failing, and also going to the backing
	 * allocator.  To avoid a thundering herd problem in which lots of
	 * threads do batch allocs and overfill this bin as a result, we only
	 * allow one batch allocation at a time for a bin.  This bool tracks
	 * whether or not some thread is already batch allocating.
	 *
	 * Eventually, the right answer may be a smarter sharding policy for the
	 * bins (e.g. a mutex per bin, which would also be more scalable
	 * generally; the batch-allocating thread could hold it while
	 * batch-allocating).
	 */
	bool being_batch_filled;

	/*
	 * Number of bytes in this particular bin (as opposed to the
	 * sec_shard_t's bytes_cur.  This isn't user visible or reported in
	 * stats; rather, it allows us to quickly determine the change in the
	 * centralized counter when flushing.
	 */
	size_t bytes_cur;
	edata_list_active_t freelist;
};

typedef struct sec_shard_s sec_shard_t;
struct sec_shard_s {
	/*
	 * We don't keep per-bin mutexes, even though that would allow more
	 * sharding; this allows global cache-eviction, which in turn allows for
	 * better balancing across free lists.
	 */
	malloc_mutex_t mtx;
	/*
	 * A SEC may need to be shut down (i.e. flushed of its contents and
	 * prevented from further caching).  To avoid tricky synchronization
	 * issues, we just track enabled-status in each shard, guarded by a
	 * mutex.  In practice, this is only ever checked during brief races,
	 * since the arena-level atomic boolean tracking HPA enabled-ness means
	 * that we won't go down these pathways very often after custom extent
	 * hooks are installed.
	 */
	bool enabled;
	sec_bin_t *bins;
	/* Number of bytes in all bins in the shard. */
	size_t bytes_cur;
	/* The next pszind to flush in the flush-some pathways. */
	pszind_t to_flush_next;
};

typedef struct sec_s sec_t;
struct sec_s {
	pai_t pai;
	pai_t *fallback;

	sec_opts_t opts;
	sec_shard_t *shards;
	pszind_t npsizes;
};

bool sec_init(tsdn_t *tsdn, sec_t *sec, base_t *base, pai_t *fallback,
    const sec_opts_t *opts);
void sec_flush(tsdn_t *tsdn, sec_t *sec);
void sec_disable(tsdn_t *tsdn, sec_t *sec);

/*
 * Morally, these two stats methods probably ought to be a single one (and the
 * mutex_prof_data ought to live in the sec_stats_t.  But splitting them apart
 * lets them fit easily into the pa_shard stats framework (which also has this
 * split), which simplifies the stats management.
 */
void sec_stats_merge(tsdn_t *tsdn, sec_t *sec, sec_stats_t *stats);
void sec_mutex_stats_read(tsdn_t *tsdn, sec_t *sec,
    mutex_prof_data_t *mutex_prof_data);

/*
 * We use the arena lock ordering; these are acquired in phase 2 of forking, but
 * should be acquired before the underlying allocator mutexes.
 */
void sec_prefork2(tsdn_t *tsdn, sec_t *sec);
void sec_postfork_parent(tsdn_t *tsdn, sec_t *sec);
void sec_postfork_child(tsdn_t *tsdn, sec_t *sec);

#endif /* JEMALLOC_INTERNAL_SEC_H */
