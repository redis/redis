#include "jemalloc/internal/jemalloc_preamble.h"

#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/bit_util.h"
#include "jemalloc/internal/bitmap.h"
#include "jemalloc/internal/pages.h"
#include "jemalloc/internal/sc.h"

/*
 * This module computes the size classes used to satisfy allocations.  The logic
 * here was ported more or less line-by-line from a shell script, and because of
 * that is not the most idiomatic C.  Eventually we should fix this, but for now
 * at least the damage is compartmentalized to this file.
 */

size_t
reg_size_compute(int lg_base, int lg_delta, int ndelta) {
	return (ZU(1) << lg_base) + (ZU(ndelta) << lg_delta);
}

/* Returns the number of pages in the slab. */
static int
slab_size(int lg_page, int lg_base, int lg_delta, int ndelta) {
	size_t page = (ZU(1) << lg_page);
	size_t reg_size = reg_size_compute(lg_base, lg_delta, ndelta);

	size_t try_slab_size = page;
	size_t try_nregs = try_slab_size / reg_size;
	size_t perfect_slab_size = 0;
	bool perfect = false;
	/*
	 * This loop continues until we find the least common multiple of the
	 * page size and size class size.  Size classes are all of the form
	 * base + ndelta * delta == (ndelta + base/ndelta) * delta, which is
	 * (ndelta + ngroup) * delta.  The way we choose slabbing strategies
	 * means that delta is at most the page size and ndelta < ngroup.  So
	 * the loop executes for at most 2 * ngroup - 1 iterations, which is
	 * also the bound on the number of pages in a slab chosen by default.
	 * With the current default settings, this is at most 7.
	 */
	while (!perfect) {
		perfect_slab_size = try_slab_size;
		size_t perfect_nregs = try_nregs;
		try_slab_size += page;
		try_nregs = try_slab_size / reg_size;
		if (perfect_slab_size == perfect_nregs * reg_size) {
			perfect = true;
		}
	}
	return (int)(perfect_slab_size / page);
}

static void
size_class(
    /* Output. */
    sc_t *sc,
    /* Configuration decisions. */
    int lg_max_lookup, int lg_page, int lg_ngroup,
    /* Inputs specific to the size class. */
    int index, int lg_base, int lg_delta, int ndelta) {
	sc->index = index;
	sc->lg_base = lg_base;
	sc->lg_delta = lg_delta;
	sc->ndelta = ndelta;
	size_t size = reg_size_compute(lg_base, lg_delta, ndelta);
	sc->psz = (size % (ZU(1) << lg_page) == 0);
	if (index == 0) {
		assert(!sc->psz);
	}
	if (size < (ZU(1) << (lg_page + lg_ngroup))) {
		sc->bin = true;
		sc->pgs = slab_size(lg_page, lg_base, lg_delta, ndelta);
	} else {
		sc->bin = false;
		sc->pgs = 0;
	}
	if (size <= (ZU(1) << lg_max_lookup)) {
		sc->lg_delta_lookup = lg_delta;
	} else {
		sc->lg_delta_lookup = 0;
	}
}

static void
size_classes(
    /* Output. */
    sc_data_t *sc_data,
    /* Determined by the system. */
    size_t lg_ptr_size, int lg_quantum,
    /* Configuration decisions. */
    int lg_tiny_min, int lg_max_lookup, int lg_page, int lg_ngroup) {
	int ptr_bits = (1 << lg_ptr_size) * 8;
	int ngroup = (1 << lg_ngroup);
	int ntiny = 0;
	int nlbins = 0;
	int lg_tiny_maxclass = (unsigned)-1;
	int nbins = 0;
	int npsizes = 0;

	int index = 0;

	int ndelta = 0;
	int lg_base = lg_tiny_min;
	int lg_delta = lg_base;

	/* Outputs that we update as we go. */
	size_t lookup_maxclass = 0;
	size_t small_maxclass = 0;
	int lg_large_minclass = 0;
	size_t large_maxclass = 0;

	/* Tiny size classes. */
	while (lg_base < lg_quantum) {
		sc_t *sc = &sc_data->sc[index];
		size_class(sc, lg_max_lookup, lg_page, lg_ngroup, index,
		    lg_base, lg_delta, ndelta);
		if (sc->lg_delta_lookup != 0) {
			nlbins = index + 1;
		}
		if (sc->psz) {
			npsizes++;
		}
		if (sc->bin) {
			nbins++;
		}
		ntiny++;
		/* Final written value is correct. */
		lg_tiny_maxclass = lg_base;
		index++;
		lg_delta = lg_base;
		lg_base++;
	}

	/* First non-tiny (pseudo) group. */
	if (ntiny != 0) {
		sc_t *sc = &sc_data->sc[index];
		/*
		 * See the note in sc.h; the first non-tiny size class has an
		 * unusual encoding.
		 */
		lg_base--;
		ndelta = 1;
		size_class(sc, lg_max_lookup, lg_page, lg_ngroup, index,
		    lg_base, lg_delta, ndelta);
		index++;
		lg_base++;
		lg_delta++;
		if (sc->psz) {
			npsizes++;
		}
		if (sc->bin) {
			nbins++;
		}
	}
	while (ndelta < ngroup) {
		sc_t *sc = &sc_data->sc[index];
		size_class(sc, lg_max_lookup, lg_page, lg_ngroup, index,
		    lg_base, lg_delta, ndelta);
		index++;
		ndelta++;
		if (sc->psz) {
			npsizes++;
		}
		if (sc->bin) {
			nbins++;
		}
	}

	/* All remaining groups. */
	lg_base = lg_base + lg_ngroup;
	while (lg_base < ptr_bits - 1) {
		ndelta = 1;
		int ndelta_limit;
		if (lg_base == ptr_bits - 2) {
			ndelta_limit = ngroup - 1;
		} else {
			ndelta_limit = ngroup;
		}
		while (ndelta <= ndelta_limit) {
			sc_t *sc = &sc_data->sc[index];
			size_class(sc, lg_max_lookup, lg_page, lg_ngroup, index,
			    lg_base, lg_delta, ndelta);
			if (sc->lg_delta_lookup != 0) {
				nlbins = index + 1;
				/* Final written value is correct. */
				lookup_maxclass = (ZU(1) << lg_base)
				    + (ZU(ndelta) << lg_delta);
			}
			if (sc->psz) {
				npsizes++;
			}
			if (sc->bin) {
				nbins++;
				/* Final written value is correct. */
				small_maxclass = (ZU(1) << lg_base)
				    + (ZU(ndelta) << lg_delta);
				if (lg_ngroup > 0) {
					lg_large_minclass = lg_base + 1;
				} else {
					lg_large_minclass = lg_base + 2;
				}
			}
			large_maxclass = (ZU(1) << lg_base)
			    + (ZU(ndelta) << lg_delta);
			index++;
			ndelta++;
		}
		lg_base++;
		lg_delta++;
	}
	/* Additional outputs. */
	int nsizes = index;
	unsigned lg_ceil_nsizes = lg_ceil(nsizes);

	/* Fill in the output data. */
	sc_data->ntiny = ntiny;
	sc_data->nlbins = nlbins;
	sc_data->nbins = nbins;
	sc_data->nsizes = nsizes;
	sc_data->lg_ceil_nsizes = lg_ceil_nsizes;
	sc_data->npsizes = npsizes;
	sc_data->lg_tiny_maxclass = lg_tiny_maxclass;
	sc_data->lookup_maxclass = lookup_maxclass;
	sc_data->small_maxclass = small_maxclass;
	sc_data->lg_large_minclass = lg_large_minclass;
	sc_data->large_minclass = (ZU(1) << lg_large_minclass);
	sc_data->large_maxclass = large_maxclass;

	/*
	 * We compute these values in two ways:
	 *   - Incrementally, as above.
	 *   - In macros, in sc.h.
	 * The computation is easier when done incrementally, but putting it in
	 * a constant makes it available to the fast paths without having to
	 * touch the extra global cacheline.  We assert, however, that the two
	 * computations are equivalent.
	 */
	assert(sc_data->npsizes == SC_NPSIZES);
	assert(sc_data->lg_tiny_maxclass == SC_LG_TINY_MAXCLASS);
	assert(sc_data->small_maxclass == SC_SMALL_MAXCLASS);
	assert(sc_data->large_minclass == SC_LARGE_MINCLASS);
	assert(sc_data->lg_large_minclass == SC_LG_LARGE_MINCLASS);
	assert(sc_data->large_maxclass == SC_LARGE_MAXCLASS);

	/*
	 * In the allocation fastpath, we want to assume that we can
	 * unconditionally subtract the requested allocation size from
	 * a ssize_t, and detect passing through 0 correctly.  This
	 * results in optimal generated code.  For this to work, the
	 * maximum allocation size must be less than SSIZE_MAX.
	 */
	assert(SC_LARGE_MAXCLASS < SSIZE_MAX);
}

void
sc_data_init(sc_data_t *sc_data) {
	size_classes(sc_data, LG_SIZEOF_PTR, LG_QUANTUM, SC_LG_TINY_MIN,
	    SC_LG_MAX_LOOKUP, LG_PAGE, SC_LG_NGROUP);

	sc_data->initialized = true;
}

static void
sc_data_update_sc_slab_size(sc_t *sc, size_t reg_size, size_t pgs_guess) {
	size_t min_pgs = reg_size / PAGE;
	if (reg_size % PAGE != 0) {
		min_pgs++;
	}
	/*
	 * BITMAP_MAXBITS is actually determined by putting the smallest
	 * possible size-class on one page, so this can never be 0.
	 */
	size_t max_pgs = BITMAP_MAXBITS * reg_size / PAGE;

	assert(min_pgs <= max_pgs);
	assert(min_pgs > 0);
	assert(max_pgs >= 1);
	if (pgs_guess < min_pgs) {
		sc->pgs = (int)min_pgs;
	} else if (pgs_guess > max_pgs) {
		sc->pgs = (int)max_pgs;
	} else {
		sc->pgs = (int)pgs_guess;
	}
}

void
sc_data_update_slab_size(sc_data_t *data, size_t begin, size_t end, int pgs) {
	assert(data->initialized);
	for (int i = 0; i < data->nsizes; i++) {
		sc_t *sc = &data->sc[i];
		if (!sc->bin) {
			break;
		}
		size_t reg_size = reg_size_compute(sc->lg_base, sc->lg_delta,
		    sc->ndelta);
		if (begin <= reg_size && reg_size <= end) {
			sc_data_update_sc_slab_size(sc, reg_size, pgs);
		}
	}
}

void
sc_boot(sc_data_t *data) {
	sc_data_init(data);
}
