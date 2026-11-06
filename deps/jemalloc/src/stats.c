#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/ctl.h"
#include "jemalloc/internal/emitter.h"
#include "jemalloc/internal/fxp.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/mutex_prof.h"
#include "jemalloc/internal/prof_stats.h"

const char *global_mutex_names[mutex_prof_num_global_mutexes] = {
#define OP(mtx) #mtx,
	MUTEX_PROF_GLOBAL_MUTEXES
#undef OP
};

const char *arena_mutex_names[mutex_prof_num_arena_mutexes] = {
#define OP(mtx) #mtx,
	MUTEX_PROF_ARENA_MUTEXES
#undef OP
};

#define CTL_GET(n, v, t) do {						\
	size_t sz = sizeof(t);						\
	xmallctl(n, (void *)v, &sz, NULL, 0);				\
} while (0)

#define CTL_LEAF_PREPARE(mib, miblen, name) do {			\
	assert(miblen < CTL_MAX_DEPTH);					\
	size_t miblen_new = CTL_MAX_DEPTH;				\
	xmallctlmibnametomib(mib, miblen, name, &miblen_new);		\
	assert(miblen_new > miblen);					\
} while (0)

#define CTL_LEAF(mib, miblen, leaf, v, t) do {			\
	assert(miblen < CTL_MAX_DEPTH);					\
	size_t miblen_new = CTL_MAX_DEPTH;				\
	size_t sz = sizeof(t);						\
	xmallctlbymibname(mib, miblen, leaf, &miblen_new, (void *)v,	\
	    &sz, NULL, 0);						\
	assert(miblen_new == miblen + 1);				\
} while (0)

#define CTL_M2_GET(n, i, v, t) do {					\
	size_t mib[CTL_MAX_DEPTH];					\
	size_t miblen = sizeof(mib) / sizeof(size_t);			\
	size_t sz = sizeof(t);						\
	xmallctlnametomib(n, mib, &miblen);				\
	mib[2] = (i);							\
	xmallctlbymib(mib, miblen, (void *)v, &sz, NULL, 0);		\
} while (0)

/******************************************************************************/
/* Data. */

bool opt_stats_print = false;
char opt_stats_print_opts[stats_print_tot_num_options+1] = "";

int64_t opt_stats_interval = STATS_INTERVAL_DEFAULT;
char opt_stats_interval_opts[stats_print_tot_num_options+1] = "";

static counter_accum_t stats_interval_accumulated;
/* Per thread batch accum size for stats_interval. */
static uint64_t stats_interval_accum_batch;

/******************************************************************************/

static uint64_t
rate_per_second(uint64_t value, uint64_t uptime_ns) {
	uint64_t billion = 1000000000;
	if (uptime_ns == 0 || value == 0) {
		return 0;
	}
	if (uptime_ns < billion) {
		return value;
	} else {
		uint64_t uptime_s = uptime_ns / billion;
		return value / uptime_s;
	}
}

/* Calculate x.yyy and output a string (takes a fixed sized char array). */
static bool
get_rate_str(uint64_t dividend, uint64_t divisor, char str[6]) {
	if (divisor == 0 || dividend > divisor) {
		/* The rate is not supposed to be greater than 1. */
		return true;
	}
	if (dividend > 0) {
		assert(UINT64_MAX / dividend >= 1000);
	}

	unsigned n = (unsigned)((dividend * 1000) / divisor);
	if (n < 10) {
		malloc_snprintf(str, 6, "0.00%u", n);
	} else if (n < 100) {
		malloc_snprintf(str, 6, "0.0%u", n);
	} else if (n < 1000) {
		malloc_snprintf(str, 6, "0.%u", n);
	} else {
		malloc_snprintf(str, 6, "1");
	}

	return false;
}

static void
mutex_stats_init_cols(emitter_row_t *row, const char *table_name,
    emitter_col_t *name,
    emitter_col_t col_uint64_t[mutex_prof_num_uint64_t_counters],
    emitter_col_t col_uint32_t[mutex_prof_num_uint32_t_counters]) {
	mutex_prof_uint64_t_counter_ind_t k_uint64_t = 0;
	mutex_prof_uint32_t_counter_ind_t k_uint32_t = 0;

	emitter_col_t *col;

	if (name != NULL) {
		emitter_col_init(name, row);
		name->justify = emitter_justify_left;
		name->width = 21;
		name->type = emitter_type_title;
		name->str_val = table_name;
	}

#define WIDTH_uint32_t 12
#define WIDTH_uint64_t 16
#define OP(counter, counter_type, human, derived, base_counter)		\
	col = &col_##counter_type[k_##counter_type];			\
	++k_##counter_type;						\
	emitter_col_init(col, row);					\
	col->justify = emitter_justify_right;				\
	col->width = derived ? 8 : WIDTH_##counter_type;		\
	col->type = emitter_type_title;					\
	col->str_val = human;
	MUTEX_PROF_COUNTERS
#undef OP
#undef WIDTH_uint32_t
#undef WIDTH_uint64_t
	col_uint64_t[mutex_counter_total_wait_time_ps].width = 10;
}

static void
mutex_stats_read_global(size_t mib[], size_t miblen, const char *name,
    emitter_col_t *col_name,
    emitter_col_t col_uint64_t[mutex_prof_num_uint64_t_counters],
    emitter_col_t col_uint32_t[mutex_prof_num_uint32_t_counters],
    uint64_t uptime) {
	CTL_LEAF_PREPARE(mib, miblen, name);
	size_t miblen_name = miblen + 1;

	col_name->str_val = name;

	emitter_col_t *dst;
#define EMITTER_TYPE_uint32_t emitter_type_uint32
#define EMITTER_TYPE_uint64_t emitter_type_uint64
#define OP(counter, counter_type, human, derived, base_counter)		\
	dst = &col_##counter_type[mutex_counter_##counter];		\
	dst->type = EMITTER_TYPE_##counter_type;			\
	if (!derived) {							\
		CTL_LEAF(mib, miblen_name, #counter,			\
		    (counter_type *)&dst->bool_val, counter_type);	\
	} else {							\
		emitter_col_t *base =					\
		    &col_##counter_type[mutex_counter_##base_counter];	\
		dst->counter_type##_val =				\
		    (counter_type)rate_per_second(			\
		    base->counter_type##_val, uptime);			\
	}
	MUTEX_PROF_COUNTERS
#undef OP
#undef EMITTER_TYPE_uint32_t
#undef EMITTER_TYPE_uint64_t
}

static void
mutex_stats_read_arena(size_t mib[], size_t miblen, const char *name,
    emitter_col_t *col_name,
    emitter_col_t col_uint64_t[mutex_prof_num_uint64_t_counters],
    emitter_col_t col_uint32_t[mutex_prof_num_uint32_t_counters],
    uint64_t uptime) {
	CTL_LEAF_PREPARE(mib, miblen, name);
	size_t miblen_name = miblen + 1;

	col_name->str_val = name;

	emitter_col_t *dst;
#define EMITTER_TYPE_uint32_t emitter_type_uint32
#define EMITTER_TYPE_uint64_t emitter_type_uint64
#define OP(counter, counter_type, human, derived, base_counter)		\
	dst = &col_##counter_type[mutex_counter_##counter];		\
	dst->type = EMITTER_TYPE_##counter_type;			\
	if (!derived) {							\
		CTL_LEAF(mib, miblen_name, #counter,			\
		    (counter_type *)&dst->bool_val, counter_type);	\
	} else {							\
		emitter_col_t *base =					\
		    &col_##counter_type[mutex_counter_##base_counter];	\
		dst->counter_type##_val =				\
		    (counter_type)rate_per_second(			\
		    base->counter_type##_val, uptime);			\
	}
	MUTEX_PROF_COUNTERS
#undef OP
#undef EMITTER_TYPE_uint32_t
#undef EMITTER_TYPE_uint64_t
}

static void
mutex_stats_read_arena_bin(size_t mib[], size_t miblen,
    emitter_col_t col_uint64_t[mutex_prof_num_uint64_t_counters],
    emitter_col_t col_uint32_t[mutex_prof_num_uint32_t_counters],
    uint64_t uptime) {
	CTL_LEAF_PREPARE(mib, miblen, "mutex");
	size_t miblen_mutex = miblen + 1;

	emitter_col_t *dst;

#define EMITTER_TYPE_uint32_t emitter_type_uint32
#define EMITTER_TYPE_uint64_t emitter_type_uint64
#define OP(counter, counter_type, human, derived, base_counter)		\
	dst = &col_##counter_type[mutex_counter_##counter];		\
	dst->type = EMITTER_TYPE_##counter_type;			\
	if (!derived) {							\
		CTL_LEAF(mib, miblen_mutex, #counter,			\
		    (counter_type *)&dst->bool_val, counter_type);	\
	} else {							\
		emitter_col_t *base =					\
		    &col_##counter_type[mutex_counter_##base_counter];	\
		dst->counter_type##_val =				\
		    (counter_type)rate_per_second(			\
		    base->counter_type##_val, uptime);			\
	}
	MUTEX_PROF_COUNTERS
#undef OP
#undef EMITTER_TYPE_uint32_t
#undef EMITTER_TYPE_uint64_t
}

/* "row" can be NULL to avoid emitting in table mode. */
static void
mutex_stats_emit(emitter_t *emitter, emitter_row_t *row,
    emitter_col_t col_uint64_t[mutex_prof_num_uint64_t_counters],
    emitter_col_t col_uint32_t[mutex_prof_num_uint32_t_counters]) {
	if (row != NULL) {
		emitter_table_row(emitter, row);
	}

	mutex_prof_uint64_t_counter_ind_t k_uint64_t = 0;
	mutex_prof_uint32_t_counter_ind_t k_uint32_t = 0;

	emitter_col_t *col;

#define EMITTER_TYPE_uint32_t emitter_type_uint32
#define EMITTER_TYPE_uint64_t emitter_type_uint64
#define OP(counter, type, human, derived, base_counter)		\
	if (!derived) {                    \
		col = &col_##type[k_##type];                        \
		++k_##type;                            \
		emitter_json_kv(emitter, #counter, EMITTER_TYPE_##type,        \
		    (const void *)&col->bool_val); \
	}
	MUTEX_PROF_COUNTERS;
#undef OP
#undef EMITTER_TYPE_uint32_t
#undef EMITTER_TYPE_uint64_t
}

#define COL_DECLARE(column_name)					\
	emitter_col_t col_##column_name;

#define COL_INIT(row_name, column_name, left_or_right, col_width, etype)\
	emitter_col_init(&col_##column_name, &row_name);		\
	col_##column_name.justify = emitter_justify_##left_or_right;	\
	col_##column_name.width = col_width;				\
	col_##column_name.type = emitter_type_##etype;

#define COL(row_name, column_name, left_or_right, col_width, etype)	\
	COL_DECLARE(column_name);					\
	COL_INIT(row_name, column_name, left_or_right, col_width, etype)

#define COL_HDR_DECLARE(column_name)					\
	COL_DECLARE(column_name);					\
	emitter_col_t header_##column_name;

#define COL_HDR_INIT(row_name, column_name, human, left_or_right,	\
	col_width, etype)						\
	COL_INIT(row_name, column_name, left_or_right, col_width, etype)\
	emitter_col_init(&header_##column_name, &header_##row_name);	\
	header_##column_name.justify = emitter_justify_##left_or_right;	\
	header_##column_name.width = col_width;				\
	header_##column_name.type = emitter_type_title;			\
	header_##column_name.str_val = human ? human : #column_name;

#define COL_HDR(row_name, column_name, human, left_or_right, col_width,	\
    etype)								\
	COL_HDR_DECLARE(column_name)					\
	COL_HDR_INIT(row_name, column_name, human, left_or_right,	\
	    col_width, etype)

JEMALLOC_COLD
static void
stats_arena_bins_print(emitter_t *emitter, bool mutex, unsigned i,
    uint64_t uptime) {
	size_t page;
	bool in_gap, in_gap_prev;
	unsigned nbins, j;

	CTL_GET("arenas.page", &page, size_t);

	CTL_GET("arenas.nbins", &nbins, unsigned);

	emitter_row_t header_row;
	emitter_row_init(&header_row);

	emitter_row_t row;
	emitter_row_init(&row);

	bool prof_stats_on = config_prof && opt_prof && opt_prof_stats
	    && i == MALLCTL_ARENAS_ALL;

	COL_HDR(row, size, NULL, right, 20, size)
	COL_HDR(row, ind, NULL, right, 4, unsigned)
	COL_HDR(row, allocated, NULL, right, 13, uint64)
	COL_HDR(row, nmalloc, NULL, right, 13, uint64)
	COL_HDR(row, nmalloc_ps, "(#/sec)", right, 8, uint64)
	COL_HDR(row, ndalloc, NULL, right, 13, uint64)
	COL_HDR(row, ndalloc_ps, "(#/sec)", right, 8, uint64)
	COL_HDR(row, nrequests, NULL, right, 13, uint64)
	COL_HDR(row, nrequests_ps, "(#/sec)", right, 10, uint64)
	COL_HDR_DECLARE(prof_live_requested);
	COL_HDR_DECLARE(prof_live_count);
	COL_HDR_DECLARE(prof_accum_requested);
	COL_HDR_DECLARE(prof_accum_count);
	if (prof_stats_on) {
		COL_HDR_INIT(row, prof_live_requested, NULL, right, 21, uint64)
		COL_HDR_INIT(row, prof_live_count, NULL, right, 17, uint64)
		COL_HDR_INIT(row, prof_accum_requested, NULL, right, 21, uint64)
		COL_HDR_INIT(row, prof_accum_count, NULL, right, 17, uint64)
	}
	COL_HDR(row, nshards, NULL, right, 9, unsigned)
	COL_HDR(row, curregs, NULL, right, 13, size)
	COL_HDR(row, curslabs, NULL, right, 13, size)
	COL_HDR(row, nonfull_slabs, NULL, right, 15, size)
	COL_HDR(row, regs, NULL, right, 5, unsigned)
	COL_HDR(row, pgs, NULL, right, 4, size)
	/* To buffer a right- and left-justified column. */
	COL_HDR(row, justify_spacer, NULL, right, 1, title)
	COL_HDR(row, util, NULL, right, 6, title)
	COL_HDR(row, nfills, NULL, right, 13, uint64)
	COL_HDR(row, nfills_ps, "(#/sec)", right, 8, uint64)
	COL_HDR(row, nflushes, NULL, right, 13, uint64)
	COL_HDR(row, nflushes_ps, "(#/sec)", right, 8, uint64)
	COL_HDR(row, nslabs, NULL, right, 13, uint64)
	COL_HDR(row, nreslabs, NULL, right, 13, uint64)
	COL_HDR(row, nreslabs_ps, "(#/sec)", right, 8, uint64)

	/* Don't want to actually print the name. */
	header_justify_spacer.str_val = " ";
	col_justify_spacer.str_val = " ";

	emitter_col_t col_mutex64[mutex_prof_num_uint64_t_counters];
	emitter_col_t col_mutex32[mutex_prof_num_uint32_t_counters];

	emitter_col_t header_mutex64[mutex_prof_num_uint64_t_counters];
	emitter_col_t header_mutex32[mutex_prof_num_uint32_t_counters];

	if (mutex) {
		mutex_stats_init_cols(&row, NULL, NULL, col_mutex64,
		    col_mutex32);
		mutex_stats_init_cols(&header_row, NULL, NULL, header_mutex64,
		    header_mutex32);
	}

	/*
	 * We print a "bins:" header as part of the table row; we need to adjust
	 * the header size column to compensate.
	 */
	header_size.width -=5;
	emitter_table_printf(emitter, "bins:");
	emitter_table_row(emitter, &header_row);
	emitter_json_array_kv_begin(emitter, "bins");

	size_t stats_arenas_mib[CTL_MAX_DEPTH];
	CTL_LEAF_PREPARE(stats_arenas_mib, 0, "stats.arenas");
	stats_arenas_mib[2] = i;
	CTL_LEAF_PREPARE(stats_arenas_mib, 3, "bins");

	size_t arenas_bin_mib[CTL_MAX_DEPTH];
	CTL_LEAF_PREPARE(arenas_bin_mib, 0, "arenas.bin");

	size_t prof_stats_mib[CTL_MAX_DEPTH];
	if (prof_stats_on) {
		CTL_LEAF_PREPARE(prof_stats_mib, 0, "prof.stats.bins");
	}

	for (j = 0, in_gap = false; j < nbins; j++) {
		uint64_t nslabs;
		size_t reg_size, slab_size, curregs;
		size_t curslabs;
		size_t nonfull_slabs;
		uint32_t nregs, nshards;
		uint64_t nmalloc, ndalloc, nrequests, nfills, nflushes;
		uint64_t nreslabs;
		prof_stats_t prof_live;
		prof_stats_t prof_accum;

		stats_arenas_mib[4] = j;
		arenas_bin_mib[2] = j;

		CTL_LEAF(stats_arenas_mib, 5, "nslabs", &nslabs, uint64_t);

		if (prof_stats_on) {
			prof_stats_mib[3] = j;
			CTL_LEAF(prof_stats_mib, 4, "live", &prof_live,
			    prof_stats_t);
			CTL_LEAF(prof_stats_mib, 4, "accum", &prof_accum,
			    prof_stats_t);
		}

		in_gap_prev = in_gap;
		if (prof_stats_on) {
			in_gap = (nslabs == 0 && prof_accum.count == 0);
		} else {
			in_gap = (nslabs == 0);
		}

		if (in_gap_prev && !in_gap) {
			emitter_table_printf(emitter,
			    "                     ---\n");
		}

		if (in_gap && !emitter_outputs_json(emitter)) {
			continue;
		}

		CTL_LEAF(arenas_bin_mib, 3, "size", &reg_size, size_t);
		CTL_LEAF(arenas_bin_mib, 3, "nregs", &nregs, uint32_t);
		CTL_LEAF(arenas_bin_mib, 3, "slab_size", &slab_size, size_t);
		CTL_LEAF(arenas_bin_mib, 3, "nshards", &nshards, uint32_t);
		CTL_LEAF(stats_arenas_mib, 5, "nmalloc", &nmalloc, uint64_t);
		CTL_LEAF(stats_arenas_mib, 5, "ndalloc", &ndalloc, uint64_t);
		CTL_LEAF(stats_arenas_mib, 5, "curregs", &curregs, size_t);
		CTL_LEAF(stats_arenas_mib, 5, "nrequests", &nrequests,
		    uint64_t);
		CTL_LEAF(stats_arenas_mib, 5, "nfills", &nfills, uint64_t);
		CTL_LEAF(stats_arenas_mib, 5, "nflushes", &nflushes, uint64_t);
		CTL_LEAF(stats_arenas_mib, 5, "nreslabs", &nreslabs, uint64_t);
		CTL_LEAF(stats_arenas_mib, 5, "curslabs", &curslabs, size_t);
		CTL_LEAF(stats_arenas_mib, 5, "nonfull_slabs", &nonfull_slabs,
		    size_t);

		if (mutex) {
			mutex_stats_read_arena_bin(stats_arenas_mib, 5,
			    col_mutex64, col_mutex32, uptime);
		}

		emitter_json_object_begin(emitter);
		emitter_json_kv(emitter, "nmalloc", emitter_type_uint64,
		    &nmalloc);
		emitter_json_kv(emitter, "ndalloc", emitter_type_uint64,
		    &ndalloc);
		emitter_json_kv(emitter, "curregs", emitter_type_size,
		    &curregs);
		emitter_json_kv(emitter, "nrequests", emitter_type_uint64,
		    &nrequests);
		if (prof_stats_on) {
			emitter_json_kv(emitter, "prof_live_requested",
			    emitter_type_uint64, &prof_live.req_sum);
			emitter_json_kv(emitter, "prof_live_count",
			    emitter_type_uint64, &prof_live.count);
			emitter_json_kv(emitter, "prof_accum_requested",
			    emitter_type_uint64, &prof_accum.req_sum);
			emitter_json_kv(emitter, "prof_accum_count",
			    emitter_type_uint64, &prof_accum.count);
		}
		emitter_json_kv(emitter, "nfills", emitter_type_uint64,
		    &nfills);
		emitter_json_kv(emitter, "nflushes", emitter_type_uint64,
		    &nflushes);
		emitter_json_kv(emitter, "nreslabs", emitter_type_uint64,
		    &nreslabs);
		emitter_json_kv(emitter, "curslabs", emitter_type_size,
		    &curslabs);
		emitter_json_kv(emitter, "nonfull_slabs", emitter_type_size,
		    &nonfull_slabs);
		if (mutex) {
			emitter_json_object_kv_begin(emitter, "mutex");
			mutex_stats_emit(emitter, NULL, col_mutex64,
			    col_mutex32);
			emitter_json_object_end(emitter);
		}
		emitter_json_object_end(emitter);

		size_t availregs = nregs * curslabs;
		char util[6];
		if (get_rate_str((uint64_t)curregs, (uint64_t)availregs, util))
		{
			if (availregs == 0) {
				malloc_snprintf(util, sizeof(util), "1");
			} else if (curregs > availregs) {
				/*
				 * Race detected: the counters were read in
				 * separate mallctl calls and concurrent
				 * operations happened in between.  In this case
				 * no meaningful utilization can be computed.
				 */
				malloc_snprintf(util, sizeof(util), " race");
			} else {
				not_reached();
			}
		}

		col_size.size_val = reg_size;
		col_ind.unsigned_val = j;
		col_allocated.size_val = curregs * reg_size;
		col_nmalloc.uint64_val = nmalloc;
		col_nmalloc_ps.uint64_val = rate_per_second(nmalloc, uptime);
		col_ndalloc.uint64_val = ndalloc;
		col_ndalloc_ps.uint64_val = rate_per_second(ndalloc, uptime);
		col_nrequests.uint64_val = nrequests;
		col_nrequests_ps.uint64_val = rate_per_second(nrequests, uptime);
		if (prof_stats_on) {
			col_prof_live_requested.uint64_val = prof_live.req_sum;
			col_prof_live_count.uint64_val = prof_live.count;
			col_prof_accum_requested.uint64_val =
			    prof_accum.req_sum;
			col_prof_accum_count.uint64_val = prof_accum.count;
		}
		col_nshards.unsigned_val = nshards;
		col_curregs.size_val = curregs;
		col_curslabs.size_val = curslabs;
		col_nonfull_slabs.size_val = nonfull_slabs;
		col_regs.unsigned_val = nregs;
		col_pgs.size_val = slab_size / page;
		col_util.str_val = util;
		col_nfills.uint64_val = nfills;
		col_nfills_ps.uint64_val = rate_per_second(nfills, uptime);
		col_nflushes.uint64_val = nflushes;
		col_nflushes_ps.uint64_val = rate_per_second(nflushes, uptime);
		col_nslabs.uint64_val = nslabs;
		col_nreslabs.uint64_val = nreslabs;
		col_nreslabs_ps.uint64_val = rate_per_second(nreslabs, uptime);

		/*
		 * Note that mutex columns were initialized above, if mutex ==
		 * true.
		 */

		emitter_table_row(emitter, &row);
	}
	emitter_json_array_end(emitter); /* Close "bins". */

	if (in_gap) {
		emitter_table_printf(emitter, "                     ---\n");
	}
}

JEMALLOC_COLD
static void
stats_arena_lextents_print(emitter_t *emitter, unsigned i, uint64_t uptime) {
	unsigned nbins, nlextents, j;
	bool in_gap, in_gap_prev;

	CTL_GET("arenas.nbins", &nbins, unsigned);
	CTL_GET("arenas.nlextents", &nlextents, unsigned);

	emitter_row_t header_row;
	emitter_row_init(&header_row);
	emitter_row_t row;
	emitter_row_init(&row);

	bool prof_stats_on = config_prof && opt_prof && opt_prof_stats
	    && i == MALLCTL_ARENAS_ALL;

	COL_HDR(row, size, NULL, right, 20, size)
	COL_HDR(row, ind, NULL, right, 4, unsigned)
	COL_HDR(row, allocated, NULL, right, 13, size)
	COL_HDR(row, nmalloc, NULL, right, 13, uint64)
	COL_HDR(row, nmalloc_ps, "(#/sec)", right, 8, uint64)
	COL_HDR(row, ndalloc, NULL, right, 13, uint64)
	COL_HDR(row, ndalloc_ps, "(#/sec)", right, 8, uint64)
	COL_HDR(row, nrequests, NULL, right, 13, uint64)
	COL_HDR(row, nrequests_ps, "(#/sec)", right, 8, uint64)
	COL_HDR_DECLARE(prof_live_requested)
	COL_HDR_DECLARE(prof_live_count)
	COL_HDR_DECLARE(prof_accum_requested)
	COL_HDR_DECLARE(prof_accum_count)
	if (prof_stats_on) {
		COL_HDR_INIT(row, prof_live_requested, NULL, right, 21, uint64)
		COL_HDR_INIT(row, prof_live_count, NULL, right, 17, uint64)
		COL_HDR_INIT(row, prof_accum_requested, NULL, right, 21, uint64)
		COL_HDR_INIT(row, prof_accum_count, NULL, right, 17, uint64)
	}
	COL_HDR(row, curlextents, NULL, right, 13, size)

	/* As with bins, we label the large extents table. */
	header_size.width -= 6;
	emitter_table_printf(emitter, "large:");
	emitter_table_row(emitter, &header_row);
	emitter_json_array_kv_begin(emitter, "lextents");

	size_t stats_arenas_mib[CTL_MAX_DEPTH];
	CTL_LEAF_PREPARE(stats_arenas_mib, 0, "stats.arenas");
	stats_arenas_mib[2] = i;
	CTL_LEAF_PREPARE(stats_arenas_mib, 3, "lextents");

	size_t arenas_lextent_mib[CTL_MAX_DEPTH];
	CTL_LEAF_PREPARE(arenas_lextent_mib, 0, "arenas.lextent");

	size_t prof_stats_mib[CTL_MAX_DEPTH];
	if (prof_stats_on) {
		CTL_LEAF_PREPARE(prof_stats_mib, 0, "prof.stats.lextents");
	}

	for (j = 0, in_gap = false; j < nlextents; j++) {
		uint64_t nmalloc, ndalloc, nrequests;
		size_t lextent_size, curlextents;
		prof_stats_t prof_live;
		prof_stats_t prof_accum;

		stats_arenas_mib[4] = j;
		arenas_lextent_mib[2] = j;

		CTL_LEAF(stats_arenas_mib, 5, "nmalloc", &nmalloc, uint64_t);
		CTL_LEAF(stats_arenas_mib, 5, "ndalloc", &ndalloc, uint64_t);
		CTL_LEAF(stats_arenas_mib, 5, "nrequests", &nrequests,
		    uint64_t);

		in_gap_prev = in_gap;
		in_gap = (nrequests == 0);

		if (in_gap_prev && !in_gap) {
			emitter_table_printf(emitter,
			    "                     ---\n");
		}

		CTL_LEAF(arenas_lextent_mib, 3, "size", &lextent_size, size_t);
		CTL_LEAF(stats_arenas_mib, 5, "curlextents", &curlextents,
		    size_t);

		if (prof_stats_on) {
			prof_stats_mib[3] = j;
			CTL_LEAF(prof_stats_mib, 4, "live", &prof_live,
			    prof_stats_t);
			CTL_LEAF(prof_stats_mib, 4, "accum", &prof_accum,
			    prof_stats_t);
		}

		emitter_json_object_begin(emitter);
		if (prof_stats_on) {
			emitter_json_kv(emitter, "prof_live_requested",
			    emitter_type_uint64, &prof_live.req_sum);
			emitter_json_kv(emitter, "prof_live_count",
			    emitter_type_uint64, &prof_live.count);
			emitter_json_kv(emitter, "prof_accum_requested",
			    emitter_type_uint64, &prof_accum.req_sum);
			emitter_json_kv(emitter, "prof_accum_count",
			    emitter_type_uint64, &prof_accum.count);
		}
		emitter_json_kv(emitter, "curlextents", emitter_type_size,
		    &curlextents);
		emitter_json_object_end(emitter);

		col_size.size_val = lextent_size;
		col_ind.unsigned_val = nbins + j;
		col_allocated.size_val = curlextents * lextent_size;
		col_nmalloc.uint64_val = nmalloc;
		col_nmalloc_ps.uint64_val = rate_per_second(nmalloc, uptime);
		col_ndalloc.uint64_val = ndalloc;
		col_ndalloc_ps.uint64_val = rate_per_second(ndalloc, uptime);
		col_nrequests.uint64_val = nrequests;
		col_nrequests_ps.uint64_val = rate_per_second(nrequests, uptime);
		if (prof_stats_on) {
			col_prof_live_requested.uint64_val = prof_live.req_sum;
			col_prof_live_count.uint64_val = prof_live.count;
			col_prof_accum_requested.uint64_val =
			    prof_accum.req_sum;
			col_prof_accum_count.uint64_val = prof_accum.count;
		}
		col_curlextents.size_val = curlextents;

		if (!in_gap) {
			emitter_table_row(emitter, &row);
		}
	}
	emitter_json_array_end(emitter); /* Close "lextents". */
	if (in_gap) {
		emitter_table_printf(emitter, "                     ---\n");
	}
}

JEMALLOC_COLD
static void
stats_arena_extents_print(emitter_t *emitter, unsigned i) {
	unsigned j;
	bool in_gap, in_gap_prev;
	emitter_row_t header_row;
	emitter_row_init(&header_row);
	emitter_row_t row;
	emitter_row_init(&row);

	COL_HDR(row, size, NULL, right, 20, size)
	COL_HDR(row, ind, NULL, right, 4, unsigned)
	COL_HDR(row, ndirty, NULL, right, 13, size)
	COL_HDR(row, dirty, NULL, right, 13, size)
	COL_HDR(row, nmuzzy, NULL, right, 13, size)
	COL_HDR(row, muzzy, NULL, right, 13, size)
	COL_HDR(row, nretained, NULL, right, 13, size)
	COL_HDR(row, retained, NULL, right, 13, size)
	COL_HDR(row, ntotal, NULL, right, 13, size)
	COL_HDR(row, total, NULL, right, 13, size)

	/* Label this section. */
	header_size.width -= 8;
	emitter_table_printf(emitter, "extents:");
	emitter_table_row(emitter, &header_row);
	emitter_json_array_kv_begin(emitter, "extents");

	size_t stats_arenas_mib[CTL_MAX_DEPTH];
	CTL_LEAF_PREPARE(stats_arenas_mib, 0, "stats.arenas");
	stats_arenas_mib[2] = i;
	CTL_LEAF_PREPARE(stats_arenas_mib, 3, "extents");

	in_gap = false;
	for (j = 0; j < SC_NPSIZES; j++) {
		size_t ndirty, nmuzzy, nretained, total, dirty_bytes,
		    muzzy_bytes, retained_bytes, total_bytes;
		stats_arenas_mib[4] = j;

		CTL_LEAF(stats_arenas_mib, 5, "ndirty", &ndirty, size_t);
		CTL_LEAF(stats_arenas_mib, 5, "nmuzzy", &nmuzzy, size_t);
		CTL_LEAF(stats_arenas_mib, 5, "nretained", &nretained, size_t);
		CTL_LEAF(stats_arenas_mib, 5, "dirty_bytes", &dirty_bytes,
		    size_t);
		CTL_LEAF(stats_arenas_mib, 5, "muzzy_bytes", &muzzy_bytes,
		    size_t);
		CTL_LEAF(stats_arenas_mib, 5, "retained_bytes",
		    &retained_bytes, size_t);

		total = ndirty + nmuzzy + nretained;
		total_bytes = dirty_bytes + muzzy_bytes + retained_bytes;

		in_gap_prev = in_gap;
		in_gap = (total == 0);

		if (in_gap_prev && !in_gap) {
			emitter_table_printf(emitter,
			    "                     ---\n");
		}

		emitter_json_object_begin(emitter);
		emitter_json_kv(emitter, "ndirty", emitter_type_size, &ndirty);
		emitter_json_kv(emitter, "nmuzzy", emitter_type_size, &nmuzzy);
		emitter_json_kv(emitter, "nretained", emitter_type_size,
		    &nretained);

		emitter_json_kv(emitter, "dirty_bytes", emitter_type_size,
		    &dirty_bytes);
		emitter_json_kv(emitter, "muzzy_bytes", emitter_type_size,
		    &muzzy_bytes);
		emitter_json_kv(emitter, "retained_bytes", emitter_type_size,
		    &retained_bytes);
		emitter_json_object_end(emitter);

		col_size.size_val = sz_pind2sz(j);
		col_ind.size_val = j;
		col_ndirty.size_val = ndirty;
		col_dirty.size_val = dirty_bytes;
		col_nmuzzy.size_val = nmuzzy;
		col_muzzy.size_val = muzzy_bytes;
		col_nretained.size_val = nretained;
		col_retained.size_val = retained_bytes;
		col_ntotal.size_val = total;
		col_total.size_val = total_bytes;

		if (!in_gap) {
			emitter_table_row(emitter, &row);
		}
	}
	emitter_json_array_end(emitter); /* Close "extents". */
	if (in_gap) {
		emitter_table_printf(emitter, "                     ---\n");
	}
}

static void
stats_arena_hpa_shard_print(emitter_t *emitter, unsigned i, uint64_t uptime) {
	emitter_row_t header_row;
	emitter_row_init(&header_row);
	emitter_row_t row;
	emitter_row_init(&row);

	uint64_t npurge_passes;
	uint64_t npurges;
	uint64_t nhugifies;
	uint64_t ndehugifies;

	CTL_M2_GET("stats.arenas.0.hpa_shard.npurge_passes",
	    i, &npurge_passes, uint64_t);
	CTL_M2_GET("stats.arenas.0.hpa_shard.npurges",
	    i, &npurges, uint64_t);
	CTL_M2_GET("stats.arenas.0.hpa_shard.nhugifies",
	    i, &nhugifies, uint64_t);
	CTL_M2_GET("stats.arenas.0.hpa_shard.ndehugifies",
	    i, &ndehugifies, uint64_t);

	size_t npageslabs_huge;
	size_t nactive_huge;
	size_t ndirty_huge;

	size_t npageslabs_nonhuge;
	size_t nactive_nonhuge;
	size_t ndirty_nonhuge;
	size_t nretained_nonhuge;

	size_t sec_bytes;
	CTL_M2_GET("stats.arenas.0.hpa_sec_bytes", i, &sec_bytes, size_t);
	emitter_kv(emitter, "sec_bytes", "Bytes in small extent cache",
	    emitter_type_size, &sec_bytes);

	/* First, global stats. */
	emitter_table_printf(emitter,
	    "HPA shard stats:\n"
	    "  Purge passes: %" FMTu64 " (%" FMTu64 " / sec)\n"
	    "  Purges: %" FMTu64 " (%" FMTu64 " / sec)\n"
	    "  Hugeifies: %" FMTu64 " (%" FMTu64 " / sec)\n"
	    "  Dehugifies: %" FMTu64 " (%" FMTu64 " / sec)\n"
	    "\n",
	    npurge_passes, rate_per_second(npurge_passes, uptime),
	    npurges, rate_per_second(npurges, uptime),
	    nhugifies, rate_per_second(nhugifies, uptime),
	    ndehugifies, rate_per_second(ndehugifies, uptime));

	emitter_json_object_kv_begin(emitter, "hpa_shard");
	emitter_json_kv(emitter, "npurge_passes", emitter_type_uint64,
	    &npurge_passes);
	emitter_json_kv(emitter, "npurges", emitter_type_uint64,
	    &npurges);
	emitter_json_kv(emitter, "nhugifies", emitter_type_uint64,
	    &nhugifies);
	emitter_json_kv(emitter, "ndehugifies", emitter_type_uint64,
	    &ndehugifies);

	/* Next, full slab stats. */
	CTL_M2_GET("stats.arenas.0.hpa_shard.full_slabs.npageslabs_huge",
	    i, &npageslabs_huge, size_t);
	CTL_M2_GET("stats.arenas.0.hpa_shard.full_slabs.nactive_huge",
	    i, &nactive_huge, size_t);
	CTL_M2_GET("stats.arenas.0.hpa_shard.full_slabs.ndirty_huge",
	    i, &ndirty_huge, size_t);

	CTL_M2_GET("stats.arenas.0.hpa_shard.full_slabs.npageslabs_nonhuge",
	    i, &npageslabs_nonhuge, size_t);
	CTL_M2_GET("stats.arenas.0.hpa_shard.full_slabs.nactive_nonhuge",
	    i, &nactive_nonhuge, size_t);
	CTL_M2_GET("stats.arenas.0.hpa_shard.full_slabs.ndirty_nonhuge",
	    i, &ndirty_nonhuge, size_t);
	nretained_nonhuge = npageslabs_nonhuge * HUGEPAGE_PAGES
	    - nactive_nonhuge - ndirty_nonhuge;

	emitter_table_printf(emitter,
	    "  In full slabs:\n"
	    "      npageslabs: %zu huge, %zu nonhuge\n"
	    "      nactive: %zu huge, %zu nonhuge \n"
	    "      ndirty: %zu huge, %zu nonhuge \n"
	    "      nretained: 0 huge, %zu nonhuge \n",
	    npageslabs_huge, npageslabs_nonhuge,
	    nactive_huge, nactive_nonhuge,
	    ndirty_huge, ndirty_nonhuge,
	    nretained_nonhuge);

	emitter_json_object_kv_begin(emitter, "full_slabs");
	emitter_json_kv(emitter, "npageslabs_huge", emitter_type_size,
	    &npageslabs_huge);
	emitter_json_kv(emitter, "nactive_huge", emitter_type_size,
	    &nactive_huge);
	emitter_json_kv(emitter, "nactive_huge", emitter_type_size,
	    &nactive_huge);
	emitter_json_kv(emitter, "npageslabs_nonhuge", emitter_type_size,
	    &npageslabs_nonhuge);
	emitter_json_kv(emitter, "nactive_nonhuge", emitter_type_size,
	    &nactive_nonhuge);
	emitter_json_kv(emitter, "ndirty_nonhuge", emitter_type_size,
	    &ndirty_nonhuge);
	emitter_json_object_end(emitter); /* End "full_slabs" */

	/* Next, empty slab stats. */
	CTL_M2_GET("stats.arenas.0.hpa_shard.empty_slabs.npageslabs_huge",
	    i, &npageslabs_huge, size_t);
	CTL_M2_GET("stats.arenas.0.hpa_shard.empty_slabs.nactive_huge",
	    i, &nactive_huge, size_t);
	CTL_M2_GET("stats.arenas.0.hpa_shard.empty_slabs.ndirty_huge",
	    i, &ndirty_huge, size_t);

	CTL_M2_GET("stats.arenas.0.hpa_shard.empty_slabs.npageslabs_nonhuge",
	    i, &npageslabs_nonhuge, size_t);
	CTL_M2_GET("stats.arenas.0.hpa_shard.empty_slabs.nactive_nonhuge",
	    i, &nactive_nonhuge, size_t);
	CTL_M2_GET("stats.arenas.0.hpa_shard.empty_slabs.ndirty_nonhuge",
	    i, &ndirty_nonhuge, size_t);
	nretained_nonhuge = npageslabs_nonhuge * HUGEPAGE_PAGES
	    - nactive_nonhuge - ndirty_nonhuge;

	emitter_table_printf(emitter,
	    "  In empty slabs:\n"
	    "      npageslabs: %zu huge, %zu nonhuge\n"
	    "      nactive: %zu huge, %zu nonhuge \n"
	    "      ndirty: %zu huge, %zu nonhuge \n"
	    "      nretained: 0 huge, %zu nonhuge \n"
	    "\n",
	    npageslabs_huge, npageslabs_nonhuge,
	    nactive_huge, nactive_nonhuge,
	    ndirty_huge, ndirty_nonhuge,
	    nretained_nonhuge);

	emitter_json_object_kv_begin(emitter, "empty_slabs");
	emitter_json_kv(emitter, "npageslabs_huge", emitter_type_size,
	    &npageslabs_huge);
	emitter_json_kv(emitter, "nactive_huge", emitter_type_size,
	    &nactive_huge);
	emitter_json_kv(emitter, "nactive_huge", emitter_type_size,
	    &nactive_huge);
	emitter_json_kv(emitter, "npageslabs_nonhuge", emitter_type_size,
	    &npageslabs_nonhuge);
	emitter_json_kv(emitter, "nactive_nonhuge", emitter_type_size,
	    &nactive_nonhuge);
	emitter_json_kv(emitter, "ndirty_nonhuge", emitter_type_size,
	    &ndirty_nonhuge);
	emitter_json_object_end(emitter); /* End "empty_slabs" */

	COL_HDR(row, size, NULL, right, 20, size)
	COL_HDR(row, ind, NULL, right, 4, unsigned)
	COL_HDR(row, npageslabs_huge, NULL, right, 16, size)
	COL_HDR(row, nactive_huge, NULL, right, 16, size)
	COL_HDR(row, ndirty_huge, NULL, right, 16, size)
	COL_HDR(row, npageslabs_nonhuge, NULL, right, 20, size)
	COL_HDR(row, nactive_nonhuge, NULL, right, 20, size)
	COL_HDR(row, ndirty_nonhuge, NULL, right, 20, size)
	COL_HDR(row, nretained_nonhuge, NULL, right, 20, size)

	size_t stats_arenas_mib[CTL_MAX_DEPTH];
	CTL_LEAF_PREPARE(stats_arenas_mib, 0, "stats.arenas");
	stats_arenas_mib[2] = i;
	CTL_LEAF_PREPARE(stats_arenas_mib, 3, "hpa_shard.nonfull_slabs");

	emitter_table_row(emitter, &header_row);
	emitter_json_array_kv_begin(emitter, "nonfull_slabs");
	bool in_gap = false;
	for (pszind_t j = 0; j < PSSET_NPSIZES && j < SC_NPSIZES; j++) {
		stats_arenas_mib[5] = j;

		CTL_LEAF(stats_arenas_mib, 6, "npageslabs_huge",
		    &npageslabs_huge, size_t);
		CTL_LEAF(stats_arenas_mib, 6, "nactive_huge",
		    &nactive_huge, size_t);
		CTL_LEAF(stats_arenas_mib, 6, "ndirty_huge",
		    &ndirty_huge, size_t);

		CTL_LEAF(stats_arenas_mib, 6, "npageslabs_nonhuge",
		    &npageslabs_nonhuge, size_t);
		CTL_LEAF(stats_arenas_mib, 6, "nactive_nonhuge",
		    &nactive_nonhuge, size_t);
		CTL_LEAF(stats_arenas_mib, 6, "ndirty_nonhuge",
		    &ndirty_nonhuge, size_t);
		nretained_nonhuge = npageslabs_nonhuge * HUGEPAGE_PAGES
		    - nactive_nonhuge - ndirty_nonhuge;

		bool in_gap_prev = in_gap;
		in_gap = (npageslabs_huge == 0 && npageslabs_nonhuge == 0);
		if (in_gap_prev && !in_gap) {
			emitter_table_printf(emitter,
			    "                     ---\n");
		}

		col_size.size_val = sz_pind2sz(j);
		col_ind.size_val = j;
		col_npageslabs_huge.size_val = npageslabs_huge;
		col_nactive_huge.size_val = nactive_huge;
		col_ndirty_huge.size_val = ndirty_huge;
		col_npageslabs_nonhuge.size_val = npageslabs_nonhuge;
		col_nactive_nonhuge.size_val = nactive_nonhuge;
		col_ndirty_nonhuge.size_val = ndirty_nonhuge;
		col_nretained_nonhuge.size_val = nretained_nonhuge;
		if (!in_gap) {
			emitter_table_row(emitter, &row);
		}

		emitter_json_object_begin(emitter);
		emitter_json_kv(emitter, "npageslabs_huge", emitter_type_size,
		    &npageslabs_huge);
		emitter_json_kv(emitter, "nactive_huge", emitter_type_size,
		    &nactive_huge);
		emitter_json_kv(emitter, "ndirty_huge", emitter_type_size,
		    &ndirty_huge);
		emitter_json_kv(emitter, "npageslabs_nonhuge", emitter_type_size,
		    &npageslabs_nonhuge);
		emitter_json_kv(emitter, "nactive_nonhuge", emitter_type_size,
		    &nactive_nonhuge);
		emitter_json_kv(emitter, "ndirty_nonhuge", emitter_type_size,
		    &ndirty_nonhuge);
		emitter_json_object_end(emitter);
	}
	emitter_json_array_end(emitter); /* End "nonfull_slabs" */
	emitter_json_object_end(emitter); /* End "hpa_shard" */
	if (in_gap) {
		emitter_table_printf(emitter, "                     ---\n");
	}
}

static void
stats_arena_mutexes_print(emitter_t *emitter, unsigned arena_ind, uint64_t uptime) {
	emitter_row_t row;
	emitter_col_t col_name;
	emitter_col_t col64[mutex_prof_num_uint64_t_counters];
	emitter_col_t col32[mutex_prof_num_uint32_t_counters];

	emitter_row_init(&row);
	mutex_stats_init_cols(&row, "", &col_name, col64, col32);

	emitter_json_object_kv_begin(emitter, "mutexes");
	emitter_table_row(emitter, &row);

	size_t stats_arenas_mib[CTL_MAX_DEPTH];
	CTL_LEAF_PREPARE(stats_arenas_mib, 0, "stats.arenas");
	stats_arenas_mib[2] = arena_ind;
	CTL_LEAF_PREPARE(stats_arenas_mib, 3, "mutexes");

	for (mutex_prof_arena_ind_t i = 0; i < mutex_prof_num_arena_mutexes;
	    i++) {
		const char *name = arena_mutex_names[i];
		emitter_json_object_kv_begin(emitter, name);
		mutex_stats_read_arena(stats_arenas_mib, 4, name, &col_name,
		    col64, col32, uptime);
		mutex_stats_emit(emitter, &row, col64, col32);
		emitter_json_object_end(emitter); /* Close the mutex dict. */
	}
	emitter_json_object_end(emitter); /* End "mutexes". */
}

JEMALLOC_COLD
static void
stats_arena_print(emitter_t *emitter, unsigned i, bool bins, bool large,
    bool mutex, bool extents, bool hpa) {
	unsigned nthreads;
	const char *dss;
	ssize_t dirty_decay_ms, muzzy_decay_ms;
	size_t page, pactive, pdirty, pmuzzy, mapped, retained;
	size_t base, internal, resident, metadata_thp, extent_avail;
	uint64_t dirty_npurge, dirty_nmadvise, dirty_purged;
	uint64_t muzzy_npurge, muzzy_nmadvise, muzzy_purged;
	size_t small_allocated;
	uint64_t small_nmalloc, small_ndalloc, small_nrequests, small_nfills,
	    small_nflushes;
	size_t large_allocated;
	uint64_t large_nmalloc, large_ndalloc, large_nrequests, large_nfills,
	    large_nflushes;
	size_t tcache_bytes, tcache_stashed_bytes, abandoned_vm;
	uint64_t uptime;

	CTL_GET("arenas.page", &page, size_t);

	CTL_M2_GET("stats.arenas.0.nthreads", i, &nthreads, unsigned);
	emitter_kv(emitter, "nthreads", "assigned threads",
	    emitter_type_unsigned, &nthreads);

	CTL_M2_GET("stats.arenas.0.uptime", i, &uptime, uint64_t);
	emitter_kv(emitter, "uptime_ns", "uptime", emitter_type_uint64,
	    &uptime);

	CTL_M2_GET("stats.arenas.0.dss", i, &dss, const char *);
	emitter_kv(emitter, "dss", "dss allocation precedence",
	    emitter_type_string, &dss);

	CTL_M2_GET("stats.arenas.0.dirty_decay_ms", i, &dirty_decay_ms,
	    ssize_t);
	CTL_M2_GET("stats.arenas.0.muzzy_decay_ms", i, &muzzy_decay_ms,
	    ssize_t);
	CTL_M2_GET("stats.arenas.0.pactive", i, &pactive, size_t);
	CTL_M2_GET("stats.arenas.0.pdirty", i, &pdirty, size_t);
	CTL_M2_GET("stats.arenas.0.pmuzzy", i, &pmuzzy, size_t);
	CTL_M2_GET("stats.arenas.0.dirty_npurge", i, &dirty_npurge, uint64_t);
	CTL_M2_GET("stats.arenas.0.dirty_nmadvise", i, &dirty_nmadvise,
	    uint64_t);
	CTL_M2_GET("stats.arenas.0.dirty_purged", i, &dirty_purged, uint64_t);
	CTL_M2_GET("stats.arenas.0.muzzy_npurge", i, &muzzy_npurge, uint64_t);
	CTL_M2_GET("stats.arenas.0.muzzy_nmadvise", i, &muzzy_nmadvise,
	    uint64_t);
	CTL_M2_GET("stats.arenas.0.muzzy_purged", i, &muzzy_purged, uint64_t);

	emitter_row_t decay_row;
	emitter_row_init(&decay_row);

	/* JSON-style emission. */
	emitter_json_kv(emitter, "dirty_decay_ms", emitter_type_ssize,
	    &dirty_decay_ms);
	emitter_json_kv(emitter, "muzzy_decay_ms", emitter_type_ssize,
	    &muzzy_decay_ms);

	emitter_json_kv(emitter, "pactive", emitter_type_size, &pactive);
	emitter_json_kv(emitter, "pdirty", emitter_type_size, &pdirty);
	emitter_json_kv(emitter, "pmuzzy", emitter_type_size, &pmuzzy);

	emitter_json_kv(emitter, "dirty_npurge", emitter_type_uint64,
	    &dirty_npurge);
	emitter_json_kv(emitter, "dirty_nmadvise", emitter_type_uint64,
	    &dirty_nmadvise);
	emitter_json_kv(emitter, "dirty_purged", emitter_type_uint64,
	    &dirty_purged);

	emitter_json_kv(emitter, "muzzy_npurge", emitter_type_uint64,
	    &muzzy_npurge);
	emitter_json_kv(emitter, "muzzy_nmadvise", emitter_type_uint64,
	    &muzzy_nmadvise);
	emitter_json_kv(emitter, "muzzy_purged", emitter_type_uint64,
	    &muzzy_purged);

	/* Table-style emission. */
	COL(decay_row, decay_type, right, 9, title);
	col_decay_type.str_val = "decaying:";

	COL(decay_row, decay_time, right, 6, title);
	col_decay_time.str_val = "time";

	COL(decay_row, decay_npages, right, 13, title);
	col_decay_npages.str_val = "npages";

	COL(decay_row, decay_sweeps, right, 13, title);
	col_decay_sweeps.str_val = "sweeps";

	COL(decay_row, decay_madvises, right, 13, title);
	col_decay_madvises.str_val = "madvises";

	COL(decay_row, decay_purged, right, 13, title);
	col_decay_purged.str_val = "purged";

	/* Title row. */
	emitter_table_row(emitter, &decay_row);

	/* Dirty row. */
	col_decay_type.str_val = "dirty:";

	if (dirty_decay_ms >= 0) {
		col_decay_time.type = emitter_type_ssize;
		col_decay_time.ssize_val = dirty_decay_ms;
	} else {
		col_decay_time.type = emitter_type_title;
		col_decay_time.str_val = "N/A";
	}

	col_decay_npages.type = emitter_type_size;
	col_decay_npages.size_val = pdirty;

	col_decay_sweeps.type = emitter_type_uint64;
	col_decay_sweeps.uint64_val = dirty_npurge;

	col_decay_madvises.type = emitter_type_uint64;
	col_decay_madvises.uint64_val = dirty_nmadvise;

	col_decay_purged.type = emitter_type_uint64;
	col_decay_purged.uint64_val = dirty_purged;

	emitter_table_row(emitter, &decay_row);

	/* Muzzy row. */
	col_decay_type.str_val = "muzzy:";

	if (muzzy_decay_ms >= 0) {
		col_decay_time.type = emitter_type_ssize;
		col_decay_time.ssize_val = muzzy_decay_ms;
	} else {
		col_decay_time.type = emitter_type_title;
		col_decay_time.str_val = "N/A";
	}

	col_decay_npages.type = emitter_type_size;
	col_decay_npages.size_val = pmuzzy;

	col_decay_sweeps.type = emitter_type_uint64;
	col_decay_sweeps.uint64_val = muzzy_npurge;

	col_decay_madvises.type = emitter_type_uint64;
	col_decay_madvises.uint64_val = muzzy_nmadvise;

	col_decay_purged.type = emitter_type_uint64;
	col_decay_purged.uint64_val = muzzy_purged;

	emitter_table_row(emitter, &decay_row);

	/* Small / large / total allocation counts. */
	emitter_row_t alloc_count_row;
	emitter_row_init(&alloc_count_row);

	COL(alloc_count_row, count_title, left, 21, title);
	col_count_title.str_val = "";

	COL(alloc_count_row, count_allocated, right, 16, title);
	col_count_allocated.str_val = "allocated";

	COL(alloc_count_row, count_nmalloc, right, 16, title);
	col_count_nmalloc.str_val = "nmalloc";
	COL(alloc_count_row, count_nmalloc_ps, right, 10, title);
	col_count_nmalloc_ps.str_val = "(#/sec)";

	COL(alloc_count_row, count_ndalloc, right, 16, title);
	col_count_ndalloc.str_val = "ndalloc";
	COL(alloc_count_row, count_ndalloc_ps, right, 10, title);
	col_count_ndalloc_ps.str_val = "(#/sec)";

	COL(alloc_count_row, count_nrequests, right, 16, title);
	col_count_nrequests.str_val = "nrequests";
	COL(alloc_count_row, count_nrequests_ps, right, 10, title);
	col_count_nrequests_ps.str_val = "(#/sec)";

	COL(alloc_count_row, count_nfills, right, 16, title);
	col_count_nfills.str_val = "nfill";
	COL(alloc_count_row, count_nfills_ps, right, 10, title);
	col_count_nfills_ps.str_val = "(#/sec)";

	COL(alloc_count_row, count_nflushes, right, 16, title);
	col_count_nflushes.str_val = "nflush";
	COL(alloc_count_row, count_nflushes_ps, right, 10, title);
	col_count_nflushes_ps.str_val = "(#/sec)";

	emitter_table_row(emitter, &alloc_count_row);

	col_count_nmalloc_ps.type = emitter_type_uint64;
	col_count_ndalloc_ps.type = emitter_type_uint64;
	col_count_nrequests_ps.type = emitter_type_uint64;
	col_count_nfills_ps.type = emitter_type_uint64;
	col_count_nflushes_ps.type = emitter_type_uint64;

#define GET_AND_EMIT_ALLOC_STAT(small_or_large, name, valtype)		\
	CTL_M2_GET("stats.arenas.0." #small_or_large "." #name, i,	\
	    &small_or_large##_##name, valtype##_t);			\
	emitter_json_kv(emitter, #name, emitter_type_##valtype,		\
	    &small_or_large##_##name);					\
	col_count_##name.type = emitter_type_##valtype;		\
	col_count_##name.valtype##_val = small_or_large##_##name;

	emitter_json_object_kv_begin(emitter, "small");
	col_count_title.str_val = "small:";

	GET_AND_EMIT_ALLOC_STAT(small, allocated, size)
	GET_AND_EMIT_ALLOC_STAT(small, nmalloc, uint64)
	col_count_nmalloc_ps.uint64_val =
	    rate_per_second(col_count_nmalloc.uint64_val, uptime);
	GET_AND_EMIT_ALLOC_STAT(small, ndalloc, uint64)
	col_count_ndalloc_ps.uint64_val =
	    rate_per_second(col_count_ndalloc.uint64_val, uptime);
	GET_AND_EMIT_ALLOC_STAT(small, nrequests, uint64)
	col_count_nrequests_ps.uint64_val =
	    rate_per_second(col_count_nrequests.uint64_val, uptime);
	GET_AND_EMIT_ALLOC_STAT(small, nfills, uint64)
	col_count_nfills_ps.uint64_val =
	    rate_per_second(col_count_nfills.uint64_val, uptime);
	GET_AND_EMIT_ALLOC_STAT(small, nflushes, uint64)
	col_count_nflushes_ps.uint64_val =
	    rate_per_second(col_count_nflushes.uint64_val, uptime);

	emitter_table_row(emitter, &alloc_count_row);
	emitter_json_object_end(emitter); /* Close "small". */

	emitter_json_object_kv_begin(emitter, "large");
	col_count_title.str_val = "large:";

	GET_AND_EMIT_ALLOC_STAT(large, allocated, size)
	GET_AND_EMIT_ALLOC_STAT(large, nmalloc, uint64)
	col_count_nmalloc_ps.uint64_val =
	    rate_per_second(col_count_nmalloc.uint64_val, uptime);
	GET_AND_EMIT_ALLOC_STAT(large, ndalloc, uint64)
	col_count_ndalloc_ps.uint64_val =
	    rate_per_second(col_count_ndalloc.uint64_val, uptime);
	GET_AND_EMIT_ALLOC_STAT(large, nrequests, uint64)
	col_count_nrequests_ps.uint64_val =
	    rate_per_second(col_count_nrequests.uint64_val, uptime);
	GET_AND_EMIT_ALLOC_STAT(large, nfills, uint64)
	col_count_nfills_ps.uint64_val =
	    rate_per_second(col_count_nfills.uint64_val, uptime);
	GET_AND_EMIT_ALLOC_STAT(large, nflushes, uint64)
	col_count_nflushes_ps.uint64_val =
	    rate_per_second(col_count_nflushes.uint64_val, uptime);

	emitter_table_row(emitter, &alloc_count_row);
	emitter_json_object_end(emitter); /* Close "large". */

#undef GET_AND_EMIT_ALLOC_STAT

	/* Aggregated small + large stats are emitter only in table mode. */
	col_count_title.str_val = "total:";
	col_count_allocated.size_val = small_allocated + large_allocated;
	col_count_nmalloc.uint64_val = small_nmalloc + large_nmalloc;
	col_count_ndalloc.uint64_val = small_ndalloc + large_ndalloc;
	col_count_nrequests.uint64_val = small_nrequests + large_nrequests;
	col_count_nfills.uint64_val = small_nfills + large_nfills;
	col_count_nflushes.uint64_val = small_nflushes + large_nflushes;
	col_count_nmalloc_ps.uint64_val =
	    rate_per_second(col_count_nmalloc.uint64_val, uptime);
	col_count_ndalloc_ps.uint64_val =
	    rate_per_second(col_count_ndalloc.uint64_val, uptime);
	col_count_nrequests_ps.uint64_val =
	    rate_per_second(col_count_nrequests.uint64_val, uptime);
	col_count_nfills_ps.uint64_val =
	    rate_per_second(col_count_nfills.uint64_val, uptime);
	col_count_nflushes_ps.uint64_val =
	    rate_per_second(col_count_nflushes.uint64_val, uptime);
	emitter_table_row(emitter, &alloc_count_row);

	emitter_row_t mem_count_row;
	emitter_row_init(&mem_count_row);

	emitter_col_t mem_count_title;
	emitter_col_init(&mem_count_title, &mem_count_row);
	mem_count_title.justify = emitter_justify_left;
	mem_count_title.width = 21;
	mem_count_title.type = emitter_type_title;
	mem_count_title.str_val = "";

	emitter_col_t mem_count_val;
	emitter_col_init(&mem_count_val, &mem_count_row);
	mem_count_val.justify = emitter_justify_right;
	mem_count_val.width = 16;
	mem_count_val.type = emitter_type_title;
	mem_count_val.str_val = "";

	emitter_table_row(emitter, &mem_count_row);
	mem_count_val.type = emitter_type_size;

	/* Active count in bytes is emitted only in table mode. */
	mem_count_title.str_val = "active:";
	mem_count_val.size_val = pactive * page;
	emitter_table_row(emitter, &mem_count_row);

#define GET_AND_EMIT_MEM_STAT(stat)					\
	CTL_M2_GET("stats.arenas.0."#stat, i, &stat, size_t);		\
	emitter_json_kv(emitter, #stat, emitter_type_size, &stat);	\
	mem_count_title.str_val = #stat":";				\
	mem_count_val.size_val = stat;					\
	emitter_table_row(emitter, &mem_count_row);

	GET_AND_EMIT_MEM_STAT(mapped)
	GET_AND_EMIT_MEM_STAT(retained)
	GET_AND_EMIT_MEM_STAT(base)
	GET_AND_EMIT_MEM_STAT(internal)
	GET_AND_EMIT_MEM_STAT(metadata_thp)
	GET_AND_EMIT_MEM_STAT(tcache_bytes)
	GET_AND_EMIT_MEM_STAT(tcache_stashed_bytes)
	GET_AND_EMIT_MEM_STAT(resident)
	GET_AND_EMIT_MEM_STAT(abandoned_vm)
	GET_AND_EMIT_MEM_STAT(extent_avail)
#undef GET_AND_EMIT_MEM_STAT

	if (mutex) {
		stats_arena_mutexes_print(emitter, i, uptime);
	}
	if (bins) {
		stats_arena_bins_print(emitter, mutex, i, uptime);
	}
	if (large) {
		stats_arena_lextents_print(emitter, i, uptime);
	}
	if (extents) {
		stats_arena_extents_print(emitter, i);
	}
	if (hpa) {
		stats_arena_hpa_shard_print(emitter, i, uptime);
	}
}

JEMALLOC_COLD
static void
stats_general_print(emitter_t *emitter) {
	const char *cpv;
	bool bv, bv2;
	unsigned uv;
	uint32_t u32v;
	uint64_t u64v;
	int64_t i64v;
	ssize_t ssv, ssv2;
	size_t sv, bsz, usz, u32sz, u64sz, i64sz, ssz, sssz, cpsz;

	bsz = sizeof(bool);
	usz = sizeof(unsigned);
	ssz = sizeof(size_t);
	sssz = sizeof(ssize_t);
	cpsz = sizeof(const char *);
	u32sz = sizeof(uint32_t);
	i64sz = sizeof(int64_t);
	u64sz = sizeof(uint64_t);

	CTL_GET("version", &cpv, const char *);
	emitter_kv(emitter, "version", "Version", emitter_type_string, &cpv);

	/* config. */
	emitter_dict_begin(emitter, "config", "Build-time option settings");
#define CONFIG_WRITE_BOOL(name)						\
	do {								\
		CTL_GET("config."#name, &bv, bool);			\
		emitter_kv(emitter, #name, "config."#name,		\
		    emitter_type_bool, &bv);				\
	} while (0)

	CONFIG_WRITE_BOOL(cache_oblivious);
	CONFIG_WRITE_BOOL(debug);
	CONFIG_WRITE_BOOL(fill);
	CONFIG_WRITE_BOOL(lazy_lock);
	emitter_kv(emitter, "malloc_conf", "config.malloc_conf",
	    emitter_type_string, &config_malloc_conf);

	CONFIG_WRITE_BOOL(opt_safety_checks);
	CONFIG_WRITE_BOOL(prof);
	CONFIG_WRITE_BOOL(prof_libgcc);
	CONFIG_WRITE_BOOL(prof_libunwind);
	CONFIG_WRITE_BOOL(stats);
	CONFIG_WRITE_BOOL(utrace);
	CONFIG_WRITE_BOOL(xmalloc);
#undef CONFIG_WRITE_BOOL
	emitter_dict_end(emitter); /* Close "config" dict. */

	/* opt. */
#define OPT_WRITE(name, var, size, emitter_type)			\
	if (je_mallctl("opt."name, (void *)&var, &size, NULL, 0) ==	\
	    0) {							\
		emitter_kv(emitter, name, "opt."name, emitter_type,	\
		    &var);						\
	}

#define OPT_WRITE_MUTABLE(name, var1, var2, size, emitter_type,		\
    altname)								\
	if (je_mallctl("opt."name, (void *)&var1, &size, NULL, 0) ==	\
	    0 && je_mallctl(altname, (void *)&var2, &size, NULL, 0)	\
	    == 0) {							\
		emitter_kv_note(emitter, name, "opt."name,		\
		    emitter_type, &var1, altname, emitter_type,		\
		    &var2);						\
	}

#define OPT_WRITE_BOOL(name) OPT_WRITE(name, bv, bsz, emitter_type_bool)
#define OPT_WRITE_BOOL_MUTABLE(name, altname)				\
	OPT_WRITE_MUTABLE(name, bv, bv2, bsz, emitter_type_bool, altname)

#define OPT_WRITE_UNSIGNED(name)					\
	OPT_WRITE(name, uv, usz, emitter_type_unsigned)

#define OPT_WRITE_INT64(name)						\
	OPT_WRITE(name, i64v, i64sz, emitter_type_int64)
#define OPT_WRITE_UINT64(name)						\
	OPT_WRITE(name, u64v, u64sz, emitter_type_uint64)

#define OPT_WRITE_SIZE_T(name)						\
	OPT_WRITE(name, sv, ssz, emitter_type_size)
#define OPT_WRITE_SSIZE_T(name)						\
	OPT_WRITE(name, ssv, sssz, emitter_type_ssize)
#define OPT_WRITE_SSIZE_T_MUTABLE(name, altname)			\
	OPT_WRITE_MUTABLE(name, ssv, ssv2, sssz, emitter_type_ssize,	\
	    altname)

#define OPT_WRITE_CHAR_P(name)						\
	OPT_WRITE(name, cpv, cpsz, emitter_type_string)

	emitter_dict_begin(emitter, "opt", "Run-time option settings");

	OPT_WRITE_BOOL("abort")
	OPT_WRITE_BOOL("abort_conf")
	OPT_WRITE_BOOL("cache_oblivious")
	OPT_WRITE_BOOL("confirm_conf")
	OPT_WRITE_BOOL("retain")
	OPT_WRITE_CHAR_P("dss")
	OPT_WRITE_UNSIGNED("narenas")
	OPT_WRITE_CHAR_P("percpu_arena")
	OPT_WRITE_SIZE_T("oversize_threshold")
	OPT_WRITE_BOOL("hpa")
	OPT_WRITE_SIZE_T("hpa_slab_max_alloc")
	OPT_WRITE_SIZE_T("hpa_hugification_threshold")
	OPT_WRITE_UINT64("hpa_hugify_delay_ms")
	OPT_WRITE_UINT64("hpa_min_purge_interval_ms")
	if (je_mallctl("opt.hpa_dirty_mult", (void *)&u32v, &u32sz, NULL, 0)
	    == 0) {
		/*
		 * We cheat a little and "know" the secret meaning of this
		 * representation.
		 */
		if (u32v == (uint32_t)-1) {
			const char *neg1 = "-1";
			emitter_kv(emitter, "hpa_dirty_mult",
			    "opt.hpa_dirty_mult", emitter_type_string, &neg1);
		} else {
			char buf[FXP_BUF_SIZE];
			fxp_print(u32v, buf);
			const char *bufp = buf;
			emitter_kv(emitter, "hpa_dirty_mult",
			    "opt.hpa_dirty_mult", emitter_type_string, &bufp);
		}
	}
	OPT_WRITE_SIZE_T("hpa_sec_nshards")
	OPT_WRITE_SIZE_T("hpa_sec_max_alloc")
	OPT_WRITE_SIZE_T("hpa_sec_max_bytes")
	OPT_WRITE_SIZE_T("hpa_sec_bytes_after_flush")
	OPT_WRITE_SIZE_T("hpa_sec_batch_fill_extra")
	OPT_WRITE_CHAR_P("metadata_thp")
	OPT_WRITE_INT64("mutex_max_spin")
	OPT_WRITE_BOOL_MUTABLE("background_thread", "background_thread")
	OPT_WRITE_SSIZE_T_MUTABLE("dirty_decay_ms", "arenas.dirty_decay_ms")
	OPT_WRITE_SSIZE_T_MUTABLE("muzzy_decay_ms", "arenas.muzzy_decay_ms")
	OPT_WRITE_SIZE_T("lg_extent_max_active_fit")
	OPT_WRITE_CHAR_P("junk")
	OPT_WRITE_BOOL("zero")
	OPT_WRITE_BOOL("utrace")
	OPT_WRITE_BOOL("xmalloc")
	OPT_WRITE_BOOL("experimental_infallible_new")
	OPT_WRITE_BOOL("tcache")
	OPT_WRITE_SIZE_T("tcache_max")
	OPT_WRITE_UNSIGNED("tcache_nslots_small_min")
	OPT_WRITE_UNSIGNED("tcache_nslots_small_max")
	OPT_WRITE_UNSIGNED("tcache_nslots_large")
	OPT_WRITE_SSIZE_T("lg_tcache_nslots_mul")
	OPT_WRITE_SIZE_T("tcache_gc_incr_bytes")
	OPT_WRITE_SIZE_T("tcache_gc_delay_bytes")
	OPT_WRITE_UNSIGNED("lg_tcache_flush_small_div")
	OPT_WRITE_UNSIGNED("lg_tcache_flush_large_div")
	OPT_WRITE_CHAR_P("thp")
	OPT_WRITE_BOOL("prof")
	OPT_WRITE_CHAR_P("prof_prefix")
	OPT_WRITE_BOOL_MUTABLE("prof_active", "prof.active")
	OPT_WRITE_BOOL_MUTABLE("prof_thread_active_init",
	    "prof.thread_active_init")
	OPT_WRITE_SSIZE_T_MUTABLE("lg_prof_sample", "prof.lg_sample")
	OPT_WRITE_BOOL("prof_accum")
	OPT_WRITE_SSIZE_T("lg_prof_interval")
	OPT_WRITE_BOOL("prof_gdump")
	OPT_WRITE_BOOL("prof_final")
	OPT_WRITE_BOOL("prof_leak")
	OPT_WRITE_BOOL("prof_leak_error")
	OPT_WRITE_BOOL("stats_print")
	OPT_WRITE_CHAR_P("stats_print_opts")
	OPT_WRITE_BOOL("stats_print")
	OPT_WRITE_CHAR_P("stats_print_opts")
	OPT_WRITE_INT64("stats_interval")
	OPT_WRITE_CHAR_P("stats_interval_opts")
	OPT_WRITE_CHAR_P("zero_realloc")

	emitter_dict_end(emitter);

#undef OPT_WRITE
#undef OPT_WRITE_MUTABLE
#undef OPT_WRITE_BOOL
#undef OPT_WRITE_BOOL_MUTABLE
#undef OPT_WRITE_UNSIGNED
#undef OPT_WRITE_SSIZE_T
#undef OPT_WRITE_SSIZE_T_MUTABLE
#undef OPT_WRITE_CHAR_P

	/* prof. */
	if (config_prof) {
		emitter_dict_begin(emitter, "prof", "Profiling settings");

		CTL_GET("prof.thread_active_init", &bv, bool);
		emitter_kv(emitter, "thread_active_init",
		    "prof.thread_active_init", emitter_type_bool, &bv);

		CTL_GET("prof.active", &bv, bool);
		emitter_kv(emitter, "active", "prof.active", emitter_type_bool,
		    &bv);

		CTL_GET("prof.gdump", &bv, bool);
		emitter_kv(emitter, "gdump", "prof.gdump", emitter_type_bool,
		    &bv);

		CTL_GET("prof.interval", &u64v, uint64_t);
		emitter_kv(emitter, "interval", "prof.interval",
		    emitter_type_uint64, &u64v);

		CTL_GET("prof.lg_sample", &ssv, ssize_t);
		emitter_kv(emitter, "lg_sample", "prof.lg_sample",
		    emitter_type_ssize, &ssv);

		emitter_dict_end(emitter); /* Close "prof". */
	}

	/* arenas. */
	/*
	 * The json output sticks arena info into an "arenas" dict; the table
	 * output puts them at the top-level.
	 */
	emitter_json_object_kv_begin(emitter, "arenas");

	CTL_GET("arenas.narenas", &uv, unsigned);
	emitter_kv(emitter, "narenas", "Arenas", emitter_type_unsigned, &uv);

	/*
	 * Decay settings are emitted only in json mode; in table mode, they're
	 * emitted as notes with the opt output, above.
	 */
	CTL_GET("arenas.dirty_decay_ms", &ssv, ssize_t);
	emitter_json_kv(emitter, "dirty_decay_ms", emitter_type_ssize, &ssv);

	CTL_GET("arenas.muzzy_decay_ms", &ssv, ssize_t);
	emitter_json_kv(emitter, "muzzy_decay_ms", emitter_type_ssize, &ssv);

	CTL_GET("arenas.quantum", &sv, size_t);
	emitter_kv(emitter, "quantum", "Quantum size", emitter_type_size, &sv);

	CTL_GET("arenas.page", &sv, size_t);
	emitter_kv(emitter, "page", "Page size", emitter_type_size, &sv);

	if (je_mallctl("arenas.tcache_max", (void *)&sv, &ssz, NULL, 0) == 0) {
		emitter_kv(emitter, "tcache_max",
		    "Maximum thread-cached size class", emitter_type_size, &sv);
	}

	unsigned arenas_nbins;
	CTL_GET("arenas.nbins", &arenas_nbins, unsigned);
	emitter_kv(emitter, "nbins", "Number of bin size classes",
	    emitter_type_unsigned, &arenas_nbins);

	unsigned arenas_nhbins;
	CTL_GET("arenas.nhbins", &arenas_nhbins, unsigned);
	emitter_kv(emitter, "nhbins", "Number of thread-cache bin size classes",
	    emitter_type_unsigned, &arenas_nhbins);

	/*
	 * We do enough mallctls in a loop that we actually want to omit them
	 * (not just omit the printing).
	 */
	if (emitter_outputs_json(emitter)) {
		emitter_json_array_kv_begin(emitter, "bin");
		size_t arenas_bin_mib[CTL_MAX_DEPTH];
		CTL_LEAF_PREPARE(arenas_bin_mib, 0, "arenas.bin");
		for (unsigned i = 0; i < arenas_nbins; i++) {
			arenas_bin_mib[2] = i;
			emitter_json_object_begin(emitter);

			CTL_LEAF(arenas_bin_mib, 3, "size", &sv, size_t);
			emitter_json_kv(emitter, "size", emitter_type_size,
			    &sv);

			CTL_LEAF(arenas_bin_mib, 3, "nregs", &u32v, uint32_t);
			emitter_json_kv(emitter, "nregs", emitter_type_uint32,
			    &u32v);

			CTL_LEAF(arenas_bin_mib, 3, "slab_size", &sv, size_t);
			emitter_json_kv(emitter, "slab_size", emitter_type_size,
			    &sv);

			CTL_LEAF(arenas_bin_mib, 3, "nshards", &u32v, uint32_t);
			emitter_json_kv(emitter, "nshards", emitter_type_uint32,
			    &u32v);

			emitter_json_object_end(emitter);
		}
		emitter_json_array_end(emitter); /* Close "bin". */
	}

	unsigned nlextents;
	CTL_GET("arenas.nlextents", &nlextents, unsigned);
	emitter_kv(emitter, "nlextents", "Number of large size classes",
	    emitter_type_unsigned, &nlextents);

	if (emitter_outputs_json(emitter)) {
		emitter_json_array_kv_begin(emitter, "lextent");
		size_t arenas_lextent_mib[CTL_MAX_DEPTH];
		CTL_LEAF_PREPARE(arenas_lextent_mib, 0, "arenas.lextent");
		for (unsigned i = 0; i < nlextents; i++) {
			arenas_lextent_mib[2] = i;
			emitter_json_object_begin(emitter);

			CTL_LEAF(arenas_lextent_mib, 3, "size", &sv, size_t);
			emitter_json_kv(emitter, "size", emitter_type_size,
			    &sv);

			emitter_json_object_end(emitter);
		}
		emitter_json_array_end(emitter); /* Close "lextent". */
	}

	emitter_json_object_end(emitter); /* Close "arenas" */
}

JEMALLOC_COLD
static void
stats_print_helper(emitter_t *emitter, bool merged, bool destroyed,
    bool unmerged, bool bins, bool large, bool mutex, bool extents, bool hpa) {
	/*
	 * These should be deleted.  We keep them around for a while, to aid in
	 * the transition to the emitter code.
	 */
	size_t allocated, active, metadata, metadata_thp, resident, mapped,
	    retained;
	size_t num_background_threads;
	size_t zero_reallocs;
	uint64_t background_thread_num_runs, background_thread_run_interval;

	CTL_GET("stats.allocated", &allocated, size_t);
	CTL_GET("stats.active", &active, size_t);
	CTL_GET("stats.metadata", &metadata, size_t);
	CTL_GET("stats.metadata_thp", &metadata_thp, size_t);
	CTL_GET("stats.resident", &resident, size_t);
	CTL_GET("stats.mapped", &mapped, size_t);
	CTL_GET("stats.retained", &retained, size_t);

	CTL_GET("stats.zero_reallocs", &zero_reallocs, size_t);

	if (have_background_thread) {
		CTL_GET("stats.background_thread.num_threads",
		    &num_background_threads, size_t);
		CTL_GET("stats.background_thread.num_runs",
		    &background_thread_num_runs, uint64_t);
		CTL_GET("stats.background_thread.run_interval",
		    &background_thread_run_interval, uint64_t);
	} else {
		num_background_threads = 0;
		background_thread_num_runs = 0;
		background_thread_run_interval = 0;
	}

	/* Generic global stats. */
	emitter_json_object_kv_begin(emitter, "stats");
	emitter_json_kv(emitter, "allocated", emitter_type_size, &allocated);
	emitter_json_kv(emitter, "active", emitter_type_size, &active);
	emitter_json_kv(emitter, "metadata", emitter_type_size, &metadata);
	emitter_json_kv(emitter, "metadata_thp", emitter_type_size,
	    &metadata_thp);
	emitter_json_kv(emitter, "resident", emitter_type_size, &resident);
	emitter_json_kv(emitter, "mapped", emitter_type_size, &mapped);
	emitter_json_kv(emitter, "retained", emitter_type_size, &retained);
	emitter_json_kv(emitter, "zero_reallocs", emitter_type_size,
	    &zero_reallocs);

	emitter_table_printf(emitter, "Allocated: %zu, active: %zu, "
	    "metadata: %zu (n_thp %zu), resident: %zu, mapped: %zu, "
	    "retained: %zu\n", allocated, active, metadata, metadata_thp,
	    resident, mapped, retained);

	/* Strange behaviors */
	emitter_table_printf(emitter,
	    "Count of realloc(non-null-ptr, 0) calls: %zu\n", zero_reallocs);

	/* Background thread stats. */
	emitter_json_object_kv_begin(emitter, "background_thread");
	emitter_json_kv(emitter, "num_threads", emitter_type_size,
	    &num_background_threads);
	emitter_json_kv(emitter, "num_runs", emitter_type_uint64,
	    &background_thread_num_runs);
	emitter_json_kv(emitter, "run_interval", emitter_type_uint64,
	    &background_thread_run_interval);
	emitter_json_object_end(emitter); /* Close "background_thread". */

	emitter_table_printf(emitter, "Background threads: %zu, "
	    "num_runs: %"FMTu64", run_interval: %"FMTu64" ns\n",
	    num_background_threads, background_thread_num_runs,
	    background_thread_run_interval);

	if (mutex) {
		emitter_row_t row;
		emitter_col_t name;
		emitter_col_t col64[mutex_prof_num_uint64_t_counters];
		emitter_col_t col32[mutex_prof_num_uint32_t_counters];
		uint64_t uptime;

		emitter_row_init(&row);
		mutex_stats_init_cols(&row, "", &name, col64, col32);

		emitter_table_row(emitter, &row);
		emitter_json_object_kv_begin(emitter, "mutexes");

		CTL_M2_GET("stats.arenas.0.uptime", 0, &uptime, uint64_t);

		size_t stats_mutexes_mib[CTL_MAX_DEPTH];
		CTL_LEAF_PREPARE(stats_mutexes_mib, 0, "stats.mutexes");
		for (int i = 0; i < mutex_prof_num_global_mutexes; i++) {
			mutex_stats_read_global(stats_mutexes_mib, 2,
			    global_mutex_names[i], &name, col64, col32, uptime);
			emitter_json_object_kv_begin(emitter, global_mutex_names[i]);
			mutex_stats_emit(emitter, &row, col64, col32);
			emitter_json_object_end(emitter);
		}

		emitter_json_object_end(emitter); /* Close "mutexes". */
	}

	emitter_json_object_end(emitter); /* Close "stats". */

	if (merged || destroyed || unmerged) {
		unsigned narenas;

		emitter_json_object_kv_begin(emitter, "stats.arenas");

		CTL_GET("arenas.narenas", &narenas, unsigned);
		size_t mib[3];
		size_t miblen = sizeof(mib) / sizeof(size_t);
		size_t sz;
		VARIABLE_ARRAY(bool, initialized, narenas);
		bool destroyed_initialized;
		unsigned i, j, ninitialized;

		xmallctlnametomib("arena.0.initialized", mib, &miblen);
		for (i = ninitialized = 0; i < narenas; i++) {
			mib[1] = i;
			sz = sizeof(bool);
			xmallctlbymib(mib, miblen, &initialized[i], &sz,
			    NULL, 0);
			if (initialized[i]) {
				ninitialized++;
			}
		}
		mib[1] = MALLCTL_ARENAS_DESTROYED;
		sz = sizeof(bool);
		xmallctlbymib(mib, miblen, &destroyed_initialized, &sz,
		    NULL, 0);

		/* Merged stats. */
		if (merged && (ninitialized > 1 || !unmerged)) {
			/* Print merged arena stats. */
			emitter_table_printf(emitter, "Merged arenas stats:\n");
			emitter_json_object_kv_begin(emitter, "merged");
			stats_arena_print(emitter, MALLCTL_ARENAS_ALL, bins,
			    large, mutex, extents, hpa);
			emitter_json_object_end(emitter); /* Close "merged". */
		}

		/* Destroyed stats. */
		if (destroyed_initialized && destroyed) {
			/* Print destroyed arena stats. */
			emitter_table_printf(emitter,
			    "Destroyed arenas stats:\n");
			emitter_json_object_kv_begin(emitter, "destroyed");
			stats_arena_print(emitter, MALLCTL_ARENAS_DESTROYED,
			    bins, large, mutex, extents, hpa);
			emitter_json_object_end(emitter); /* Close "destroyed". */
		}

		/* Unmerged stats. */
		if (unmerged) {
			for (i = j = 0; i < narenas; i++) {
				if (initialized[i]) {
					char arena_ind_str[20];
					malloc_snprintf(arena_ind_str,
					    sizeof(arena_ind_str), "%u", i);
					emitter_json_object_kv_begin(emitter,
					    arena_ind_str);
					emitter_table_printf(emitter,
					    "arenas[%s]:\n", arena_ind_str);
					stats_arena_print(emitter, i, bins,
					    large, mutex, extents, hpa);
					/* Close "<arena-ind>". */
					emitter_json_object_end(emitter);
				}
			}
		}
		emitter_json_object_end(emitter); /* Close "stats.arenas". */
	}
}

void
stats_print(write_cb_t *write_cb, void *cbopaque, const char *opts) {
	int err;
	uint64_t epoch;
	size_t u64sz;
#define OPTION(o, v, d, s) bool v = d;
	STATS_PRINT_OPTIONS
#undef OPTION

	/*
	 * Refresh stats, in case mallctl() was called by the application.
	 *
	 * Check for OOM here, since refreshing the ctl cache can trigger
	 * allocation.  In practice, none of the subsequent mallctl()-related
	 * calls in this function will cause OOM if this one succeeds.
	 * */
	epoch = 1;
	u64sz = sizeof(uint64_t);
	err = je_mallctl("epoch", (void *)&epoch, &u64sz, (void *)&epoch,
	    sizeof(uint64_t));
	if (err != 0) {
		if (err == EAGAIN) {
			malloc_write("<jemalloc>: Memory allocation failure in "
			    "mallctl(\"epoch\", ...)\n");
			return;
		}
		malloc_write("<jemalloc>: Failure in mallctl(\"epoch\", "
		    "...)\n");
		abort();
	}

	if (opts != NULL) {
		for (unsigned i = 0; opts[i] != '\0'; i++) {
			switch (opts[i]) {
#define OPTION(o, v, d, s) case o: v = s; break;
				STATS_PRINT_OPTIONS
#undef OPTION
			default:;
			}
		}
	}

	emitter_t emitter;
	emitter_init(&emitter,
	    json ? emitter_output_json_compact : emitter_output_table,
	    write_cb, cbopaque);
	emitter_begin(&emitter);
	emitter_table_printf(&emitter, "___ Begin jemalloc statistics ___\n");
	emitter_json_object_kv_begin(&emitter, "jemalloc");

	if (general) {
		stats_general_print(&emitter);
	}
	if (config_stats) {
		stats_print_helper(&emitter, merged, destroyed, unmerged,
		    bins, large, mutex, extents, hpa);
	}

	emitter_json_object_end(&emitter); /* Closes the "jemalloc" dict. */
	emitter_table_printf(&emitter, "--- End jemalloc statistics ---\n");
	emitter_end(&emitter);
}

uint64_t
stats_interval_new_event_wait(tsd_t *tsd) {
	return stats_interval_accum_batch;
}

uint64_t
stats_interval_postponed_event_wait(tsd_t *tsd) {
	return TE_MIN_START_WAIT;
}

void
stats_interval_event_handler(tsd_t *tsd, uint64_t elapsed) {
	assert(elapsed > 0 && elapsed != TE_INVALID_ELAPSED);
	if (counter_accum(tsd_tsdn(tsd), &stats_interval_accumulated,
	    elapsed)) {
		je_malloc_stats_print(NULL, NULL, opt_stats_interval_opts);
	}
}

bool
stats_boot(void) {
	uint64_t stats_interval;
	if (opt_stats_interval < 0) {
		assert(opt_stats_interval == -1);
		stats_interval = 0;
		stats_interval_accum_batch = 0;
	} else{
		/* See comments in stats.h */
		stats_interval = (opt_stats_interval > 0) ?
		    opt_stats_interval : 1;
		uint64_t batch = stats_interval >>
		    STATS_INTERVAL_ACCUM_LG_BATCH_SIZE;
		if (batch > STATS_INTERVAL_ACCUM_BATCH_MAX) {
			batch = STATS_INTERVAL_ACCUM_BATCH_MAX;
		} else if (batch == 0) {
			batch = 1;
		}
		stats_interval_accum_batch = batch;
	}

	return counter_accum_init(&stats_interval_accumulated, stats_interval);
}

void
stats_prefork(tsdn_t *tsdn) {
	counter_prefork(tsdn, &stats_interval_accumulated);
}

void
stats_postfork_parent(tsdn_t *tsdn) {
	counter_postfork_parent(tsdn, &stats_interval_accumulated);
}

void
stats_postfork_child(tsdn_t *tsdn) {
	counter_postfork_child(tsdn, &stats_interval_accumulated);
}
