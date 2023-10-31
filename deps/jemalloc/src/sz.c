#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"
#include "jemalloc/internal/sz.h"

JEMALLOC_ALIGNED(CACHELINE)
size_t sz_pind2sz_tab[SC_NPSIZES+1];
size_t sz_large_pad;

size_t
sz_psz_quantize_floor(size_t size) {
	size_t ret;
	pszind_t pind;

	assert(size > 0);
	assert((size & PAGE_MASK) == 0);

	pind = sz_psz2ind(size - sz_large_pad + 1);
	if (pind == 0) {
		/*
		 * Avoid underflow.  This short-circuit would also do the right
		 * thing for all sizes in the range for which there are
		 * PAGE-spaced size classes, but it's simplest to just handle
		 * the one case that would cause erroneous results.
		 */
		return size;
	}
	ret = sz_pind2sz(pind - 1) + sz_large_pad;
	assert(ret <= size);
	return ret;
}

size_t
sz_psz_quantize_ceil(size_t size) {
	size_t ret;

	assert(size > 0);
	assert(size - sz_large_pad <= SC_LARGE_MAXCLASS);
	assert((size & PAGE_MASK) == 0);

	ret = sz_psz_quantize_floor(size);
	if (ret < size) {
		/*
		 * Skip a quantization that may have an adequately large extent,
		 * because under-sized extents may be mixed in.  This only
		 * happens when an unusual size is requested, i.e. for aligned
		 * allocation, and is just one of several places where linear
		 * search would potentially find sufficiently aligned available
		 * memory somewhere lower.
		 */
		ret = sz_pind2sz(sz_psz2ind(ret - sz_large_pad + 1)) +
		    sz_large_pad;
	}
	return ret;
}

static void
sz_boot_pind2sz_tab(const sc_data_t *sc_data) {
	int pind = 0;
	for (unsigned i = 0; i < SC_NSIZES; i++) {
		const sc_t *sc = &sc_data->sc[i];
		if (sc->psz) {
			sz_pind2sz_tab[pind] = (ZU(1) << sc->lg_base)
			    + (ZU(sc->ndelta) << sc->lg_delta);
			pind++;
		}
	}
	for (int i = pind; i <= (int)SC_NPSIZES; i++) {
		sz_pind2sz_tab[pind] = sc_data->large_maxclass + PAGE;
	}
}

JEMALLOC_ALIGNED(CACHELINE)
size_t sz_index2size_tab[SC_NSIZES];

static void
sz_boot_index2size_tab(const sc_data_t *sc_data) {
	for (unsigned i = 0; i < SC_NSIZES; i++) {
		const sc_t *sc = &sc_data->sc[i];
		sz_index2size_tab[i] = (ZU(1) << sc->lg_base)
		    + (ZU(sc->ndelta) << (sc->lg_delta));
	}
}

/*
 * To keep this table small, we divide sizes by the tiny min size, which gives
 * the smallest interval for which the result can change.
 */
JEMALLOC_ALIGNED(CACHELINE)
uint8_t sz_size2index_tab[(SC_LOOKUP_MAXCLASS >> SC_LG_TINY_MIN) + 1];

static void
sz_boot_size2index_tab(const sc_data_t *sc_data) {
	size_t dst_max = (SC_LOOKUP_MAXCLASS >> SC_LG_TINY_MIN) + 1;
	size_t dst_ind = 0;
	for (unsigned sc_ind = 0; sc_ind < SC_NSIZES && dst_ind < dst_max;
	    sc_ind++) {
		const sc_t *sc = &sc_data->sc[sc_ind];
		size_t sz = (ZU(1) << sc->lg_base)
		    + (ZU(sc->ndelta) << sc->lg_delta);
		size_t max_ind = ((sz + (ZU(1) << SC_LG_TINY_MIN) - 1)
				   >> SC_LG_TINY_MIN);
		for (; dst_ind <= max_ind && dst_ind < dst_max; dst_ind++) {
			sz_size2index_tab[dst_ind] = sc_ind;
		}
	}
}

void
sz_boot(const sc_data_t *sc_data, bool cache_oblivious) {
	sz_large_pad = cache_oblivious ? PAGE : 0;
	sz_boot_pind2sz_tab(sc_data);
	sz_boot_index2size_tab(sc_data);
	sz_boot_size2index_tab(sc_data);
}
