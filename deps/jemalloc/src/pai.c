#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

size_t
pai_alloc_batch_default(tsdn_t *tsdn, pai_t *self, size_t size, size_t nallocs,
    edata_list_active_t *results, bool *deferred_work_generated) {
	for (size_t i = 0; i < nallocs; i++) {
		bool deferred_by_alloc = false;
		edata_t *edata = pai_alloc(tsdn, self, size, PAGE,
		    /* zero */ false, /* guarded */ false,
		    /* frequent_reuse */ false, &deferred_by_alloc);
		*deferred_work_generated |= deferred_by_alloc;
		if (edata == NULL) {
			return i;
		}
		edata_list_active_append(results, edata);
	}
	return nallocs;
}

void
pai_dalloc_batch_default(tsdn_t *tsdn, pai_t *self,
    edata_list_active_t *list, bool *deferred_work_generated) {
	edata_t *edata;
	while ((edata = edata_list_active_first(list)) != NULL) {
		bool deferred_by_dalloc = false;
		edata_list_active_remove(list, edata);
		pai_dalloc(tsdn, self, edata, &deferred_by_dalloc);
		*deferred_work_generated |= deferred_by_dalloc;
	}
}
