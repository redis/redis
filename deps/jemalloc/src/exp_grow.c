#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

void
exp_grow_init(exp_grow_t *exp_grow) {
	exp_grow->next = sz_psz2ind(HUGEPAGE);
	exp_grow->limit = sz_psz2ind(SC_LARGE_MAXCLASS);
}
