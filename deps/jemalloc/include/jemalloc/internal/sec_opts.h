#ifndef JEMALLOC_INTERNAL_SEC_OPTS_H
#define JEMALLOC_INTERNAL_SEC_OPTS_H

/*
 * The configuration settings used by an sec_t.  Morally, this is part of the
 * SEC interface, but we put it here for header-ordering reasons.
 */

typedef struct sec_opts_s sec_opts_t;
struct sec_opts_s {
	/*
	 * We don't necessarily always use all the shards; requests are
	 * distributed across shards [0, nshards - 1).
	 */
	size_t nshards;
	/*
	 * We'll automatically refuse to cache any objects in this sec if
	 * they're larger than max_alloc bytes, instead forwarding such objects
	 * directly to the fallback.
	 */
	size_t max_alloc;
	/*
	 * Exceeding this amount of cached extents in a shard causes us to start
	 * flushing bins in that shard until we fall below bytes_after_flush.
	 */
	size_t max_bytes;
	/*
	 * The number of bytes (in all bins) we flush down to when we exceed
	 * bytes_cur.  We want this to be less than bytes_cur, because
	 * otherwise we could get into situations where a shard undergoing
	 * net-deallocation keeps bytes_cur very near to max_bytes, so that
	 * most deallocations get immediately forwarded to the underlying PAI
	 * implementation, defeating the point of the SEC.
	 */
	size_t bytes_after_flush;
	/*
	 * When we can't satisfy an allocation out of the SEC because there are
	 * no available ones cached, we allocate multiple of that size out of
	 * the fallback allocator.  Eventually we might want to do something
	 * cleverer, but for now we just grab a fixed number.
	 */
	size_t batch_fill_extra;
};

#define SEC_OPTS_DEFAULT {						\
	/* nshards */							\
	4,								\
	/* max_alloc */							\
	(32 * 1024) < PAGE ? PAGE : (32 * 1024),			\
	/* max_bytes */							\
	256 * 1024,							\
	/* bytes_after_flush */						\
	128 * 1024,							\
	/* batch_fill_extra */						\
	0								\
}


#endif /* JEMALLOC_INTERNAL_SEC_OPTS_H */
