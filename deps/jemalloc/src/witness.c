#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/malloc_io.h"

void
witness_init(witness_t *witness, const char *name, witness_rank_t rank,
    witness_comp_t *comp, void *opaque) {
	witness->name = name;
	witness->rank = rank;
	witness->comp = comp;
	witness->opaque = opaque;
}

static void
witness_print_witness(witness_t *w, unsigned n) {
	assert(n > 0);
	if (n == 1) {
		malloc_printf(" %s(%u)", w->name, w->rank);
	} else {
		malloc_printf(" %s(%u)X%u", w->name, w->rank, n);
	}
}

static void
witness_print_witnesses(const witness_list_t *witnesses) {
	witness_t *w, *last = NULL;
	unsigned n = 0;
	ql_foreach(w, witnesses, link) {
		if (last != NULL && w->rank > last->rank) {
			assert(w->name != last->name);
			witness_print_witness(last, n);
			n = 0;
		} else if (last != NULL) {
			assert(w->rank == last->rank);
			assert(w->name == last->name);
		}
		last = w;
		++n;
	}
	if (last != NULL) {
		witness_print_witness(last, n);
	}
}

static void
witness_lock_error_impl(const witness_list_t *witnesses,
    const witness_t *witness) {
	malloc_printf("<jemalloc>: Lock rank order reversal:");
	witness_print_witnesses(witnesses);
	malloc_printf(" %s(%u)\n", witness->name, witness->rank);
	abort();
}
witness_lock_error_t *JET_MUTABLE witness_lock_error = witness_lock_error_impl;

static void
witness_owner_error_impl(const witness_t *witness) {
	malloc_printf("<jemalloc>: Should own %s(%u)\n", witness->name,
	    witness->rank);
	abort();
}
witness_owner_error_t *JET_MUTABLE witness_owner_error =
    witness_owner_error_impl;

static void
witness_not_owner_error_impl(const witness_t *witness) {
	malloc_printf("<jemalloc>: Should not own %s(%u)\n", witness->name,
	    witness->rank);
	abort();
}
witness_not_owner_error_t *JET_MUTABLE witness_not_owner_error =
    witness_not_owner_error_impl;

static void
witness_depth_error_impl(const witness_list_t *witnesses,
    witness_rank_t rank_inclusive, unsigned depth) {
	malloc_printf("<jemalloc>: Should own %u lock%s of rank >= %u:", depth,
	    (depth != 1) ?  "s" : "", rank_inclusive);
	witness_print_witnesses(witnesses);
	malloc_printf("\n");
	abort();
}
witness_depth_error_t *JET_MUTABLE witness_depth_error =
    witness_depth_error_impl;

void
witnesses_cleanup(witness_tsd_t *witness_tsd) {
	witness_assert_lockless(witness_tsd_tsdn(witness_tsd));

	/* Do nothing. */
}

void
witness_prefork(witness_tsd_t *witness_tsd) {
	if (!config_debug) {
		return;
	}
	witness_tsd->forking = true;
}

void
witness_postfork_parent(witness_tsd_t *witness_tsd) {
	if (!config_debug) {
		return;
	}
	witness_tsd->forking = false;
}

void
witness_postfork_child(witness_tsd_t *witness_tsd) {
	if (!config_debug) {
		return;
	}
#ifndef JEMALLOC_MUTEX_INIT_CB
	witness_list_t *witnesses;

	witnesses = &witness_tsd->witnesses;
	ql_new(witnesses);
#endif
	witness_tsd->forking = false;
}
