#define	JEMALLOC_PROF_C_
#include "jemalloc/internal/jemalloc_internal.h"
#ifdef JEMALLOC_PROF
/******************************************************************************/

#ifdef JEMALLOC_PROF_LIBUNWIND
#define	UNW_LOCAL_ONLY
#include <libunwind.h>
#endif

#ifdef JEMALLOC_PROF_LIBGCC
#include <unwind.h>
#endif

/******************************************************************************/
/* Data. */

bool		opt_prof = false;
bool		opt_prof_active = true;
size_t		opt_lg_prof_bt_max = LG_PROF_BT_MAX_DEFAULT;
size_t		opt_lg_prof_sample = LG_PROF_SAMPLE_DEFAULT;
ssize_t		opt_lg_prof_interval = LG_PROF_INTERVAL_DEFAULT;
bool		opt_prof_gdump = false;
bool		opt_prof_leak = false;
bool		opt_prof_accum = true;
ssize_t		opt_lg_prof_tcmax = LG_PROF_TCMAX_DEFAULT;
char		opt_prof_prefix[PATH_MAX + 1];

uint64_t	prof_interval;
bool		prof_promote;

unsigned	prof_bt_max;

#ifndef NO_TLS
__thread prof_tdata_t	*prof_tdata_tls
    JEMALLOC_ATTR(tls_model("initial-exec"));
#endif
pthread_key_t	prof_tdata_tsd;

/*
 * Global hash of (prof_bt_t *)-->(prof_ctx_t *).  This is the master data
 * structure that knows about all backtraces currently captured.
 */
static ckh_t		bt2ctx;
static malloc_mutex_t	bt2ctx_mtx;

static malloc_mutex_t	prof_dump_seq_mtx;
static uint64_t		prof_dump_seq;
static uint64_t		prof_dump_iseq;
static uint64_t		prof_dump_mseq;
static uint64_t		prof_dump_useq;

/*
 * This buffer is rather large for stack allocation, so use a single buffer for
 * all profile dumps.  The buffer is implicitly protected by bt2ctx_mtx, since
 * it must be locked anyway during dumping.
 */
static char		prof_dump_buf[PROF_DUMP_BUF_SIZE];
static unsigned		prof_dump_buf_end;
static int		prof_dump_fd;

/* Do not dump any profiles until bootstrapping is complete. */
static bool		prof_booted = false;

static malloc_mutex_t	enq_mtx;
static bool		enq;
static bool		enq_idump;
static bool		enq_gdump;

/******************************************************************************/
/* Function prototypes for non-inline static functions. */

static prof_bt_t	*bt_dup(prof_bt_t *bt);
static void	bt_destroy(prof_bt_t *bt);
#ifdef JEMALLOC_PROF_LIBGCC
static _Unwind_Reason_Code	prof_unwind_init_callback(
    struct _Unwind_Context *context, void *arg);
static _Unwind_Reason_Code	prof_unwind_callback(
    struct _Unwind_Context *context, void *arg);
#endif
static bool	prof_flush(bool propagate_err);
static bool	prof_write(const char *s, bool propagate_err);
static void	prof_ctx_sum(prof_ctx_t *ctx, prof_cnt_t *cnt_all,
    size_t *leak_nctx);
static void	prof_ctx_destroy(prof_ctx_t *ctx);
static void	prof_ctx_merge(prof_ctx_t *ctx, prof_thr_cnt_t *cnt);
static bool	prof_dump_ctx(prof_ctx_t *ctx, prof_bt_t *bt,
    bool propagate_err);
static bool	prof_dump_maps(bool propagate_err);
static bool	prof_dump(const char *filename, bool leakcheck,
    bool propagate_err);
static void	prof_dump_filename(char *filename, char v, int64_t vseq);
static void	prof_fdump(void);
static void	prof_bt_hash(const void *key, unsigned minbits, size_t *hash1,
    size_t *hash2);
static bool	prof_bt_keycomp(const void *k1, const void *k2);
static void	prof_tdata_cleanup(void *arg);

/******************************************************************************/

void
bt_init(prof_bt_t *bt, void **vec)
{

	bt->vec = vec;
	bt->len = 0;
}

static void
bt_destroy(prof_bt_t *bt)
{

	idalloc(bt);
}

static prof_bt_t *
bt_dup(prof_bt_t *bt)
{
	prof_bt_t *ret;

	/*
	 * Create a single allocation that has space for vec immediately
	 * following the prof_bt_t structure.  The backtraces that get
	 * stored in the backtrace caches are copied from stack-allocated
	 * temporary variables, so size is known at creation time.  Making this
	 * a contiguous object improves cache locality.
	 */
	ret = (prof_bt_t *)imalloc(QUANTUM_CEILING(sizeof(prof_bt_t)) +
	    (bt->len * sizeof(void *)));
	if (ret == NULL)
		return (NULL);
	ret->vec = (void **)((uintptr_t)ret +
	    QUANTUM_CEILING(sizeof(prof_bt_t)));
	memcpy(ret->vec, bt->vec, bt->len * sizeof(void *));
	ret->len = bt->len;

	return (ret);
}

static inline void
prof_enter(void)
{

	malloc_mutex_lock(&enq_mtx);
	enq = true;
	malloc_mutex_unlock(&enq_mtx);

	malloc_mutex_lock(&bt2ctx_mtx);
}

static inline void
prof_leave(void)
{
	bool idump, gdump;

	malloc_mutex_unlock(&bt2ctx_mtx);

	malloc_mutex_lock(&enq_mtx);
	enq = false;
	idump = enq_idump;
	enq_idump = false;
	gdump = enq_gdump;
	enq_gdump = false;
	malloc_mutex_unlock(&enq_mtx);

	if (idump)
		prof_idump();
	if (gdump)
		prof_gdump();
}

#ifdef JEMALLOC_PROF_LIBUNWIND
void
prof_backtrace(prof_bt_t *bt, unsigned nignore, unsigned max)
{
	unw_context_t uc;
	unw_cursor_t cursor;
	unsigned i;
	int err;

	assert(bt->len == 0);
	assert(bt->vec != NULL);
	assert(max <= (1U << opt_lg_prof_bt_max));

	unw_getcontext(&uc);
	unw_init_local(&cursor, &uc);

	/* Throw away (nignore+1) stack frames, if that many exist. */
	for (i = 0; i < nignore + 1; i++) {
		err = unw_step(&cursor);
		if (err <= 0)
			return;
	}

	/*
	 * Iterate over stack frames until there are no more, or until no space
	 * remains in bt.
	 */
	for (i = 0; i < max; i++) {
		unw_get_reg(&cursor, UNW_REG_IP, (unw_word_t *)&bt->vec[i]);
		bt->len++;
		err = unw_step(&cursor);
		if (err <= 0)
			break;
	}
}
#endif
#ifdef JEMALLOC_PROF_LIBGCC
static _Unwind_Reason_Code
prof_unwind_init_callback(struct _Unwind_Context *context, void *arg)
{

	return (_URC_NO_REASON);
}

static _Unwind_Reason_Code
prof_unwind_callback(struct _Unwind_Context *context, void *arg)
{
	prof_unwind_data_t *data = (prof_unwind_data_t *)arg;

	if (data->nignore > 0)
		data->nignore--;
	else {
		data->bt->vec[data->bt->len] = (void *)_Unwind_GetIP(context);
		data->bt->len++;
		if (data->bt->len == data->max)
			return (_URC_END_OF_STACK);
	}

	return (_URC_NO_REASON);
}

void
prof_backtrace(prof_bt_t *bt, unsigned nignore, unsigned max)
{
	prof_unwind_data_t data = {bt, nignore, max};

	_Unwind_Backtrace(prof_unwind_callback, &data);
}
#endif
#ifdef JEMALLOC_PROF_GCC
void
prof_backtrace(prof_bt_t *bt, unsigned nignore, unsigned max)
{
#define	BT_FRAME(i)							\
	if ((i) < nignore + max) {					\
		void *p;						\
		if (__builtin_frame_address(i) == 0)			\
			return;						\
		p = __builtin_return_address(i);			\
		if (p == NULL)						\
			return;						\
		if (i >= nignore) {					\
			bt->vec[(i) - nignore] = p;			\
			bt->len = (i) - nignore + 1;			\
		}							\
	} else								\
		return;

	assert(nignore <= 3);
	assert(max <= (1U << opt_lg_prof_bt_max));

	BT_FRAME(0)
	BT_FRAME(1)
	BT_FRAME(2)
	BT_FRAME(3)
	BT_FRAME(4)
	BT_FRAME(5)
	BT_FRAME(6)
	BT_FRAME(7)
	BT_FRAME(8)
	BT_FRAME(9)

	BT_FRAME(10)
	BT_FRAME(11)
	BT_FRAME(12)
	BT_FRAME(13)
	BT_FRAME(14)
	BT_FRAME(15)
	BT_FRAME(16)
	BT_FRAME(17)
	BT_FRAME(18)
	BT_FRAME(19)

	BT_FRAME(20)
	BT_FRAME(21)
	BT_FRAME(22)
	BT_FRAME(23)
	BT_FRAME(24)
	BT_FRAME(25)
	BT_FRAME(26)
	BT_FRAME(27)
	BT_FRAME(28)
	BT_FRAME(29)

	BT_FRAME(30)
	BT_FRAME(31)
	BT_FRAME(32)
	BT_FRAME(33)
	BT_FRAME(34)
	BT_FRAME(35)
	BT_FRAME(36)
	BT_FRAME(37)
	BT_FRAME(38)
	BT_FRAME(39)

	BT_FRAME(40)
	BT_FRAME(41)
	BT_FRAME(42)
	BT_FRAME(43)
	BT_FRAME(44)
	BT_FRAME(45)
	BT_FRAME(46)
	BT_FRAME(47)
	BT_FRAME(48)
	BT_FRAME(49)

	BT_FRAME(50)
	BT_FRAME(51)
	BT_FRAME(52)
	BT_FRAME(53)
	BT_FRAME(54)
	BT_FRAME(55)
	BT_FRAME(56)
	BT_FRAME(57)
	BT_FRAME(58)
	BT_FRAME(59)

	BT_FRAME(60)
	BT_FRAME(61)
	BT_FRAME(62)
	BT_FRAME(63)
	BT_FRAME(64)
	BT_FRAME(65)
	BT_FRAME(66)
	BT_FRAME(67)
	BT_FRAME(68)
	BT_FRAME(69)

	BT_FRAME(70)
	BT_FRAME(71)
	BT_FRAME(72)
	BT_FRAME(73)
	BT_FRAME(74)
	BT_FRAME(75)
	BT_FRAME(76)
	BT_FRAME(77)
	BT_FRAME(78)
	BT_FRAME(79)

	BT_FRAME(80)
	BT_FRAME(81)
	BT_FRAME(82)
	BT_FRAME(83)
	BT_FRAME(84)
	BT_FRAME(85)
	BT_FRAME(86)
	BT_FRAME(87)
	BT_FRAME(88)
	BT_FRAME(89)

	BT_FRAME(90)
	BT_FRAME(91)
	BT_FRAME(92)
	BT_FRAME(93)
	BT_FRAME(94)
	BT_FRAME(95)
	BT_FRAME(96)
	BT_FRAME(97)
	BT_FRAME(98)
	BT_FRAME(99)

	BT_FRAME(100)
	BT_FRAME(101)
	BT_FRAME(102)
	BT_FRAME(103)
	BT_FRAME(104)
	BT_FRAME(105)
	BT_FRAME(106)
	BT_FRAME(107)
	BT_FRAME(108)
	BT_FRAME(109)

	BT_FRAME(110)
	BT_FRAME(111)
	BT_FRAME(112)
	BT_FRAME(113)
	BT_FRAME(114)
	BT_FRAME(115)
	BT_FRAME(116)
	BT_FRAME(117)
	BT_FRAME(118)
	BT_FRAME(119)

	BT_FRAME(120)
	BT_FRAME(121)
	BT_FRAME(122)
	BT_FRAME(123)
	BT_FRAME(124)
	BT_FRAME(125)
	BT_FRAME(126)
	BT_FRAME(127)

	/* Extras to compensate for nignore. */
	BT_FRAME(128)
	BT_FRAME(129)
	BT_FRAME(130)
#undef BT_FRAME
}
#endif

prof_thr_cnt_t *
prof_lookup(prof_bt_t *bt)
{
	union {
		prof_thr_cnt_t	*p;
		void		*v;
	} ret;
	prof_tdata_t *prof_tdata;

	prof_tdata = PROF_TCACHE_GET();
	if (prof_tdata == NULL) {
		prof_tdata = prof_tdata_init();
		if (prof_tdata == NULL)
			return (NULL);
	}

	if (ckh_search(&prof_tdata->bt2cnt, bt, NULL, &ret.v)) {
		union {
			prof_bt_t	*p;
			void		*v;
		} btkey;
		union {
			prof_ctx_t	*p;
			void		*v;
		} ctx;
		bool new_ctx;

		/*
		 * This thread's cache lacks bt.  Look for it in the global
		 * cache.
		 */
		prof_enter();
		if (ckh_search(&bt2ctx, bt, &btkey.v, &ctx.v)) {
			/* bt has never been seen before.  Insert it. */
			ctx.v = imalloc(sizeof(prof_ctx_t));
			if (ctx.v == NULL) {
				prof_leave();
				return (NULL);
			}
			btkey.p = bt_dup(bt);
			if (btkey.v == NULL) {
				prof_leave();
				idalloc(ctx.v);
				return (NULL);
			}
			ctx.p->bt = btkey.p;
			if (malloc_mutex_init(&ctx.p->lock)) {
				prof_leave();
				idalloc(btkey.v);
				idalloc(ctx.v);
				return (NULL);
			}
			memset(&ctx.p->cnt_merged, 0, sizeof(prof_cnt_t));
			ql_new(&ctx.p->cnts_ql);
			if (ckh_insert(&bt2ctx, btkey.v, ctx.v)) {
				/* OOM. */
				prof_leave();
				malloc_mutex_destroy(&ctx.p->lock);
				idalloc(btkey.v);
				idalloc(ctx.v);
				return (NULL);
			}
			/*
			 * Artificially raise curobjs, in order to avoid a race
			 * condition with prof_ctx_merge()/prof_ctx_destroy().
			 *
			 * No locking is necessary for ctx here because no other
			 * threads have had the opportunity to fetch it from
			 * bt2ctx yet.
			 */
			ctx.p->cnt_merged.curobjs++;
			new_ctx = true;
		} else {
			/*
			 * Artificially raise curobjs, in order to avoid a race
			 * condition with prof_ctx_merge()/prof_ctx_destroy().
			 */
			malloc_mutex_lock(&ctx.p->lock);
			ctx.p->cnt_merged.curobjs++;
			malloc_mutex_unlock(&ctx.p->lock);
			new_ctx = false;
		}
		prof_leave();

		/* Link a prof_thd_cnt_t into ctx for this thread. */
		if (opt_lg_prof_tcmax >= 0 && ckh_count(&prof_tdata->bt2cnt)
		    == (ZU(1) << opt_lg_prof_tcmax)) {
			assert(ckh_count(&prof_tdata->bt2cnt) > 0);
			/*
			 * Flush the least recently used cnt in order to keep
			 * bt2cnt from becoming too large.
			 */
			ret.p = ql_last(&prof_tdata->lru_ql, lru_link);
			assert(ret.v != NULL);
			if (ckh_remove(&prof_tdata->bt2cnt, ret.p->ctx->bt,
			    NULL, NULL))
				assert(false);
			ql_remove(&prof_tdata->lru_ql, ret.p, lru_link);
			prof_ctx_merge(ret.p->ctx, ret.p);
			/* ret can now be re-used. */
		} else {
			assert(opt_lg_prof_tcmax < 0 ||
			    ckh_count(&prof_tdata->bt2cnt) < (ZU(1) <<
			    opt_lg_prof_tcmax));
			/* Allocate and partially initialize a new cnt. */
			ret.v = imalloc(sizeof(prof_thr_cnt_t));
			if (ret.p == NULL) {
				if (new_ctx)
					prof_ctx_destroy(ctx.p);
				return (NULL);
			}
			ql_elm_new(ret.p, cnts_link);
			ql_elm_new(ret.p, lru_link);
		}
		/* Finish initializing ret. */
		ret.p->ctx = ctx.p;
		ret.p->epoch = 0;
		memset(&ret.p->cnts, 0, sizeof(prof_cnt_t));
		if (ckh_insert(&prof_tdata->bt2cnt, btkey.v, ret.v)) {
			if (new_ctx)
				prof_ctx_destroy(ctx.p);
			idalloc(ret.v);
			return (NULL);
		}
		ql_head_insert(&prof_tdata->lru_ql, ret.p, lru_link);
		malloc_mutex_lock(&ctx.p->lock);
		ql_tail_insert(&ctx.p->cnts_ql, ret.p, cnts_link);
		ctx.p->cnt_merged.curobjs--;
		malloc_mutex_unlock(&ctx.p->lock);
	} else {
		/* Move ret to the front of the LRU. */
		ql_remove(&prof_tdata->lru_ql, ret.p, lru_link);
		ql_head_insert(&prof_tdata->lru_ql, ret.p, lru_link);
	}

	return (ret.p);
}

static bool
prof_flush(bool propagate_err)
{
	bool ret = false;
	ssize_t err;

	err = write(prof_dump_fd, prof_dump_buf, prof_dump_buf_end);
	if (err == -1) {
		if (propagate_err == false) {
			malloc_write("<jemalloc>: write() failed during heap "
			    "profile flush\n");
			if (opt_abort)
				abort();
		}
		ret = true;
	}
	prof_dump_buf_end = 0;

	return (ret);
}

static bool
prof_write(const char *s, bool propagate_err)
{
	unsigned i, slen, n;

	i = 0;
	slen = strlen(s);
	while (i < slen) {
		/* Flush the buffer if it is full. */
		if (prof_dump_buf_end == PROF_DUMP_BUF_SIZE)
			if (prof_flush(propagate_err) && propagate_err)
				return (true);

		if (prof_dump_buf_end + slen <= PROF_DUMP_BUF_SIZE) {
			/* Finish writing. */
			n = slen - i;
		} else {
			/* Write as much of s as will fit. */
			n = PROF_DUMP_BUF_SIZE - prof_dump_buf_end;
		}
		memcpy(&prof_dump_buf[prof_dump_buf_end], &s[i], n);
		prof_dump_buf_end += n;
		i += n;
	}

	return (false);
}

static void
prof_ctx_sum(prof_ctx_t *ctx, prof_cnt_t *cnt_all, size_t *leak_nctx)
{
	prof_thr_cnt_t *thr_cnt;
	prof_cnt_t tcnt;

	malloc_mutex_lock(&ctx->lock);

	memcpy(&ctx->cnt_summed, &ctx->cnt_merged, sizeof(prof_cnt_t));
	ql_foreach(thr_cnt, &ctx->cnts_ql, cnts_link) {
		volatile unsigned *epoch = &thr_cnt->epoch;

		while (true) {
			unsigned epoch0 = *epoch;

			/* Make sure epoch is even. */
			if (epoch0 & 1U)
				continue;

			memcpy(&tcnt, &thr_cnt->cnts, sizeof(prof_cnt_t));

			/* Terminate if epoch didn't change while reading. */
			if (*epoch == epoch0)
				break;
		}

		ctx->cnt_summed.curobjs += tcnt.curobjs;
		ctx->cnt_summed.curbytes += tcnt.curbytes;
		if (opt_prof_accum) {
			ctx->cnt_summed.accumobjs += tcnt.accumobjs;
			ctx->cnt_summed.accumbytes += tcnt.accumbytes;
		}
	}

	if (ctx->cnt_summed.curobjs != 0)
		(*leak_nctx)++;

	/* Add to cnt_all. */
	cnt_all->curobjs += ctx->cnt_summed.curobjs;
	cnt_all->curbytes += ctx->cnt_summed.curbytes;
	if (opt_prof_accum) {
		cnt_all->accumobjs += ctx->cnt_summed.accumobjs;
		cnt_all->accumbytes += ctx->cnt_summed.accumbytes;
	}

	malloc_mutex_unlock(&ctx->lock);
}

static void
prof_ctx_destroy(prof_ctx_t *ctx)
{

	/*
	 * Check that ctx is still unused by any thread cache before destroying
	 * it.  prof_lookup() artificially raises ctx->cnt_merge.curobjs in
	 * order to avoid a race condition with this function, as does
	 * prof_ctx_merge() in order to avoid a race between the main body of
	 * prof_ctx_merge() and entry into this function.
	 */
	prof_enter();
	malloc_mutex_lock(&ctx->lock);
	if (ql_first(&ctx->cnts_ql) == NULL && ctx->cnt_merged.curobjs == 1) {
		assert(ctx->cnt_merged.curbytes == 0);
		assert(ctx->cnt_merged.accumobjs == 0);
		assert(ctx->cnt_merged.accumbytes == 0);
		/* Remove ctx from bt2ctx. */
		if (ckh_remove(&bt2ctx, ctx->bt, NULL, NULL))
			assert(false);
		prof_leave();
		/* Destroy ctx. */
		malloc_mutex_unlock(&ctx->lock);
		bt_destroy(ctx->bt);
		malloc_mutex_destroy(&ctx->lock);
		idalloc(ctx);
	} else {
		/*
		 * Compensate for increment in prof_ctx_merge() or
		 * prof_lookup().
		 */
		ctx->cnt_merged.curobjs--;
		malloc_mutex_unlock(&ctx->lock);
		prof_leave();
	}
}

static void
prof_ctx_merge(prof_ctx_t *ctx, prof_thr_cnt_t *cnt)
{
	bool destroy;

	/* Merge cnt stats and detach from ctx. */
	malloc_mutex_lock(&ctx->lock);
	ctx->cnt_merged.curobjs += cnt->cnts.curobjs;
	ctx->cnt_merged.curbytes += cnt->cnts.curbytes;
	ctx->cnt_merged.accumobjs += cnt->cnts.accumobjs;
	ctx->cnt_merged.accumbytes += cnt->cnts.accumbytes;
	ql_remove(&ctx->cnts_ql, cnt, cnts_link);
	if (opt_prof_accum == false && ql_first(&ctx->cnts_ql) == NULL &&
	    ctx->cnt_merged.curobjs == 0) {
		/*
		 * Artificially raise ctx->cnt_merged.curobjs in order to keep
		 * another thread from winning the race to destroy ctx while
		 * this one has ctx->lock dropped.  Without this, it would be
		 * possible for another thread to:
		 *
		 * 1) Sample an allocation associated with ctx.
		 * 2) Deallocate the sampled object.
		 * 3) Successfully prof_ctx_destroy(ctx).
		 *
		 * The result would be that ctx no longer exists by the time
		 * this thread accesses it in prof_ctx_destroy().
		 */
		ctx->cnt_merged.curobjs++;
		destroy = true;
	} else
		destroy = false;
	malloc_mutex_unlock(&ctx->lock);
	if (destroy)
		prof_ctx_destroy(ctx);
}

static bool
prof_dump_ctx(prof_ctx_t *ctx, prof_bt_t *bt, bool propagate_err)
{
	char buf[UMAX2S_BUFSIZE];
	unsigned i;

	if (opt_prof_accum == false && ctx->cnt_summed.curobjs == 0) {
		assert(ctx->cnt_summed.curbytes == 0);
		assert(ctx->cnt_summed.accumobjs == 0);
		assert(ctx->cnt_summed.accumbytes == 0);
		return (false);
	}

	if (prof_write(u2s(ctx->cnt_summed.curobjs, 10, buf), propagate_err)
	    || prof_write(": ", propagate_err)
	    || prof_write(u2s(ctx->cnt_summed.curbytes, 10, buf),
	    propagate_err)
	    || prof_write(" [", propagate_err)
	    || prof_write(u2s(ctx->cnt_summed.accumobjs, 10, buf),
	    propagate_err)
	    || prof_write(": ", propagate_err)
	    || prof_write(u2s(ctx->cnt_summed.accumbytes, 10, buf),
	    propagate_err)
	    || prof_write("] @", propagate_err))
		return (true);

	for (i = 0; i < bt->len; i++) {
		if (prof_write(" 0x", propagate_err)
		    || prof_write(u2s((uintptr_t)bt->vec[i], 16, buf),
		    propagate_err))
			return (true);
	}

	if (prof_write("\n", propagate_err))
		return (true);

	return (false);
}

static bool
prof_dump_maps(bool propagate_err)
{
	int mfd;
	char buf[UMAX2S_BUFSIZE];
	char *s;
	unsigned i, slen;
	/*         /proc/<pid>/maps\0 */
	char mpath[6     + UMAX2S_BUFSIZE
			      + 5  + 1];

	i = 0;

	s = "/proc/";
	slen = strlen(s);
	memcpy(&mpath[i], s, slen);
	i += slen;

	s = u2s(getpid(), 10, buf);
	slen = strlen(s);
	memcpy(&mpath[i], s, slen);
	i += slen;

	s = "/maps";
	slen = strlen(s);
	memcpy(&mpath[i], s, slen);
	i += slen;

	mpath[i] = '\0';

	mfd = open(mpath, O_RDONLY);
	if (mfd != -1) {
		ssize_t nread;

		if (prof_write("\nMAPPED_LIBRARIES:\n", propagate_err) &&
		    propagate_err)
			return (true);
		nread = 0;
		do {
			prof_dump_buf_end += nread;
			if (prof_dump_buf_end == PROF_DUMP_BUF_SIZE) {
				/* Make space in prof_dump_buf before read(). */
				if (prof_flush(propagate_err) && propagate_err)
					return (true);
			}
			nread = read(mfd, &prof_dump_buf[prof_dump_buf_end],
			    PROF_DUMP_BUF_SIZE - prof_dump_buf_end);
		} while (nread > 0);
		close(mfd);
	} else
		return (true);

	return (false);
}

static bool
prof_dump(const char *filename, bool leakcheck, bool propagate_err)
{
	prof_cnt_t cnt_all;
	size_t tabind;
	union {
		prof_bt_t	*p;
		void		*v;
	} bt;
	union {
		prof_ctx_t	*p;
		void		*v;
	} ctx;
	char buf[UMAX2S_BUFSIZE];
	size_t leak_nctx;

	prof_enter();
	prof_dump_fd = creat(filename, 0644);
	if (prof_dump_fd == -1) {
		if (propagate_err == false) {
			malloc_write("<jemalloc>: creat(\"");
			malloc_write(filename);
			malloc_write("\", 0644) failed\n");
			if (opt_abort)
				abort();
		}
		goto ERROR;
	}

	/* Merge per thread profile stats, and sum them in cnt_all. */
	memset(&cnt_all, 0, sizeof(prof_cnt_t));
	leak_nctx = 0;
	for (tabind = 0; ckh_iter(&bt2ctx, &tabind, NULL, &ctx.v) == false;)
		prof_ctx_sum(ctx.p, &cnt_all, &leak_nctx);

	/* Dump profile header. */
	if (prof_write("heap profile: ", propagate_err)
	    || prof_write(u2s(cnt_all.curobjs, 10, buf), propagate_err)
	    || prof_write(": ", propagate_err)
	    || prof_write(u2s(cnt_all.curbytes, 10, buf), propagate_err)
	    || prof_write(" [", propagate_err)
	    || prof_write(u2s(cnt_all.accumobjs, 10, buf), propagate_err)
	    || prof_write(": ", propagate_err)
	    || prof_write(u2s(cnt_all.accumbytes, 10, buf), propagate_err))
		goto ERROR;

	if (opt_lg_prof_sample == 0) {
		if (prof_write("] @ heapprofile\n", propagate_err))
			goto ERROR;
	} else {
		if (prof_write("] @ heap_v2/", propagate_err)
		    || prof_write(u2s((uint64_t)1U << opt_lg_prof_sample, 10,
		    buf), propagate_err)
		    || prof_write("\n", propagate_err))
			goto ERROR;
	}

	/* Dump  per ctx profile stats. */
	for (tabind = 0; ckh_iter(&bt2ctx, &tabind, &bt.v, &ctx.v)
	    == false;) {
		if (prof_dump_ctx(ctx.p, bt.p, propagate_err))
			goto ERROR;
	}

	/* Dump /proc/<pid>/maps if possible. */
	if (prof_dump_maps(propagate_err))
		goto ERROR;

	if (prof_flush(propagate_err))
		goto ERROR;
	close(prof_dump_fd);
	prof_leave();

	if (leakcheck && cnt_all.curbytes != 0) {
		malloc_write("<jemalloc>: Leak summary: ");
		malloc_write(u2s(cnt_all.curbytes, 10, buf));
		malloc_write((cnt_all.curbytes != 1) ? " bytes, " : " byte, ");
		malloc_write(u2s(cnt_all.curobjs, 10, buf));
		malloc_write((cnt_all.curobjs != 1) ? " objects, " :
		    " object, ");
		malloc_write(u2s(leak_nctx, 10, buf));
		malloc_write((leak_nctx != 1) ? " contexts\n" : " context\n");
		malloc_write("<jemalloc>: Run pprof on \"");
		malloc_write(filename);
		malloc_write("\" for leak detail\n");
	}

	return (false);
ERROR:
	prof_leave();
	return (true);
}

#define	DUMP_FILENAME_BUFSIZE	(PATH_MAX+ UMAX2S_BUFSIZE		\
					       + 1			\
						+ UMAX2S_BUFSIZE	\
						     + 2		\
						       + UMAX2S_BUFSIZE	\
						             + 5  + 1)
static void
prof_dump_filename(char *filename, char v, int64_t vseq)
{
	char buf[UMAX2S_BUFSIZE];
	char *s;
	unsigned i, slen;

	/*
	 * Construct a filename of the form:
	 *
	 *   <prefix>.<pid>.<seq>.v<vseq>.heap\0
	 */

	i = 0;

	s = opt_prof_prefix;
	slen = strlen(s);
	memcpy(&filename[i], s, slen);
	i += slen;

	s = ".";
	slen = strlen(s);
	memcpy(&filename[i], s, slen);
	i += slen;

	s = u2s(getpid(), 10, buf);
	slen = strlen(s);
	memcpy(&filename[i], s, slen);
	i += slen;

	s = ".";
	slen = strlen(s);
	memcpy(&filename[i], s, slen);
	i += slen;

	s = u2s(prof_dump_seq, 10, buf);
	prof_dump_seq++;
	slen = strlen(s);
	memcpy(&filename[i], s, slen);
	i += slen;

	s = ".";
	slen = strlen(s);
	memcpy(&filename[i], s, slen);
	i += slen;

	filename[i] = v;
	i++;

	if (vseq != 0xffffffffffffffffLLU) {
		s = u2s(vseq, 10, buf);
		slen = strlen(s);
		memcpy(&filename[i], s, slen);
		i += slen;
	}

	s = ".heap";
	slen = strlen(s);
	memcpy(&filename[i], s, slen);
	i += slen;

	filename[i] = '\0';
}

static void
prof_fdump(void)
{
	char filename[DUMP_FILENAME_BUFSIZE];

	if (prof_booted == false)
		return;

	if (opt_prof_prefix[0] != '\0') {
		malloc_mutex_lock(&prof_dump_seq_mtx);
		prof_dump_filename(filename, 'f', 0xffffffffffffffffLLU);
		malloc_mutex_unlock(&prof_dump_seq_mtx);
		prof_dump(filename, opt_prof_leak, false);
	}
}

void
prof_idump(void)
{
	char filename[DUMP_FILENAME_BUFSIZE];

	if (prof_booted == false)
		return;
	malloc_mutex_lock(&enq_mtx);
	if (enq) {
		enq_idump = true;
		malloc_mutex_unlock(&enq_mtx);
		return;
	}
	malloc_mutex_unlock(&enq_mtx);

	if (opt_prof_prefix[0] != '\0') {
		malloc_mutex_lock(&prof_dump_seq_mtx);
		prof_dump_filename(filename, 'i', prof_dump_iseq);
		prof_dump_iseq++;
		malloc_mutex_unlock(&prof_dump_seq_mtx);
		prof_dump(filename, false, false);
	}
}

bool
prof_mdump(const char *filename)
{
	char filename_buf[DUMP_FILENAME_BUFSIZE];

	if (opt_prof == false || prof_booted == false)
		return (true);

	if (filename == NULL) {
		/* No filename specified, so automatically generate one. */
		if (opt_prof_prefix[0] == '\0')
			return (true);
		malloc_mutex_lock(&prof_dump_seq_mtx);
		prof_dump_filename(filename_buf, 'm', prof_dump_mseq);
		prof_dump_mseq++;
		malloc_mutex_unlock(&prof_dump_seq_mtx);
		filename = filename_buf;
	}
	return (prof_dump(filename, false, true));
}

void
prof_gdump(void)
{
	char filename[DUMP_FILENAME_BUFSIZE];

	if (prof_booted == false)
		return;
	malloc_mutex_lock(&enq_mtx);
	if (enq) {
		enq_gdump = true;
		malloc_mutex_unlock(&enq_mtx);
		return;
	}
	malloc_mutex_unlock(&enq_mtx);

	if (opt_prof_prefix[0] != '\0') {
		malloc_mutex_lock(&prof_dump_seq_mtx);
		prof_dump_filename(filename, 'u', prof_dump_useq);
		prof_dump_useq++;
		malloc_mutex_unlock(&prof_dump_seq_mtx);
		prof_dump(filename, false, false);
	}
}

static void
prof_bt_hash(const void *key, unsigned minbits, size_t *hash1, size_t *hash2)
{
	size_t ret1, ret2;
	uint64_t h;
	prof_bt_t *bt = (prof_bt_t *)key;

	assert(minbits <= 32 || (SIZEOF_PTR == 8 && minbits <= 64));
	assert(hash1 != NULL);
	assert(hash2 != NULL);

	h = hash(bt->vec, bt->len * sizeof(void *), 0x94122f335b332aeaLLU);
	if (minbits <= 32) {
		/*
		 * Avoid doing multiple hashes, since a single hash provides
		 * enough bits.
		 */
		ret1 = h & ZU(0xffffffffU);
		ret2 = h >> 32;
	} else {
		ret1 = h;
		ret2 = hash(bt->vec, bt->len * sizeof(void *),
		    0x8432a476666bbc13LLU);
	}

	*hash1 = ret1;
	*hash2 = ret2;
}

static bool
prof_bt_keycomp(const void *k1, const void *k2)
{
	const prof_bt_t *bt1 = (prof_bt_t *)k1;
	const prof_bt_t *bt2 = (prof_bt_t *)k2;

	if (bt1->len != bt2->len)
		return (false);
	return (memcmp(bt1->vec, bt2->vec, bt1->len * sizeof(void *)) == 0);
}

prof_tdata_t *
prof_tdata_init(void)
{
	prof_tdata_t *prof_tdata;

	/* Initialize an empty cache for this thread. */
	prof_tdata = (prof_tdata_t *)imalloc(sizeof(prof_tdata_t));
	if (prof_tdata == NULL)
		return (NULL);

	if (ckh_new(&prof_tdata->bt2cnt, PROF_CKH_MINITEMS,
	    prof_bt_hash, prof_bt_keycomp)) {
		idalloc(prof_tdata);
		return (NULL);
	}
	ql_new(&prof_tdata->lru_ql);

	prof_tdata->vec = imalloc(sizeof(void *) * prof_bt_max);
	if (prof_tdata->vec == NULL) {
		ckh_delete(&prof_tdata->bt2cnt);
		idalloc(prof_tdata);
		return (NULL);
	}

	prof_tdata->prn_state = 0;
	prof_tdata->threshold = 0;
	prof_tdata->accum = 0;

	PROF_TCACHE_SET(prof_tdata);

	return (prof_tdata);
}

static void
prof_tdata_cleanup(void *arg)
{
	prof_thr_cnt_t *cnt;
	prof_tdata_t *prof_tdata = (prof_tdata_t *)arg;

	/*
	 * Delete the hash table.  All of its contents can still be iterated
	 * over via the LRU.
	 */
	ckh_delete(&prof_tdata->bt2cnt);

	/* Iteratively merge cnt's into the global stats and delete them. */
	while ((cnt = ql_last(&prof_tdata->lru_ql, lru_link)) != NULL) {
		ql_remove(&prof_tdata->lru_ql, cnt, lru_link);
		prof_ctx_merge(cnt->ctx, cnt);
		idalloc(cnt);
	}

	idalloc(prof_tdata->vec);

	idalloc(prof_tdata);
	PROF_TCACHE_SET(NULL);
}

void
prof_boot0(void)
{

	memcpy(opt_prof_prefix, PROF_PREFIX_DEFAULT,
	    sizeof(PROF_PREFIX_DEFAULT));
}

void
prof_boot1(void)
{

	/*
	 * opt_prof and prof_promote must be in their final state before any
	 * arenas are initialized, so this function must be executed early.
	 */

	if (opt_prof_leak && opt_prof == false) {
		/*
		 * Enable opt_prof, but in such a way that profiles are never
		 * automatically dumped.
		 */
		opt_prof = true;
		opt_prof_gdump = false;
		prof_interval = 0;
	} else if (opt_prof) {
		if (opt_lg_prof_interval >= 0) {
			prof_interval = (((uint64_t)1U) <<
			    opt_lg_prof_interval);
		} else
			prof_interval = 0;
	}

	prof_promote = (opt_prof && opt_lg_prof_sample > PAGE_SHIFT);
}

bool
prof_boot2(void)
{

	if (opt_prof) {
		if (ckh_new(&bt2ctx, PROF_CKH_MINITEMS, prof_bt_hash,
		    prof_bt_keycomp))
			return (true);
		if (malloc_mutex_init(&bt2ctx_mtx))
			return (true);
		if (pthread_key_create(&prof_tdata_tsd, prof_tdata_cleanup)
		    != 0) {
			malloc_write(
			    "<jemalloc>: Error in pthread_key_create()\n");
			abort();
		}

		prof_bt_max = (1U << opt_lg_prof_bt_max);
		if (malloc_mutex_init(&prof_dump_seq_mtx))
			return (true);

		if (malloc_mutex_init(&enq_mtx))
			return (true);
		enq = false;
		enq_idump = false;
		enq_gdump = false;

		if (atexit(prof_fdump) != 0) {
			malloc_write("<jemalloc>: Error in atexit()\n");
			if (opt_abort)
				abort();
		}
	}

#ifdef JEMALLOC_PROF_LIBGCC
	/*
	 * Cause the backtracing machinery to allocate its internal state
	 * before enabling profiling.
	 */
	_Unwind_Backtrace(prof_unwind_init_callback, NULL);
#endif

	prof_booted = true;

	return (false);
}

/******************************************************************************/
#endif /* JEMALLOC_PROF */
