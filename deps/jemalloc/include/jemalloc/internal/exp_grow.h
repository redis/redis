#ifndef JEMALLOC_INTERNAL_EXP_GROW_H
#define JEMALLOC_INTERNAL_EXP_GROW_H

typedef struct exp_grow_s exp_grow_t;
struct exp_grow_s {
	/*
	 * Next extent size class in a growing series to use when satisfying a
	 * request via the extent hooks (only if opt_retain).  This limits the
	 * number of disjoint virtual memory ranges so that extent merging can
	 * be effective even if multiple arenas' extent allocation requests are
	 * highly interleaved.
	 *
	 * retain_grow_limit is the max allowed size ind to expand (unless the
	 * required size is greater).  Default is no limit, and controlled
	 * through mallctl only.
	 */
	pszind_t next;
	pszind_t limit;
};

static inline bool
exp_grow_size_prepare(exp_grow_t *exp_grow, size_t alloc_size_min,
    size_t *r_alloc_size, pszind_t *r_skip) {
	*r_skip = 0;
	*r_alloc_size = sz_pind2sz(exp_grow->next + *r_skip);
	while (*r_alloc_size < alloc_size_min) {
		(*r_skip)++;
		if (exp_grow->next + *r_skip  >=
		    sz_psz2ind(SC_LARGE_MAXCLASS)) {
			/* Outside legal range. */
			return true;
		}
		*r_alloc_size = sz_pind2sz(exp_grow->next + *r_skip);
	}
	return false;
}

static inline void
exp_grow_size_commit(exp_grow_t *exp_grow, pszind_t skip) {
	if (exp_grow->next + skip + 1 <= exp_grow->limit) {
		exp_grow->next += skip + 1;
	} else {
		exp_grow->next = exp_grow->limit;
	}

}

void exp_grow_init(exp_grow_t *exp_grow);

#endif /* JEMALLOC_INTERNAL_EXP_GROW_H */
