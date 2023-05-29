#ifndef JEMALLOC_INTERNAL_FB_H
#define JEMALLOC_INTERNAL_FB_H

/*
 * The flat bitmap module.  This has a larger API relative to the bitmap module
 * (supporting things like backwards searches, and searching for both set and
 * unset bits), at the cost of slower operations for very large bitmaps.
 *
 * Initialized flat bitmaps start at all-zeros (all bits unset).
 */

typedef unsigned long fb_group_t;
#define FB_GROUP_BITS (ZU(1) << (LG_SIZEOF_LONG + 3))
#define FB_NGROUPS(nbits) ((nbits) / FB_GROUP_BITS \
    + ((nbits) % FB_GROUP_BITS == 0 ? 0 : 1))

static inline void
fb_init(fb_group_t *fb, size_t nbits) {
	size_t ngroups = FB_NGROUPS(nbits);
	memset(fb, 0, ngroups * sizeof(fb_group_t));
}

static inline bool
fb_empty(fb_group_t *fb, size_t nbits) {
	size_t ngroups = FB_NGROUPS(nbits);
	for (size_t i = 0; i < ngroups; i++) {
		if (fb[i] != 0) {
			return false;
		}
	}
	return true;
}

static inline bool
fb_full(fb_group_t *fb, size_t nbits) {
	size_t ngroups = FB_NGROUPS(nbits);
	size_t trailing_bits = nbits % FB_GROUP_BITS;
	size_t limit = (trailing_bits == 0 ? ngroups : ngroups - 1);
	for (size_t i = 0; i < limit; i++) {
		if (fb[i] != ~(fb_group_t)0) {
			return false;
		}
	}
	if (trailing_bits == 0) {
		return true;
	}
	return fb[ngroups - 1] == ((fb_group_t)1 << trailing_bits) - 1;
}

static inline bool
fb_get(fb_group_t *fb, size_t nbits, size_t bit) {
	assert(bit < nbits);
	size_t group_ind = bit / FB_GROUP_BITS;
	size_t bit_ind = bit % FB_GROUP_BITS;
	return (bool)(fb[group_ind] & ((fb_group_t)1 << bit_ind));
}

static inline void
fb_set(fb_group_t *fb, size_t nbits, size_t bit) {
	assert(bit < nbits);
	size_t group_ind = bit / FB_GROUP_BITS;
	size_t bit_ind = bit % FB_GROUP_BITS;
	fb[group_ind] |= ((fb_group_t)1 << bit_ind);
}

static inline void
fb_unset(fb_group_t *fb, size_t nbits, size_t bit) {
	assert(bit < nbits);
	size_t group_ind = bit / FB_GROUP_BITS;
	size_t bit_ind = bit % FB_GROUP_BITS;
	fb[group_ind] &= ~((fb_group_t)1 << bit_ind);
}


/*
 * Some implementation details.  This visitation function lets us apply a group
 * visitor to each group in the bitmap (potentially modifying it).  The mask
 * indicates which bits are logically part of the visitation.
 */
typedef void (*fb_group_visitor_t)(void *ctx, fb_group_t *fb, fb_group_t mask);
JEMALLOC_ALWAYS_INLINE void
fb_visit_impl(fb_group_t *fb, size_t nbits, fb_group_visitor_t visit, void *ctx,
    size_t start, size_t cnt) {
	assert(cnt > 0);
	assert(start + cnt <= nbits);
	size_t group_ind = start / FB_GROUP_BITS;
	size_t start_bit_ind = start % FB_GROUP_BITS;
	/*
	 * The first group is special; it's the only one we don't start writing
	 * to from bit 0.
	 */
	size_t first_group_cnt = (start_bit_ind + cnt > FB_GROUP_BITS
		? FB_GROUP_BITS - start_bit_ind : cnt);
	/*
	 * We can basically split affected words into:
	 *   - The first group, where we touch only the high bits
	 *   - The last group, where we touch only the low bits
	 *   - The middle, where we set all the bits to the same thing.
	 * We treat each case individually.  The last two could be merged, but
	 * this can lead to bad codegen for those middle words.
	 */
	/* First group */
	fb_group_t mask = ((~(fb_group_t)0)
	    >> (FB_GROUP_BITS - first_group_cnt))
	    << start_bit_ind;
	visit(ctx, &fb[group_ind], mask);

	cnt -= first_group_cnt;
	group_ind++;
	/* Middle groups */
	while (cnt > FB_GROUP_BITS) {
		visit(ctx, &fb[group_ind], ~(fb_group_t)0);
		cnt -= FB_GROUP_BITS;
		group_ind++;
	}
	/* Last group */
	if (cnt != 0) {
		mask = (~(fb_group_t)0) >> (FB_GROUP_BITS - cnt);
		visit(ctx, &fb[group_ind], mask);
	}
}

JEMALLOC_ALWAYS_INLINE void
fb_assign_visitor(void *ctx, fb_group_t *fb, fb_group_t mask) {
	bool val = *(bool *)ctx;
	if (val) {
		*fb |= mask;
	} else {
		*fb &= ~mask;
	}
}

/* Sets the cnt bits starting at position start.  Must not have a 0 count. */
static inline void
fb_set_range(fb_group_t *fb, size_t nbits, size_t start, size_t cnt) {
	bool val = true;
	fb_visit_impl(fb, nbits, &fb_assign_visitor, &val, start, cnt);
}

/* Unsets the cnt bits starting at position start.  Must not have a 0 count. */
static inline void
fb_unset_range(fb_group_t *fb, size_t nbits, size_t start, size_t cnt) {
	bool val = false;
	fb_visit_impl(fb, nbits, &fb_assign_visitor, &val, start, cnt);
}

JEMALLOC_ALWAYS_INLINE void
fb_scount_visitor(void *ctx, fb_group_t *fb, fb_group_t mask) {
	size_t *scount = (size_t *)ctx;
	*scount += popcount_lu(*fb & mask);
}

/* Finds the number of set bit in the of length cnt starting at start. */
JEMALLOC_ALWAYS_INLINE size_t
fb_scount(fb_group_t *fb, size_t nbits, size_t start, size_t cnt) {
	size_t scount = 0;
	fb_visit_impl(fb, nbits, &fb_scount_visitor, &scount, start, cnt);
	return scount;
}

/* Finds the number of unset bit in the of length cnt starting at start. */
JEMALLOC_ALWAYS_INLINE size_t
fb_ucount(fb_group_t *fb, size_t nbits, size_t start, size_t cnt) {
	size_t scount = fb_scount(fb, nbits, start, cnt);
	return cnt - scount;
}

/*
 * An implementation detail; find the first bit at position >= min_bit with the
 * value val.
 *
 * Returns the number of bits in the bitmap if no such bit exists.
 */
JEMALLOC_ALWAYS_INLINE ssize_t
fb_find_impl(fb_group_t *fb, size_t nbits, size_t start, bool val,
    bool forward) {
	assert(start < nbits);
	size_t ngroups = FB_NGROUPS(nbits);
	ssize_t group_ind = start / FB_GROUP_BITS;
	size_t bit_ind = start % FB_GROUP_BITS;

	fb_group_t maybe_invert = (val ? 0 : (fb_group_t)-1);

	fb_group_t group = fb[group_ind];
	group ^= maybe_invert;
	if (forward) {
		/* Only keep ones in bits bit_ind and above. */
		group &= ~((1LU << bit_ind) - 1);
	} else {
		/*
		 * Only keep ones in bits bit_ind and below.  You might more
		 * naturally express this as (1 << (bit_ind + 1)) - 1, but
		 * that shifts by an invalid amount if bit_ind is one less than
		 * FB_GROUP_BITS.
		 */
		group &= ((2LU << bit_ind) - 1);
	}
	ssize_t group_ind_bound = forward ? (ssize_t)ngroups : -1;
	while (group == 0) {
		group_ind += forward ? 1 : -1;
		if (group_ind == group_ind_bound) {
			return forward ? (ssize_t)nbits : (ssize_t)-1;
		}
		group = fb[group_ind];
		group ^= maybe_invert;
	}
	assert(group != 0);
	size_t bit = forward ? ffs_lu(group) : fls_lu(group);
	size_t pos = group_ind * FB_GROUP_BITS + bit;
	/*
	 * The high bits of a partially filled last group are zeros, so if we're
	 * looking for zeros we don't want to report an invalid result.
	 */
	if (forward && !val && pos > nbits) {
		return nbits;
	}
	return pos;
}

/*
 * Find the first set bit in the bitmap with an index >= min_bit.  Returns the
 * number of bits in the bitmap if no such bit exists.
 */
static inline size_t
fb_ffu(fb_group_t *fb, size_t nbits, size_t min_bit) {
	return (size_t)fb_find_impl(fb, nbits, min_bit, /* val */ false,
	    /* forward */ true);
}

/* The same, but looks for an unset bit. */
static inline size_t
fb_ffs(fb_group_t *fb, size_t nbits, size_t min_bit) {
	return (size_t)fb_find_impl(fb, nbits, min_bit, /* val */ true,
	    /* forward */ true);
}

/*
 * Find the last set bit in the bitmap with an index <= max_bit.  Returns -1 if
 * no such bit exists.
 */
static inline ssize_t
fb_flu(fb_group_t *fb, size_t nbits, size_t max_bit) {
	return fb_find_impl(fb, nbits, max_bit, /* val */ false,
	    /* forward */ false);
}

static inline ssize_t
fb_fls(fb_group_t *fb, size_t nbits, size_t max_bit) {
	return fb_find_impl(fb, nbits, max_bit, /* val */ true,
	    /* forward */ false);
}

/* Returns whether or not we found a range. */
JEMALLOC_ALWAYS_INLINE bool
fb_iter_range_impl(fb_group_t *fb, size_t nbits, size_t start, size_t *r_begin,
    size_t *r_len, bool val, bool forward) {
	assert(start < nbits);
	ssize_t next_range_begin = fb_find_impl(fb, nbits, start, val, forward);
	if ((forward && next_range_begin == (ssize_t)nbits)
	    || (!forward && next_range_begin == (ssize_t)-1)) {
		return false;
	}
	/* Half open range; the set bits are [begin, end). */
	ssize_t next_range_end = fb_find_impl(fb, nbits, next_range_begin, !val,
	    forward);
	if (forward) {
		*r_begin = next_range_begin;
		*r_len = next_range_end - next_range_begin;
	} else {
		*r_begin = next_range_end + 1;
		*r_len = next_range_begin - next_range_end;
	}
	return true;
}

/*
 * Used to iterate through ranges of set bits.
 *
 * Tries to find the next contiguous sequence of set bits with a first index >=
 * start.  If one exists, puts the earliest bit of the range in *r_begin, its
 * length in *r_len, and returns true.  Otherwise, returns false (without
 * touching *r_begin or *r_end).
 */
static inline bool
fb_srange_iter(fb_group_t *fb, size_t nbits, size_t start, size_t *r_begin,
    size_t *r_len) {
	return fb_iter_range_impl(fb, nbits, start, r_begin, r_len,
	    /* val */ true, /* forward */ true);
}

/*
 * The same as fb_srange_iter, but searches backwards from start rather than
 * forwards.  (The position returned is still the earliest bit in the range).
 */
static inline bool
fb_srange_riter(fb_group_t *fb, size_t nbits, size_t start, size_t *r_begin,
    size_t *r_len) {
	return fb_iter_range_impl(fb, nbits, start, r_begin, r_len,
	    /* val */ true, /* forward */ false);
}

/* Similar to fb_srange_iter, but searches for unset bits. */
static inline bool
fb_urange_iter(fb_group_t *fb, size_t nbits, size_t start, size_t *r_begin,
    size_t *r_len) {
	return fb_iter_range_impl(fb, nbits, start, r_begin, r_len,
	    /* val */ false, /* forward */ true);
}

/* Similar to fb_srange_riter, but searches for unset bits. */
static inline bool
fb_urange_riter(fb_group_t *fb, size_t nbits, size_t start, size_t *r_begin,
    size_t *r_len) {
	return fb_iter_range_impl(fb, nbits, start, r_begin, r_len,
	    /* val */ false, /* forward */ false);
}

JEMALLOC_ALWAYS_INLINE size_t
fb_range_longest_impl(fb_group_t *fb, size_t nbits, bool val) {
	size_t begin = 0;
	size_t longest_len = 0;
	size_t len = 0;
	while (begin < nbits && fb_iter_range_impl(fb, nbits, begin, &begin,
	    &len, val, /* forward */ true)) {
		if (len > longest_len) {
			longest_len = len;
		}
		begin += len;
	}
	return longest_len;
}

static inline size_t
fb_srange_longest(fb_group_t *fb, size_t nbits) {
	return fb_range_longest_impl(fb, nbits, /* val */ true);
}

static inline size_t
fb_urange_longest(fb_group_t *fb, size_t nbits) {
	return fb_range_longest_impl(fb, nbits, /* val */ false);
}

/*
 * Initializes each bit of dst with the bitwise-AND of the corresponding bits of
 * src1 and src2.  All bitmaps must be the same size.
 */
static inline void
fb_bit_and(fb_group_t *dst, fb_group_t *src1, fb_group_t *src2, size_t nbits) {
	size_t ngroups = FB_NGROUPS(nbits);
	for (size_t i = 0; i < ngroups; i++) {
		dst[i] = src1[i] & src2[i];
	}
}

/* Like fb_bit_and, but with bitwise-OR. */
static inline void
fb_bit_or(fb_group_t *dst, fb_group_t *src1, fb_group_t *src2, size_t nbits) {
	size_t ngroups = FB_NGROUPS(nbits);
	for (size_t i = 0; i < ngroups; i++) {
		dst[i] = src1[i] | src2[i];
	}
}

/* Initializes dst bit i to the negation of source bit i. */
static inline void
fb_bit_not(fb_group_t *dst, fb_group_t *src, size_t nbits) {
	size_t ngroups = FB_NGROUPS(nbits);
	for (size_t i = 0; i < ngroups; i++) {
		dst[i] = ~src[i];
	}
}

#endif /* JEMALLOC_INTERNAL_FB_H */
