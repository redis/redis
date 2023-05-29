#ifndef JEMALLOC_INTERNAL_SC_H
#define JEMALLOC_INTERNAL_SC_H

#include "jemalloc/internal/jemalloc_internal_types.h"

/*
 * Size class computations:
 *
 * These are a little tricky; we'll first start by describing how things
 * generally work, and then describe some of the details.
 *
 * Ignore the first few size classes for a moment. We can then split all the
 * remaining size classes into groups. The size classes in a group are spaced
 * such that they cover allocation request sizes in a power-of-2 range. The
 * power of two is called the base of the group, and the size classes in it
 * satisfy allocations in the half-open range (base, base * 2]. There are
 * SC_NGROUP size classes in each group, equally spaced in the range, so that
 * each one covers allocations for base / SC_NGROUP possible allocation sizes.
 * We call that value (base / SC_NGROUP) the delta of the group. Each size class
 * is delta larger than the one before it (including the initial size class in a
 * group, which is delta larger than base, the largest size class in the
 * previous group).
 * To make the math all work out nicely, we require that SC_NGROUP is a power of
 * two, and define it in terms of SC_LG_NGROUP. We'll often talk in terms of
 * lg_base and lg_delta. For each of these groups then, we have that
 * lg_delta == lg_base - SC_LG_NGROUP.
 * The size classes in a group with a given lg_base and lg_delta (which, recall,
 * can be computed from lg_base for these groups) are therefore:
 *   base + 1 * delta
 *     which covers allocations in (base, base + 1 * delta]
 *   base + 2 * delta
 *     which covers allocations in (base + 1 * delta, base + 2 * delta].
 *   base + 3 * delta
 *     which covers allocations in (base + 2 * delta, base + 3 * delta].
 *   ...
 *   base + SC_NGROUP * delta ( == 2 * base)
 *     which covers allocations in (base + (SC_NGROUP - 1) * delta, 2 * base].
 * (Note that currently SC_NGROUP is always 4, so the "..." is empty in
 * practice.)
 * Note that the last size class in the group is the next power of two (after
 * base), so that we've set up the induction correctly for the next group's
 * selection of delta.
 *
 * Now, let's start considering the first few size classes. Two extra constants
 * come into play here: LG_QUANTUM and SC_LG_TINY_MIN. LG_QUANTUM ensures
 * correct platform alignment; all objects of size (1 << LG_QUANTUM) or larger
 * are at least (1 << LG_QUANTUM) aligned; this can be used to ensure that we
 * never return improperly aligned memory, by making (1 << LG_QUANTUM) equal the
 * highest required alignment of a platform. For allocation sizes smaller than
 * (1 << LG_QUANTUM) though, we can be more relaxed (since we don't support
 * platforms with types with alignment larger than their size). To allow such
 * allocations (without wasting space unnecessarily), we introduce tiny size
 * classes; one per power of two, up until we hit the quantum size. There are
 * therefore LG_QUANTUM - SC_LG_TINY_MIN such size classes.
 *
 * Next, we have a size class of size (1 << LG_QUANTUM).  This can't be the
 * start of a group in the sense we described above (covering a power of two
 * range) since, if we divided into it to pick a value of delta, we'd get a
 * delta smaller than (1 << LG_QUANTUM) for sizes >= (1 << LG_QUANTUM), which
 * is against the rules.
 *
 * The first base we can divide by SC_NGROUP while still being at least
 * (1 << LG_QUANTUM) is SC_NGROUP * (1 << LG_QUANTUM). We can get there by
 * having SC_NGROUP size classes, spaced (1 << LG_QUANTUM) apart. These size
 * classes are:
 *   1 * (1 << LG_QUANTUM)
 *   2 * (1 << LG_QUANTUM)
 *   3 * (1 << LG_QUANTUM)
 *   ... (although, as above, this "..." is empty in practice)
 *   SC_NGROUP * (1 << LG_QUANTUM).
 *
 * There are SC_NGROUP of these size classes, so we can regard it as a sort of
 * pseudo-group, even though it spans multiple powers of 2, is divided
 * differently, and both starts and ends on a power of 2 (as opposed to just
 * ending). SC_NGROUP is itself a power of two, so the first group after the
 * pseudo-group has the power-of-two base SC_NGROUP * (1 << LG_QUANTUM), for a
 * lg_base of LG_QUANTUM + SC_LG_NGROUP. We can divide this base into SC_NGROUP
 * sizes without violating our LG_QUANTUM requirements, so we can safely set
 * lg_delta = lg_base - SC_LG_GROUP (== LG_QUANTUM).
 *
 * So, in order, the size classes are:
 *
 * Tiny size classes:
 * - Count: LG_QUANTUM - SC_LG_TINY_MIN.
 * - Sizes:
 *     1 << SC_LG_TINY_MIN
 *     1 << (SC_LG_TINY_MIN + 1)
 *     1 << (SC_LG_TINY_MIN + 2)
 *     ...
 *     1 << (LG_QUANTUM - 1)
 *
 * Initial pseudo-group:
 * - Count: SC_NGROUP
 * - Sizes:
 *     1 * (1 << LG_QUANTUM)
 *     2 * (1 << LG_QUANTUM)
 *     3 * (1 << LG_QUANTUM)
 *     ...
 *     SC_NGROUP * (1 << LG_QUANTUM)
 *
 * Regular group 0:
 * - Count: SC_NGROUP
 * - Sizes:
 *   (relative to lg_base of LG_QUANTUM + SC_LG_NGROUP and lg_delta of
 *   lg_base - SC_LG_NGROUP)
 *     (1 << lg_base) + 1 * (1 << lg_delta)
 *     (1 << lg_base) + 2 * (1 << lg_delta)
 *     (1 << lg_base) + 3 * (1 << lg_delta)
 *     ...
 *     (1 << lg_base) + SC_NGROUP * (1 << lg_delta) [ == (1 << (lg_base + 1)) ]
 *
 * Regular group 1:
 * - Count: SC_NGROUP
 * - Sizes:
 *   (relative to lg_base of LG_QUANTUM + SC_LG_NGROUP + 1 and lg_delta of
 *   lg_base - SC_LG_NGROUP)
 *     (1 << lg_base) + 1 * (1 << lg_delta)
 *     (1 << lg_base) + 2 * (1 << lg_delta)
 *     (1 << lg_base) + 3 * (1 << lg_delta)
 *     ...
 *     (1 << lg_base) + SC_NGROUP * (1 << lg_delta) [ == (1 << (lg_base + 1)) ]
 *
 * ...
 *
 * Regular group N:
 * - Count: SC_NGROUP
 * - Sizes:
 *   (relative to lg_base of LG_QUANTUM + SC_LG_NGROUP + N and lg_delta of
 *   lg_base - SC_LG_NGROUP)
 *     (1 << lg_base) + 1 * (1 << lg_delta)
 *     (1 << lg_base) + 2 * (1 << lg_delta)
 *     (1 << lg_base) + 3 * (1 << lg_delta)
 *     ...
 *     (1 << lg_base) + SC_NGROUP * (1 << lg_delta) [ == (1 << (lg_base + 1)) ]
 *
 *
 * Representation of metadata:
 * To make the math easy, we'll mostly work in lg quantities. We record lg_base,
 * lg_delta, and ndelta (i.e. number of deltas above the base) on a
 * per-size-class basis, and maintain the invariant that, across all size
 * classes, size == (1 << lg_base) + ndelta * (1 << lg_delta).
 *
 * For regular groups (i.e. those with lg_base >= LG_QUANTUM + SC_LG_NGROUP),
 * lg_delta is lg_base - SC_LG_NGROUP, and ndelta goes from 1 to SC_NGROUP.
 *
 * For the initial tiny size classes (if any), lg_base is lg(size class size).
 * lg_delta is lg_base for the first size class, and lg_base - 1 for all
 * subsequent ones. ndelta is always 0.
 *
 * For the pseudo-group, if there are no tiny size classes, then we set
 * lg_base == LG_QUANTUM, lg_delta == LG_QUANTUM, and have ndelta range from 0
 * to SC_NGROUP - 1. (Note that delta == base, so base + (SC_NGROUP - 1) * delta
 * is just SC_NGROUP * base, or (1 << (SC_LG_NGROUP + LG_QUANTUM)), so we do
 * indeed get a power of two that way). If there *are* tiny size classes, then
 * the first size class needs to have lg_delta relative to the largest tiny size
 * class. We therefore set lg_base == LG_QUANTUM - 1,
 * lg_delta == LG_QUANTUM - 1, and ndelta == 1, keeping the rest of the
 * pseudo-group the same.
 *
 *
 * Other terminology:
 * "Small" size classes mean those that are allocated out of bins, which is the
 * same as those that are slab allocated.
 * "Large" size classes are those that are not small. The cutoff for counting as
 * large is page size * group size.
 */

/*
 * Size class N + (1 << SC_LG_NGROUP) twice the size of size class N.
 */
#define SC_LG_NGROUP 2
#define SC_LG_TINY_MIN 3

#if SC_LG_TINY_MIN == 0
/* The div module doesn't support division by 1, which this would require. */
#error "Unsupported LG_TINY_MIN"
#endif

/*
 * The definitions below are all determined by the above settings and system
 * characteristics.
 */
#define SC_NGROUP (1ULL << SC_LG_NGROUP)
#define SC_PTR_BITS ((1ULL << LG_SIZEOF_PTR) * 8)
#define SC_NTINY (LG_QUANTUM - SC_LG_TINY_MIN)
#define SC_LG_TINY_MAXCLASS (LG_QUANTUM > SC_LG_TINY_MIN ? LG_QUANTUM - 1 : -1)
#define SC_NPSEUDO SC_NGROUP
#define SC_LG_FIRST_REGULAR_BASE (LG_QUANTUM + SC_LG_NGROUP)
/*
 * We cap allocations to be less than 2 ** (ptr_bits - 1), so the highest base
 * we need is 2 ** (ptr_bits - 2). (This also means that the last group is 1
 * size class shorter than the others).
 * We could probably save some space in arenas by capping this at LG_VADDR size.
 */
#define SC_LG_BASE_MAX (SC_PTR_BITS - 2)
#define SC_NREGULAR (SC_NGROUP * 					\
    (SC_LG_BASE_MAX - SC_LG_FIRST_REGULAR_BASE + 1) - 1)
#define SC_NSIZES (SC_NTINY + SC_NPSEUDO + SC_NREGULAR)

/*
 * The number of size classes that are a multiple of the page size.
 *
 * Here are the first few bases that have a page-sized SC.
 *
 *      lg(base) |     base | highest SC | page-multiple SCs
 * --------------|------------------------------------------
 *   LG_PAGE - 1 | PAGE / 2 |       PAGE | 1
 *       LG_PAGE |     PAGE |   2 * PAGE | 1
 *   LG_PAGE + 1 | 2 * PAGE |   4 * PAGE | 2
 *   LG_PAGE + 2 | 4 * PAGE |   8 * PAGE | 4
 *
 * The number of page-multiple SCs continues to grow in powers of two, up until
 * lg_delta == lg_page, which corresponds to setting lg_base to lg_page +
 * SC_LG_NGROUP.  So, then, the number of size classes that are multiples of the
 * page size whose lg_delta is less than the page size are
 * is 1 + (2**0 + 2**1 + ... + 2**(lg_ngroup - 1) == 2**lg_ngroup.
 *
 * For each base with lg_base in [lg_page + lg_ngroup, lg_base_max), there are
 * NGROUP page-sized size classes, and when lg_base == lg_base_max, there are
 * NGROUP - 1.
 *
 * This gives us the quantity we seek.
 */
#define SC_NPSIZES (							\
    SC_NGROUP								\
    + (SC_LG_BASE_MAX - (LG_PAGE + SC_LG_NGROUP)) * SC_NGROUP		\
    + SC_NGROUP - 1)

/*
 * We declare a size class is binnable if size < page size * group. Or, in other
 * words, lg(size) < lg(page size) + lg(group size).
 */
#define SC_NBINS (							\
    /* Sub-regular size classes. */					\
    SC_NTINY + SC_NPSEUDO						\
    /* Groups with lg_regular_min_base <= lg_base <= lg_base_max */	\
    + SC_NGROUP * (LG_PAGE + SC_LG_NGROUP - SC_LG_FIRST_REGULAR_BASE)	\
    /* Last SC of the last group hits the bound exactly; exclude it. */	\
    - 1)

/*
 * The size2index_tab lookup table uses uint8_t to encode each bin index, so we
 * cannot support more than 256 small size classes.
 */
#if (SC_NBINS > 256)
#  error "Too many small size classes"
#endif

/* The largest size class in the lookup table, and its binary log. */
#define SC_LG_MAX_LOOKUP 12
#define SC_LOOKUP_MAXCLASS (1 << SC_LG_MAX_LOOKUP)

/* Internal, only used for the definition of SC_SMALL_MAXCLASS. */
#define SC_SMALL_MAX_BASE (1 << (LG_PAGE + SC_LG_NGROUP - 1))
#define SC_SMALL_MAX_DELTA (1 << (LG_PAGE - 1))

/* The largest size class allocated out of a slab. */
#define SC_SMALL_MAXCLASS (SC_SMALL_MAX_BASE				\
    + (SC_NGROUP - 1) * SC_SMALL_MAX_DELTA)

/* The fastpath assumes all lookup-able sizes are small. */
#if (SC_SMALL_MAXCLASS < SC_LOOKUP_MAXCLASS)
#  error "Lookup table sizes must be small"
#endif

/* The smallest size class not allocated out of a slab. */
#define SC_LARGE_MINCLASS ((size_t)1ULL << (LG_PAGE + SC_LG_NGROUP))
#define SC_LG_LARGE_MINCLASS (LG_PAGE + SC_LG_NGROUP)

/* Internal; only used for the definition of SC_LARGE_MAXCLASS. */
#define SC_MAX_BASE ((size_t)1 << (SC_PTR_BITS - 2))
#define SC_MAX_DELTA ((size_t)1 << (SC_PTR_BITS - 2 - SC_LG_NGROUP))

/* The largest size class supported. */
#define SC_LARGE_MAXCLASS (SC_MAX_BASE + (SC_NGROUP - 1) * SC_MAX_DELTA)

/* Maximum number of regions in one slab. */
#ifndef CONFIG_LG_SLAB_MAXREGS
#  define SC_LG_SLAB_MAXREGS (LG_PAGE - SC_LG_TINY_MIN)
#else
#  if CONFIG_LG_SLAB_MAXREGS < (LG_PAGE - SC_LG_TINY_MIN)
#    error "Unsupported SC_LG_SLAB_MAXREGS"
#  else
#    define SC_LG_SLAB_MAXREGS CONFIG_LG_SLAB_MAXREGS
#  endif
#endif

#define SC_SLAB_MAXREGS (1U << SC_LG_SLAB_MAXREGS)

typedef struct sc_s sc_t;
struct sc_s {
	/* Size class index, or -1 if not a valid size class. */
	int index;
	/* Lg group base size (no deltas added). */
	int lg_base;
	/* Lg delta to previous size class. */
	int lg_delta;
	/* Delta multiplier.  size == 1<<lg_base + ndelta<<lg_delta */
	int ndelta;
	/*
	 * True if the size class is a multiple of the page size, false
	 * otherwise.
	 */
	bool psz;
	/*
	 * True if the size class is a small, bin, size class. False otherwise.
	 */
	bool bin;
	/* The slab page count if a small bin size class, 0 otherwise. */
	int pgs;
	/* Same as lg_delta if a lookup table size class, 0 otherwise. */
	int lg_delta_lookup;
};

typedef struct sc_data_s sc_data_t;
struct sc_data_s {
	/* Number of tiny size classes. */
	unsigned ntiny;
	/* Number of bins supported by the lookup table. */
	int nlbins;
	/* Number of small size class bins. */
	int nbins;
	/* Number of size classes. */
	int nsizes;
	/* Number of bits required to store NSIZES. */
	int lg_ceil_nsizes;
	/* Number of size classes that are a multiple of (1U << LG_PAGE). */
	unsigned npsizes;
	/* Lg of maximum tiny size class (or -1, if none). */
	int lg_tiny_maxclass;
	/* Maximum size class included in lookup table. */
	size_t lookup_maxclass;
	/* Maximum small size class. */
	size_t small_maxclass;
	/* Lg of minimum large size class. */
	int lg_large_minclass;
	/* The minimum large size class. */
	size_t large_minclass;
	/* Maximum (large) size class. */
	size_t large_maxclass;
	/* True if the sc_data_t has been initialized (for debugging only). */
	bool initialized;

	sc_t sc[SC_NSIZES];
};

size_t reg_size_compute(int lg_base, int lg_delta, int ndelta);
void sc_data_init(sc_data_t *data);
/*
 * Updates slab sizes in [begin, end] to be pgs pages in length, if possible.
 * Otherwise, does its best to accommodate the request.
 */
void sc_data_update_slab_size(sc_data_t *data, size_t begin, size_t end,
    int pgs);
void sc_boot(sc_data_t *data);

#endif /* JEMALLOC_INTERNAL_SC_H */
