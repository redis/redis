#ifndef JEMALLOC_INTERNAL_EDATA_H
#define JEMALLOC_INTERNAL_EDATA_H

#include "jemalloc/internal/atomic.h"
#include "jemalloc/internal/bin_info.h"
#include "jemalloc/internal/bit_util.h"
#include "jemalloc/internal/hpdata.h"
#include "jemalloc/internal/nstime.h"
#include "jemalloc/internal/ph.h"
#include "jemalloc/internal/ql.h"
#include "jemalloc/internal/sc.h"
#include "jemalloc/internal/slab_data.h"
#include "jemalloc/internal/sz.h"
#include "jemalloc/internal/typed_list.h"

/*
 * sizeof(edata_t) is 128 bytes on 64-bit architectures.  Ensure the alignment
 * to free up the low bits in the rtree leaf.
 */
#define EDATA_ALIGNMENT 128

enum extent_state_e {
	extent_state_active   = 0,
	extent_state_dirty    = 1,
	extent_state_muzzy    = 2,
	extent_state_retained = 3,
	extent_state_transition = 4, /* States below are intermediate. */
	extent_state_merging = 5,
	extent_state_max = 5 /* Sanity checking only. */
};
typedef enum extent_state_e extent_state_t;

enum extent_head_state_e {
	EXTENT_NOT_HEAD,
	EXTENT_IS_HEAD   /* See comments in ehooks_default_merge_impl(). */
};
typedef enum extent_head_state_e extent_head_state_t;

/*
 * Which implementation of the page allocator interface, (PAI, defined in
 * pai.h) owns the given extent?
 */
enum extent_pai_e {
	EXTENT_PAI_PAC = 0,
	EXTENT_PAI_HPA = 1
};
typedef enum extent_pai_e extent_pai_t;

struct e_prof_info_s {
	/* Time when this was allocated. */
	nstime_t	e_prof_alloc_time;
	/* Allocation request size. */
	size_t		e_prof_alloc_size;
	/* Points to a prof_tctx_t. */
	atomic_p_t	e_prof_tctx;
	/*
	 * Points to a prof_recent_t for the allocation; NULL
	 * means the recent allocation record no longer exists.
	 * Protected by prof_recent_alloc_mtx.
	 */
	atomic_p_t	e_prof_recent_alloc;
};
typedef struct e_prof_info_s e_prof_info_t;

/*
 * The information about a particular edata that lives in an emap.  Space is
 * more precious there (the information, plus the edata pointer, has to live in
 * a 64-bit word if we want to enable a packed representation.
 *
 * There are two things that are special about the information here:
 * - It's quicker to access.  You have one fewer pointer hop, since finding the
 *   edata_t associated with an item always requires accessing the rtree leaf in
 *   which this data is stored.
 * - It can be read unsynchronized, and without worrying about lifetime issues.
 */
typedef struct edata_map_info_s edata_map_info_t;
struct edata_map_info_s {
	bool slab;
	szind_t szind;
};

typedef struct edata_cmp_summary_s edata_cmp_summary_t;
struct edata_cmp_summary_s {
	uint64_t sn;
	uintptr_t addr;
};

/* Extent (span of pages).  Use accessor functions for e_* fields. */
typedef struct edata_s edata_t;
ph_structs(edata_avail, edata_t);
ph_structs(edata_heap, edata_t);
struct edata_s {
	/*
	 * Bitfield containing several fields:
	 *
	 * a: arena_ind
	 * b: slab
	 * c: committed
	 * p: pai
	 * z: zeroed
	 * g: guarded
	 * t: state
	 * i: szind
	 * f: nfree
	 * s: bin_shard
	 *
	 * 00000000 ... 0000ssss ssffffff ffffiiii iiiitttg zpcbaaaa aaaaaaaa
	 *
	 * arena_ind: Arena from which this extent came, or all 1 bits if
	 *            unassociated.
	 *
	 * slab: The slab flag indicates whether the extent is used for a slab
	 *       of small regions.  This helps differentiate small size classes,
	 *       and it indicates whether interior pointers can be looked up via
	 *       iealloc().
	 *
	 * committed: The committed flag indicates whether physical memory is
	 *            committed to the extent, whether explicitly or implicitly
	 *            as on a system that overcommits and satisfies physical
	 *            memory needs on demand via soft page faults.
	 *
	 * pai: The pai flag is an extent_pai_t.
	 *
	 * zeroed: The zeroed flag is used by extent recycling code to track
	 *         whether memory is zero-filled.
	 *
	 * guarded: The guarded flag is use by the sanitizer to track whether
	 *          the extent has page guards around it.
	 *
	 * state: The state flag is an extent_state_t.
	 *
	 * szind: The szind flag indicates usable size class index for
	 *        allocations residing in this extent, regardless of whether the
	 *        extent is a slab.  Extent size and usable size often differ
	 *        even for non-slabs, either due to sz_large_pad or promotion of
	 *        sampled small regions.
	 *
	 * nfree: Number of free regions in slab.
	 *
	 * bin_shard: the shard of the bin from which this extent came.
	 */
	uint64_t		e_bits;
#define MASK(CURRENT_FIELD_WIDTH, CURRENT_FIELD_SHIFT) ((((((uint64_t)0x1U) << (CURRENT_FIELD_WIDTH)) - 1)) << (CURRENT_FIELD_SHIFT))

#define EDATA_BITS_ARENA_WIDTH  MALLOCX_ARENA_BITS
#define EDATA_BITS_ARENA_SHIFT  0
#define EDATA_BITS_ARENA_MASK  MASK(EDATA_BITS_ARENA_WIDTH, EDATA_BITS_ARENA_SHIFT)

#define EDATA_BITS_SLAB_WIDTH  1
#define EDATA_BITS_SLAB_SHIFT  (EDATA_BITS_ARENA_WIDTH + EDATA_BITS_ARENA_SHIFT)
#define EDATA_BITS_SLAB_MASK  MASK(EDATA_BITS_SLAB_WIDTH, EDATA_BITS_SLAB_SHIFT)

#define EDATA_BITS_COMMITTED_WIDTH  1
#define EDATA_BITS_COMMITTED_SHIFT  (EDATA_BITS_SLAB_WIDTH + EDATA_BITS_SLAB_SHIFT)
#define EDATA_BITS_COMMITTED_MASK  MASK(EDATA_BITS_COMMITTED_WIDTH, EDATA_BITS_COMMITTED_SHIFT)

#define EDATA_BITS_PAI_WIDTH  1
#define EDATA_BITS_PAI_SHIFT  (EDATA_BITS_COMMITTED_WIDTH + EDATA_BITS_COMMITTED_SHIFT)
#define EDATA_BITS_PAI_MASK  MASK(EDATA_BITS_PAI_WIDTH, EDATA_BITS_PAI_SHIFT)

#define EDATA_BITS_ZEROED_WIDTH  1
#define EDATA_BITS_ZEROED_SHIFT  (EDATA_BITS_PAI_WIDTH + EDATA_BITS_PAI_SHIFT)
#define EDATA_BITS_ZEROED_MASK  MASK(EDATA_BITS_ZEROED_WIDTH, EDATA_BITS_ZEROED_SHIFT)

#define EDATA_BITS_GUARDED_WIDTH  1
#define EDATA_BITS_GUARDED_SHIFT  (EDATA_BITS_ZEROED_WIDTH + EDATA_BITS_ZEROED_SHIFT)
#define EDATA_BITS_GUARDED_MASK  MASK(EDATA_BITS_GUARDED_WIDTH, EDATA_BITS_GUARDED_SHIFT)

#define EDATA_BITS_STATE_WIDTH  3
#define EDATA_BITS_STATE_SHIFT  (EDATA_BITS_GUARDED_WIDTH + EDATA_BITS_GUARDED_SHIFT)
#define EDATA_BITS_STATE_MASK  MASK(EDATA_BITS_STATE_WIDTH, EDATA_BITS_STATE_SHIFT)

#define EDATA_BITS_SZIND_WIDTH  LG_CEIL(SC_NSIZES)
#define EDATA_BITS_SZIND_SHIFT  (EDATA_BITS_STATE_WIDTH + EDATA_BITS_STATE_SHIFT)
#define EDATA_BITS_SZIND_MASK  MASK(EDATA_BITS_SZIND_WIDTH, EDATA_BITS_SZIND_SHIFT)

#define EDATA_BITS_NFREE_WIDTH  (SC_LG_SLAB_MAXREGS + 1)
#define EDATA_BITS_NFREE_SHIFT  (EDATA_BITS_SZIND_WIDTH + EDATA_BITS_SZIND_SHIFT)
#define EDATA_BITS_NFREE_MASK  MASK(EDATA_BITS_NFREE_WIDTH, EDATA_BITS_NFREE_SHIFT)

#define EDATA_BITS_BINSHARD_WIDTH  6
#define EDATA_BITS_BINSHARD_SHIFT  (EDATA_BITS_NFREE_WIDTH + EDATA_BITS_NFREE_SHIFT)
#define EDATA_BITS_BINSHARD_MASK  MASK(EDATA_BITS_BINSHARD_WIDTH, EDATA_BITS_BINSHARD_SHIFT)

#define EDATA_BITS_IS_HEAD_WIDTH 1
#define EDATA_BITS_IS_HEAD_SHIFT  (EDATA_BITS_BINSHARD_WIDTH + EDATA_BITS_BINSHARD_SHIFT)
#define EDATA_BITS_IS_HEAD_MASK  MASK(EDATA_BITS_IS_HEAD_WIDTH, EDATA_BITS_IS_HEAD_SHIFT)

	/* Pointer to the extent that this structure is responsible for. */
	void			*e_addr;

	union {
		/*
		 * Extent size and serial number associated with the extent
		 * structure (different than the serial number for the extent at
		 * e_addr).
		 *
		 * ssssssss [...] ssssssss ssssnnnn nnnnnnnn
		 */
		size_t			e_size_esn;
	#define EDATA_SIZE_MASK	((size_t)~(PAGE-1))
	#define EDATA_ESN_MASK		((size_t)PAGE-1)
		/* Base extent size, which may not be a multiple of PAGE. */
		size_t			e_bsize;
	};

	/*
	 * If this edata is a user allocation from an HPA, it comes out of some
	 * pageslab (we don't yet support huegpage allocations that don't fit
	 * into pageslabs).  This tracks it.
	 */
	hpdata_t *e_ps;

	/*
	 * Serial number.  These are not necessarily unique; splitting an extent
	 * results in two extents with the same serial number.
	 */
	uint64_t e_sn;

	union {
		/*
		 * List linkage used when the edata_t is active; either in
		 * arena's large allocations or bin_t's slabs_full.
		 */
		ql_elm(edata_t)	ql_link_active;
		/*
		 * Pairing heap linkage.  Used whenever the extent is inactive
		 * (in the page allocators), or when it is active and in
		 * slabs_nonfull, or when the edata_t is unassociated with an
		 * extent and sitting in an edata_cache.
		 */
		union {
			edata_heap_link_t heap_link;
			edata_avail_link_t avail_link;
		};
	};

	union {
		/*
		 * List linkage used when the extent is inactive:
		 * - Stashed dirty extents
		 * - Ecache LRU functionality.
		 */
		ql_elm(edata_t) ql_link_inactive;
		/* Small region slab metadata. */
		slab_data_t	e_slab_data;

		/* Profiling data, used for large objects. */
		e_prof_info_t	e_prof_info;
	};
};

TYPED_LIST(edata_list_active, edata_t, ql_link_active)
TYPED_LIST(edata_list_inactive, edata_t, ql_link_inactive)

static inline unsigned
edata_arena_ind_get(const edata_t *edata) {
	unsigned arena_ind = (unsigned)((edata->e_bits &
	    EDATA_BITS_ARENA_MASK) >> EDATA_BITS_ARENA_SHIFT);
	assert(arena_ind < MALLOCX_ARENA_LIMIT);

	return arena_ind;
}

static inline szind_t
edata_szind_get_maybe_invalid(const edata_t *edata) {
	szind_t szind = (szind_t)((edata->e_bits & EDATA_BITS_SZIND_MASK) >>
	    EDATA_BITS_SZIND_SHIFT);
	assert(szind <= SC_NSIZES);
	return szind;
}

static inline szind_t
edata_szind_get(const edata_t *edata) {
	szind_t szind = edata_szind_get_maybe_invalid(edata);
	assert(szind < SC_NSIZES); /* Never call when "invalid". */
	return szind;
}

static inline size_t
edata_usize_get(const edata_t *edata) {
	return sz_index2size(edata_szind_get(edata));
}

static inline unsigned
edata_binshard_get(const edata_t *edata) {
	unsigned binshard = (unsigned)((edata->e_bits &
	    EDATA_BITS_BINSHARD_MASK) >> EDATA_BITS_BINSHARD_SHIFT);
	assert(binshard < bin_infos[edata_szind_get(edata)].n_shards);
	return binshard;
}

static inline uint64_t
edata_sn_get(const edata_t *edata) {
	return edata->e_sn;
}

static inline extent_state_t
edata_state_get(const edata_t *edata) {
	return (extent_state_t)((edata->e_bits & EDATA_BITS_STATE_MASK) >>
	    EDATA_BITS_STATE_SHIFT);
}

static inline bool
edata_guarded_get(const edata_t *edata) {
	return (bool)((edata->e_bits & EDATA_BITS_GUARDED_MASK) >>
	    EDATA_BITS_GUARDED_SHIFT);
}

static inline bool
edata_zeroed_get(const edata_t *edata) {
	return (bool)((edata->e_bits & EDATA_BITS_ZEROED_MASK) >>
	    EDATA_BITS_ZEROED_SHIFT);
}

static inline bool
edata_committed_get(const edata_t *edata) {
	return (bool)((edata->e_bits & EDATA_BITS_COMMITTED_MASK) >>
	    EDATA_BITS_COMMITTED_SHIFT);
}

static inline extent_pai_t
edata_pai_get(const edata_t *edata) {
	return (extent_pai_t)((edata->e_bits & EDATA_BITS_PAI_MASK) >>
	    EDATA_BITS_PAI_SHIFT);
}

static inline bool
edata_slab_get(const edata_t *edata) {
	return (bool)((edata->e_bits & EDATA_BITS_SLAB_MASK) >>
	    EDATA_BITS_SLAB_SHIFT);
}

static inline unsigned
edata_nfree_get(const edata_t *edata) {
	assert(edata_slab_get(edata));
	return (unsigned)((edata->e_bits & EDATA_BITS_NFREE_MASK) >>
	    EDATA_BITS_NFREE_SHIFT);
}

static inline void *
edata_base_get(const edata_t *edata) {
	assert(edata->e_addr == PAGE_ADDR2BASE(edata->e_addr) ||
	    !edata_slab_get(edata));
	return PAGE_ADDR2BASE(edata->e_addr);
}

static inline void *
edata_addr_get(const edata_t *edata) {
	assert(edata->e_addr == PAGE_ADDR2BASE(edata->e_addr) ||
	    !edata_slab_get(edata));
	return edata->e_addr;
}

static inline size_t
edata_size_get(const edata_t *edata) {
	return (edata->e_size_esn & EDATA_SIZE_MASK);
}

static inline size_t
edata_esn_get(const edata_t *edata) {
	return (edata->e_size_esn & EDATA_ESN_MASK);
}

static inline size_t
edata_bsize_get(const edata_t *edata) {
	return edata->e_bsize;
}

static inline hpdata_t *
edata_ps_get(const edata_t *edata) {
	assert(edata_pai_get(edata) == EXTENT_PAI_HPA);
	return edata->e_ps;
}

static inline void *
edata_before_get(const edata_t *edata) {
	return (void *)((uintptr_t)edata_base_get(edata) - PAGE);
}

static inline void *
edata_last_get(const edata_t *edata) {
	return (void *)((uintptr_t)edata_base_get(edata) +
	    edata_size_get(edata) - PAGE);
}

static inline void *
edata_past_get(const edata_t *edata) {
	return (void *)((uintptr_t)edata_base_get(edata) +
	    edata_size_get(edata));
}

static inline slab_data_t *
edata_slab_data_get(edata_t *edata) {
	assert(edata_slab_get(edata));
	return &edata->e_slab_data;
}

static inline const slab_data_t *
edata_slab_data_get_const(const edata_t *edata) {
	assert(edata_slab_get(edata));
	return &edata->e_slab_data;
}

static inline prof_tctx_t *
edata_prof_tctx_get(const edata_t *edata) {
	return (prof_tctx_t *)atomic_load_p(&edata->e_prof_info.e_prof_tctx,
	    ATOMIC_ACQUIRE);
}

static inline const nstime_t *
edata_prof_alloc_time_get(const edata_t *edata) {
	return &edata->e_prof_info.e_prof_alloc_time;
}

static inline size_t
edata_prof_alloc_size_get(const edata_t *edata) {
	return edata->e_prof_info.e_prof_alloc_size;
}

static inline prof_recent_t *
edata_prof_recent_alloc_get_dont_call_directly(const edata_t *edata) {
	return (prof_recent_t *)atomic_load_p(
	    &edata->e_prof_info.e_prof_recent_alloc, ATOMIC_RELAXED);
}

static inline void
edata_arena_ind_set(edata_t *edata, unsigned arena_ind) {
	edata->e_bits = (edata->e_bits & ~EDATA_BITS_ARENA_MASK) |
	    ((uint64_t)arena_ind << EDATA_BITS_ARENA_SHIFT);
}

static inline void
edata_binshard_set(edata_t *edata, unsigned binshard) {
	/* The assertion assumes szind is set already. */
	assert(binshard < bin_infos[edata_szind_get(edata)].n_shards);
	edata->e_bits = (edata->e_bits & ~EDATA_BITS_BINSHARD_MASK) |
	    ((uint64_t)binshard << EDATA_BITS_BINSHARD_SHIFT);
}

static inline void
edata_addr_set(edata_t *edata, void *addr) {
	edata->e_addr = addr;
}

static inline void
edata_size_set(edata_t *edata, size_t size) {
	assert((size & ~EDATA_SIZE_MASK) == 0);
	edata->e_size_esn = size | (edata->e_size_esn & ~EDATA_SIZE_MASK);
}

static inline void
edata_esn_set(edata_t *edata, size_t esn) {
	edata->e_size_esn = (edata->e_size_esn & ~EDATA_ESN_MASK) | (esn &
	    EDATA_ESN_MASK);
}

static inline void
edata_bsize_set(edata_t *edata, size_t bsize) {
	edata->e_bsize = bsize;
}

static inline void
edata_ps_set(edata_t *edata, hpdata_t *ps) {
	assert(edata_pai_get(edata) == EXTENT_PAI_HPA);
	edata->e_ps = ps;
}

static inline void
edata_szind_set(edata_t *edata, szind_t szind) {
	assert(szind <= SC_NSIZES); /* SC_NSIZES means "invalid". */
	edata->e_bits = (edata->e_bits & ~EDATA_BITS_SZIND_MASK) |
	    ((uint64_t)szind << EDATA_BITS_SZIND_SHIFT);
}

static inline void
edata_nfree_set(edata_t *edata, unsigned nfree) {
	assert(edata_slab_get(edata));
	edata->e_bits = (edata->e_bits & ~EDATA_BITS_NFREE_MASK) |
	    ((uint64_t)nfree << EDATA_BITS_NFREE_SHIFT);
}

static inline void
edata_nfree_binshard_set(edata_t *edata, unsigned nfree, unsigned binshard) {
	/* The assertion assumes szind is set already. */
	assert(binshard < bin_infos[edata_szind_get(edata)].n_shards);
	edata->e_bits = (edata->e_bits &
	    (~EDATA_BITS_NFREE_MASK & ~EDATA_BITS_BINSHARD_MASK)) |
	    ((uint64_t)binshard << EDATA_BITS_BINSHARD_SHIFT) |
	    ((uint64_t)nfree << EDATA_BITS_NFREE_SHIFT);
}

static inline void
edata_nfree_inc(edata_t *edata) {
	assert(edata_slab_get(edata));
	edata->e_bits += ((uint64_t)1U << EDATA_BITS_NFREE_SHIFT);
}

static inline void
edata_nfree_dec(edata_t *edata) {
	assert(edata_slab_get(edata));
	edata->e_bits -= ((uint64_t)1U << EDATA_BITS_NFREE_SHIFT);
}

static inline void
edata_nfree_sub(edata_t *edata, uint64_t n) {
	assert(edata_slab_get(edata));
	edata->e_bits -= (n << EDATA_BITS_NFREE_SHIFT);
}

static inline void
edata_sn_set(edata_t *edata, uint64_t sn) {
	edata->e_sn = sn;
}

static inline void
edata_state_set(edata_t *edata, extent_state_t state) {
	edata->e_bits = (edata->e_bits & ~EDATA_BITS_STATE_MASK) |
	    ((uint64_t)state << EDATA_BITS_STATE_SHIFT);
}

static inline void
edata_guarded_set(edata_t *edata, bool guarded) {
	edata->e_bits = (edata->e_bits & ~EDATA_BITS_GUARDED_MASK) |
	    ((uint64_t)guarded << EDATA_BITS_GUARDED_SHIFT);
}

static inline void
edata_zeroed_set(edata_t *edata, bool zeroed) {
	edata->e_bits = (edata->e_bits & ~EDATA_BITS_ZEROED_MASK) |
	    ((uint64_t)zeroed << EDATA_BITS_ZEROED_SHIFT);
}

static inline void
edata_committed_set(edata_t *edata, bool committed) {
	edata->e_bits = (edata->e_bits & ~EDATA_BITS_COMMITTED_MASK) |
	    ((uint64_t)committed << EDATA_BITS_COMMITTED_SHIFT);
}

static inline void
edata_pai_set(edata_t *edata, extent_pai_t pai) {
	edata->e_bits = (edata->e_bits & ~EDATA_BITS_PAI_MASK) |
	    ((uint64_t)pai << EDATA_BITS_PAI_SHIFT);
}

static inline void
edata_slab_set(edata_t *edata, bool slab) {
	edata->e_bits = (edata->e_bits & ~EDATA_BITS_SLAB_MASK) |
	    ((uint64_t)slab << EDATA_BITS_SLAB_SHIFT);
}

static inline void
edata_prof_tctx_set(edata_t *edata, prof_tctx_t *tctx) {
	atomic_store_p(&edata->e_prof_info.e_prof_tctx, tctx, ATOMIC_RELEASE);
}

static inline void
edata_prof_alloc_time_set(edata_t *edata, nstime_t *t) {
	nstime_copy(&edata->e_prof_info.e_prof_alloc_time, t);
}

static inline void
edata_prof_alloc_size_set(edata_t *edata, size_t size) {
	edata->e_prof_info.e_prof_alloc_size = size;
}

static inline void
edata_prof_recent_alloc_set_dont_call_directly(edata_t *edata,
    prof_recent_t *recent_alloc) {
	atomic_store_p(&edata->e_prof_info.e_prof_recent_alloc, recent_alloc,
	    ATOMIC_RELAXED);
}

static inline bool
edata_is_head_get(edata_t *edata) {
	return (bool)((edata->e_bits & EDATA_BITS_IS_HEAD_MASK) >>
	    EDATA_BITS_IS_HEAD_SHIFT);
}

static inline void
edata_is_head_set(edata_t *edata, bool is_head) {
	edata->e_bits = (edata->e_bits & ~EDATA_BITS_IS_HEAD_MASK) |
	    ((uint64_t)is_head << EDATA_BITS_IS_HEAD_SHIFT);
}

static inline bool
edata_state_in_transition(extent_state_t state) {
	return state >= extent_state_transition;
}

/*
 * Because this function is implemented as a sequence of bitfield modifications,
 * even though each individual bit is properly initialized, we technically read
 * uninitialized data within it.  This is mostly fine, since most callers get
 * their edatas from zeroing sources, but callers who make stack edata_ts need
 * to manually zero them.
 */
static inline void
edata_init(edata_t *edata, unsigned arena_ind, void *addr, size_t size,
    bool slab, szind_t szind, uint64_t sn, extent_state_t state, bool zeroed,
    bool committed, extent_pai_t pai, extent_head_state_t is_head) {
	assert(addr == PAGE_ADDR2BASE(addr) || !slab);

	edata_arena_ind_set(edata, arena_ind);
	edata_addr_set(edata, addr);
	edata_size_set(edata, size);
	edata_slab_set(edata, slab);
	edata_szind_set(edata, szind);
	edata_sn_set(edata, sn);
	edata_state_set(edata, state);
	edata_guarded_set(edata, false);
	edata_zeroed_set(edata, zeroed);
	edata_committed_set(edata, committed);
	edata_pai_set(edata, pai);
	edata_is_head_set(edata, is_head == EXTENT_IS_HEAD);
	if (config_prof) {
		edata_prof_tctx_set(edata, NULL);
	}
}

static inline void
edata_binit(edata_t *edata, void *addr, size_t bsize, uint64_t sn) {
	edata_arena_ind_set(edata, (1U << MALLOCX_ARENA_BITS) - 1);
	edata_addr_set(edata, addr);
	edata_bsize_set(edata, bsize);
	edata_slab_set(edata, false);
	edata_szind_set(edata, SC_NSIZES);
	edata_sn_set(edata, sn);
	edata_state_set(edata, extent_state_active);
	edata_guarded_set(edata, false);
	edata_zeroed_set(edata, true);
	edata_committed_set(edata, true);
	/*
	 * This isn't strictly true, but base allocated extents never get
	 * deallocated and can't be looked up in the emap, but no sense in
	 * wasting a state bit to encode this fact.
	 */
	edata_pai_set(edata, EXTENT_PAI_PAC);
}

static inline int
edata_esn_comp(const edata_t *a, const edata_t *b) {
	size_t a_esn = edata_esn_get(a);
	size_t b_esn = edata_esn_get(b);

	return (a_esn > b_esn) - (a_esn < b_esn);
}

static inline int
edata_ead_comp(const edata_t *a, const edata_t *b) {
	uintptr_t a_eaddr = (uintptr_t)a;
	uintptr_t b_eaddr = (uintptr_t)b;

	return (a_eaddr > b_eaddr) - (a_eaddr < b_eaddr);
}

static inline edata_cmp_summary_t
edata_cmp_summary_get(const edata_t *edata) {
	return (edata_cmp_summary_t){edata_sn_get(edata),
		(uintptr_t)edata_addr_get(edata)};
}

static inline int
edata_cmp_summary_comp(edata_cmp_summary_t a, edata_cmp_summary_t b) {
	int ret;
	ret = (a.sn > b.sn) - (a.sn < b.sn);
	if (ret != 0) {
		return ret;
	}
	ret = (a.addr > b.addr) - (a.addr < b.addr);
	return ret;
}

static inline int
edata_snad_comp(const edata_t *a, const edata_t *b) {
	edata_cmp_summary_t a_cmp = edata_cmp_summary_get(a);
	edata_cmp_summary_t b_cmp = edata_cmp_summary_get(b);

	return edata_cmp_summary_comp(a_cmp, b_cmp);
}

static inline int
edata_esnead_comp(const edata_t *a, const edata_t *b) {
	int ret;

	ret = edata_esn_comp(a, b);
	if (ret != 0) {
		return ret;
	}

	ret = edata_ead_comp(a, b);
	return ret;
}

ph_proto(, edata_avail, edata_t)
ph_proto(, edata_heap, edata_t)

#endif /* JEMALLOC_INTERNAL_EDATA_H */
