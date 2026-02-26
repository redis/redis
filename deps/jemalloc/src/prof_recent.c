#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/buf_writer.h"
#include "jemalloc/internal/emitter.h"
#include "jemalloc/internal/prof_data.h"
#include "jemalloc/internal/prof_recent.h"

ssize_t opt_prof_recent_alloc_max = PROF_RECENT_ALLOC_MAX_DEFAULT;
malloc_mutex_t prof_recent_alloc_mtx; /* Protects the fields below */
static atomic_zd_t prof_recent_alloc_max;
static ssize_t prof_recent_alloc_count = 0;
prof_recent_list_t prof_recent_alloc_list;

malloc_mutex_t prof_recent_dump_mtx; /* Protects dumping. */

static void
prof_recent_alloc_max_init() {
	atomic_store_zd(&prof_recent_alloc_max, opt_prof_recent_alloc_max,
	    ATOMIC_RELAXED);
}

static inline ssize_t
prof_recent_alloc_max_get_no_lock() {
	return atomic_load_zd(&prof_recent_alloc_max, ATOMIC_RELAXED);
}

static inline ssize_t
prof_recent_alloc_max_get(tsd_t *tsd) {
	malloc_mutex_assert_owner(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	return prof_recent_alloc_max_get_no_lock();
}

static inline ssize_t
prof_recent_alloc_max_update(tsd_t *tsd, ssize_t max) {
	malloc_mutex_assert_owner(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	ssize_t old_max = prof_recent_alloc_max_get(tsd);
	atomic_store_zd(&prof_recent_alloc_max, max, ATOMIC_RELAXED);
	return old_max;
}

static prof_recent_t *
prof_recent_allocate_node(tsdn_t *tsdn) {
	return (prof_recent_t *)iallocztm(tsdn, sizeof(prof_recent_t),
	    sz_size2index(sizeof(prof_recent_t)), false, NULL, true,
	    arena_get(tsdn, 0, false), true);
}

static void
prof_recent_free_node(tsdn_t *tsdn, prof_recent_t *node) {
	assert(node != NULL);
	assert(isalloc(tsdn, node) == sz_s2u(sizeof(prof_recent_t)));
	idalloctm(tsdn, node, NULL, NULL, true, true);
}

static inline void
increment_recent_count(tsd_t *tsd, prof_tctx_t *tctx) {
	malloc_mutex_assert_owner(tsd_tsdn(tsd), tctx->tdata->lock);
	++tctx->recent_count;
	assert(tctx->recent_count > 0);
}

bool
prof_recent_alloc_prepare(tsd_t *tsd, prof_tctx_t *tctx) {
	cassert(config_prof);
	assert(opt_prof && prof_booted);
	malloc_mutex_assert_owner(tsd_tsdn(tsd), tctx->tdata->lock);
	malloc_mutex_assert_not_owner(tsd_tsdn(tsd), &prof_recent_alloc_mtx);

	/*
	 * Check whether last-N mode is turned on without trying to acquire the
	 * lock, so as to optimize for the following two scenarios:
	 * (1) Last-N mode is switched off;
	 * (2) Dumping, during which last-N mode is temporarily turned off so
	 *     as not to block sampled allocations.
	 */
	if (prof_recent_alloc_max_get_no_lock() == 0) {
		return false;
	}

	/*
	 * Increment recent_count to hold the tctx so that it won't be gone
	 * even after tctx->tdata->lock is released.  This acts as a
	 * "placeholder"; the real recording of the allocation requires a lock
	 * on prof_recent_alloc_mtx and is done in prof_recent_alloc (when
	 * tctx->tdata->lock has been released).
	 */
	increment_recent_count(tsd, tctx);
	return true;
}

static void
decrement_recent_count(tsd_t *tsd, prof_tctx_t *tctx) {
	malloc_mutex_assert_not_owner(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	assert(tctx != NULL);
	malloc_mutex_lock(tsd_tsdn(tsd), tctx->tdata->lock);
	assert(tctx->recent_count > 0);
	--tctx->recent_count;
	prof_tctx_try_destroy(tsd, tctx);
}

static inline edata_t *
prof_recent_alloc_edata_get_no_lock(const prof_recent_t *n) {
	return (edata_t *)atomic_load_p(&n->alloc_edata, ATOMIC_ACQUIRE);
}

edata_t *
prof_recent_alloc_edata_get_no_lock_test(const prof_recent_t *n) {
	cassert(config_prof);
	return prof_recent_alloc_edata_get_no_lock(n);
}

static inline edata_t *
prof_recent_alloc_edata_get(tsd_t *tsd, const prof_recent_t *n) {
	malloc_mutex_assert_owner(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	return prof_recent_alloc_edata_get_no_lock(n);
}

static void
prof_recent_alloc_edata_set(tsd_t *tsd, prof_recent_t *n, edata_t *edata) {
	malloc_mutex_assert_owner(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	atomic_store_p(&n->alloc_edata, edata, ATOMIC_RELEASE);
}

void
edata_prof_recent_alloc_init(edata_t *edata) {
	cassert(config_prof);
	edata_prof_recent_alloc_set_dont_call_directly(edata, NULL);
}

static inline prof_recent_t *
edata_prof_recent_alloc_get_no_lock(const edata_t *edata) {
	cassert(config_prof);
	return edata_prof_recent_alloc_get_dont_call_directly(edata);
}

prof_recent_t *
edata_prof_recent_alloc_get_no_lock_test(const edata_t *edata) {
	cassert(config_prof);
	return edata_prof_recent_alloc_get_no_lock(edata);
}

static inline prof_recent_t *
edata_prof_recent_alloc_get(tsd_t *tsd, const edata_t *edata) {
	malloc_mutex_assert_owner(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	prof_recent_t *recent_alloc =
	    edata_prof_recent_alloc_get_no_lock(edata);
	assert(recent_alloc == NULL ||
	    prof_recent_alloc_edata_get(tsd, recent_alloc) == edata);
	return recent_alloc;
}

static prof_recent_t *
edata_prof_recent_alloc_update_internal(tsd_t *tsd, edata_t *edata,
    prof_recent_t *recent_alloc) {
	malloc_mutex_assert_owner(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	prof_recent_t *old_recent_alloc =
	    edata_prof_recent_alloc_get(tsd, edata);
	edata_prof_recent_alloc_set_dont_call_directly(edata, recent_alloc);
	return old_recent_alloc;
}

static void
edata_prof_recent_alloc_set(tsd_t *tsd, edata_t *edata,
    prof_recent_t *recent_alloc) {
	malloc_mutex_assert_owner(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	assert(recent_alloc != NULL);
	prof_recent_t *old_recent_alloc =
	    edata_prof_recent_alloc_update_internal(tsd, edata, recent_alloc);
	assert(old_recent_alloc == NULL);
	prof_recent_alloc_edata_set(tsd, recent_alloc, edata);
}

static void
edata_prof_recent_alloc_reset(tsd_t *tsd, edata_t *edata,
    prof_recent_t *recent_alloc) {
	malloc_mutex_assert_owner(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	assert(recent_alloc != NULL);
	prof_recent_t *old_recent_alloc =
	    edata_prof_recent_alloc_update_internal(tsd, edata, NULL);
	assert(old_recent_alloc == recent_alloc);
	assert(edata == prof_recent_alloc_edata_get(tsd, recent_alloc));
	prof_recent_alloc_edata_set(tsd, recent_alloc, NULL);
}

/*
 * This function should be called right before an allocation is released, so
 * that the associated recent allocation record can contain the following
 * information:
 * (1) The allocation is released;
 * (2) The time of the deallocation; and
 * (3) The prof_tctx associated with the deallocation.
 */
void
prof_recent_alloc_reset(tsd_t *tsd, edata_t *edata) {
	cassert(config_prof);
	/*
	 * Check whether the recent allocation record still exists without
	 * trying to acquire the lock.
	 */
	if (edata_prof_recent_alloc_get_no_lock(edata) == NULL) {
		return;
	}

	prof_tctx_t *dalloc_tctx = prof_tctx_create(tsd);
	/*
	 * In case dalloc_tctx is NULL, e.g. due to OOM, we will not record the
	 * deallocation time / tctx, which is handled later, after we check
	 * again when holding the lock.
	 */

	if (dalloc_tctx != NULL) {
		malloc_mutex_lock(tsd_tsdn(tsd), dalloc_tctx->tdata->lock);
		increment_recent_count(tsd, dalloc_tctx);
		dalloc_tctx->prepared = false;
		malloc_mutex_unlock(tsd_tsdn(tsd), dalloc_tctx->tdata->lock);
	}

	malloc_mutex_lock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	/* Check again after acquiring the lock.  */
	prof_recent_t *recent = edata_prof_recent_alloc_get(tsd, edata);
	if (recent != NULL) {
		assert(nstime_equals_zero(&recent->dalloc_time));
		assert(recent->dalloc_tctx == NULL);
		if (dalloc_tctx != NULL) {
			nstime_prof_update(&recent->dalloc_time);
			recent->dalloc_tctx = dalloc_tctx;
			dalloc_tctx = NULL;
		}
		edata_prof_recent_alloc_reset(tsd, edata, recent);
	}
	malloc_mutex_unlock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);

	if (dalloc_tctx != NULL) {
		/* We lost the rase - the allocation record was just gone. */
		decrement_recent_count(tsd, dalloc_tctx);
	}
}

static void
prof_recent_alloc_evict_edata(tsd_t *tsd, prof_recent_t *recent_alloc) {
	malloc_mutex_assert_owner(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	edata_t *edata = prof_recent_alloc_edata_get(tsd, recent_alloc);
	if (edata != NULL) {
		edata_prof_recent_alloc_reset(tsd, edata, recent_alloc);
	}
}

static bool
prof_recent_alloc_is_empty(tsd_t *tsd) {
	malloc_mutex_assert_owner(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	if (ql_empty(&prof_recent_alloc_list)) {
		assert(prof_recent_alloc_count == 0);
		return true;
	} else {
		assert(prof_recent_alloc_count > 0);
		return false;
	}
}

static void
prof_recent_alloc_assert_count(tsd_t *tsd) {
	malloc_mutex_assert_owner(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	if (!config_debug) {
		return;
	}
	ssize_t count = 0;
	prof_recent_t *n;
	ql_foreach(n, &prof_recent_alloc_list, link) {
		++count;
	}
	assert(count == prof_recent_alloc_count);
	assert(prof_recent_alloc_max_get(tsd) == -1 ||
	    count <= prof_recent_alloc_max_get(tsd));
}

void
prof_recent_alloc(tsd_t *tsd, edata_t *edata, size_t size, size_t usize) {
	cassert(config_prof);
	assert(edata != NULL);
	prof_tctx_t *tctx = edata_prof_tctx_get(edata);

	malloc_mutex_assert_not_owner(tsd_tsdn(tsd), tctx->tdata->lock);
	malloc_mutex_lock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	prof_recent_alloc_assert_count(tsd);

	/*
	 * Reserve a new prof_recent_t node if needed.  If needed, we release
	 * the prof_recent_alloc_mtx lock and allocate.  Then, rather than
	 * immediately checking for OOM, we regain the lock and try to make use
	 * of the reserve node if needed.  There are six scenarios:
	 *
	 *          \ now | no need | need but OOMed | need and allocated
	 *     later \    |         |                |
	 *    ------------------------------------------------------------
	 *     no need    |   (1)   |      (2)       |         (3)
	 *    ------------------------------------------------------------
	 *     need       |   (4)   |      (5)       |         (6)
	 *
	 * First, "(4)" never happens, because we don't release the lock in the
	 * middle if there's no need for a new node; in such cases "(1)" always
	 * takes place, which is trivial.
	 *
	 * Out of the remaining four scenarios, "(6)" is the common case and is
	 * trivial.  "(5)" is also trivial, in which case we'll rollback the
	 * effect of prof_recent_alloc_prepare() as expected.
	 *
	 * "(2)" / "(3)" occurs when the need for a new node is gone after we
	 * regain the lock.  If the new node is successfully allocated, i.e. in
	 * the case of "(3)", we'll release it in the end; otherwise, i.e. in
	 * the case of "(2)", we do nothing - we're lucky that the OOM ends up
	 * doing no harm at all.
	 *
	 * Therefore, the only performance cost of the "release lock" ->
	 * "allocate" -> "regain lock" design is the "(3)" case, but it happens
	 * very rarely, so the cost is relatively small compared to the gain of
	 * not having to have the lock order of prof_recent_alloc_mtx above all
	 * the allocation locks.
	 */
	prof_recent_t *reserve = NULL;
	if (prof_recent_alloc_max_get(tsd) == -1 ||
	    prof_recent_alloc_count < prof_recent_alloc_max_get(tsd)) {
		assert(prof_recent_alloc_max_get(tsd) != 0);
		malloc_mutex_unlock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
		reserve = prof_recent_allocate_node(tsd_tsdn(tsd));
		malloc_mutex_lock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
		prof_recent_alloc_assert_count(tsd);
	}

	if (prof_recent_alloc_max_get(tsd) == 0) {
		assert(prof_recent_alloc_is_empty(tsd));
		goto label_rollback;
	}

	prof_tctx_t *old_alloc_tctx, *old_dalloc_tctx;
	if (prof_recent_alloc_count == prof_recent_alloc_max_get(tsd)) {
		/* If upper limit is reached, rotate the head. */
		assert(prof_recent_alloc_max_get(tsd) != -1);
		assert(!prof_recent_alloc_is_empty(tsd));
		prof_recent_t *head = ql_first(&prof_recent_alloc_list);
		old_alloc_tctx = head->alloc_tctx;
		assert(old_alloc_tctx != NULL);
		old_dalloc_tctx = head->dalloc_tctx;
		prof_recent_alloc_evict_edata(tsd, head);
		ql_rotate(&prof_recent_alloc_list, link);
	} else {
		/* Otherwise make use of the new node. */
		assert(prof_recent_alloc_max_get(tsd) == -1 ||
		    prof_recent_alloc_count < prof_recent_alloc_max_get(tsd));
		if (reserve == NULL) {
			goto label_rollback;
		}
		ql_elm_new(reserve, link);
		ql_tail_insert(&prof_recent_alloc_list, reserve, link);
		reserve = NULL;
		old_alloc_tctx = NULL;
		old_dalloc_tctx = NULL;
		++prof_recent_alloc_count;
	}

	/* Fill content into the tail node. */
	prof_recent_t *tail = ql_last(&prof_recent_alloc_list, link);
	assert(tail != NULL);
	tail->size = size;
	tail->usize = usize;
	nstime_copy(&tail->alloc_time, edata_prof_alloc_time_get(edata));
	tail->alloc_tctx = tctx;
	nstime_init_zero(&tail->dalloc_time);
	tail->dalloc_tctx = NULL;
	edata_prof_recent_alloc_set(tsd, edata, tail);

	assert(!prof_recent_alloc_is_empty(tsd));
	prof_recent_alloc_assert_count(tsd);
	malloc_mutex_unlock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);

	if (reserve != NULL) {
		prof_recent_free_node(tsd_tsdn(tsd), reserve);
	}

	/*
	 * Asynchronously handle the tctx of the old node, so that there's no
	 * simultaneous holdings of prof_recent_alloc_mtx and tdata->lock.
	 * In the worst case this may delay the tctx release but it's better
	 * than holding prof_recent_alloc_mtx for longer.
	 */
	if (old_alloc_tctx != NULL) {
		decrement_recent_count(tsd, old_alloc_tctx);
	}
	if (old_dalloc_tctx != NULL) {
		decrement_recent_count(tsd, old_dalloc_tctx);
	}
	return;

label_rollback:
	assert(edata_prof_recent_alloc_get(tsd, edata) == NULL);
	prof_recent_alloc_assert_count(tsd);
	malloc_mutex_unlock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	if (reserve != NULL) {
		prof_recent_free_node(tsd_tsdn(tsd), reserve);
	}
	decrement_recent_count(tsd, tctx);
}

ssize_t
prof_recent_alloc_max_ctl_read() {
	cassert(config_prof);
	/* Don't bother to acquire the lock. */
	return prof_recent_alloc_max_get_no_lock();
}

static void
prof_recent_alloc_restore_locked(tsd_t *tsd, prof_recent_list_t *to_delete) {
	malloc_mutex_assert_owner(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	ssize_t max = prof_recent_alloc_max_get(tsd);
	if (max == -1 || prof_recent_alloc_count <= max) {
		/* Easy case - no need to alter the list. */
		ql_new(to_delete);
		prof_recent_alloc_assert_count(tsd);
		return;
	}

	prof_recent_t *node;
	ql_foreach(node, &prof_recent_alloc_list, link) {
		if (prof_recent_alloc_count == max) {
			break;
		}
		prof_recent_alloc_evict_edata(tsd, node);
		--prof_recent_alloc_count;
	}
	assert(prof_recent_alloc_count == max);

	ql_move(to_delete, &prof_recent_alloc_list);
	if (max == 0) {
		assert(node == NULL);
	} else {
		assert(node != NULL);
		ql_split(to_delete, node, &prof_recent_alloc_list, link);
	}
	assert(!ql_empty(to_delete));
	prof_recent_alloc_assert_count(tsd);
}

static void
prof_recent_alloc_async_cleanup(tsd_t *tsd, prof_recent_list_t *to_delete) {
	malloc_mutex_assert_not_owner(tsd_tsdn(tsd), &prof_recent_dump_mtx);
	malloc_mutex_assert_not_owner(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	while (!ql_empty(to_delete)) {
		prof_recent_t *node = ql_first(to_delete);
		ql_remove(to_delete, node, link);
		decrement_recent_count(tsd, node->alloc_tctx);
		if (node->dalloc_tctx != NULL) {
			decrement_recent_count(tsd, node->dalloc_tctx);
		}
		prof_recent_free_node(tsd_tsdn(tsd), node);
	}
}

ssize_t
prof_recent_alloc_max_ctl_write(tsd_t *tsd, ssize_t max) {
	cassert(config_prof);
	assert(max >= -1);
	malloc_mutex_lock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	prof_recent_alloc_assert_count(tsd);
	const ssize_t old_max = prof_recent_alloc_max_update(tsd, max);
	prof_recent_list_t to_delete;
	prof_recent_alloc_restore_locked(tsd, &to_delete);
	malloc_mutex_unlock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	prof_recent_alloc_async_cleanup(tsd, &to_delete);
	return old_max;
}

static void
prof_recent_alloc_dump_bt(emitter_t *emitter, prof_tctx_t *tctx) {
	char bt_buf[2 * sizeof(intptr_t) + 3];
	char *s = bt_buf;
	assert(tctx != NULL);
	prof_bt_t *bt = &tctx->gctx->bt;
	for (size_t i = 0; i < bt->len; ++i) {
		malloc_snprintf(bt_buf, sizeof(bt_buf), "%p", bt->vec[i]);
		emitter_json_value(emitter, emitter_type_string, &s);
	}
}

static void
prof_recent_alloc_dump_node(emitter_t *emitter, prof_recent_t *node) {
	emitter_json_object_begin(emitter);

	emitter_json_kv(emitter, "size", emitter_type_size, &node->size);
	emitter_json_kv(emitter, "usize", emitter_type_size, &node->usize);
	bool released = prof_recent_alloc_edata_get_no_lock(node) == NULL;
	emitter_json_kv(emitter, "released", emitter_type_bool, &released);

	emitter_json_kv(emitter, "alloc_thread_uid", emitter_type_uint64,
	    &node->alloc_tctx->thr_uid);
	prof_tdata_t *alloc_tdata = node->alloc_tctx->tdata;
	assert(alloc_tdata != NULL);
	if (alloc_tdata->thread_name != NULL) {
		emitter_json_kv(emitter, "alloc_thread_name",
		    emitter_type_string, &alloc_tdata->thread_name);
	}
	uint64_t alloc_time_ns = nstime_ns(&node->alloc_time);
	emitter_json_kv(emitter, "alloc_time", emitter_type_uint64,
	    &alloc_time_ns);
	emitter_json_array_kv_begin(emitter, "alloc_trace");
	prof_recent_alloc_dump_bt(emitter, node->alloc_tctx);
	emitter_json_array_end(emitter);

	if (released && node->dalloc_tctx != NULL) {
		emitter_json_kv(emitter, "dalloc_thread_uid",
		    emitter_type_uint64, &node->dalloc_tctx->thr_uid);
		prof_tdata_t *dalloc_tdata = node->dalloc_tctx->tdata;
		assert(dalloc_tdata != NULL);
		if (dalloc_tdata->thread_name != NULL) {
			emitter_json_kv(emitter, "dalloc_thread_name",
			    emitter_type_string, &dalloc_tdata->thread_name);
		}
		assert(!nstime_equals_zero(&node->dalloc_time));
		uint64_t dalloc_time_ns = nstime_ns(&node->dalloc_time);
		emitter_json_kv(emitter, "dalloc_time", emitter_type_uint64,
		    &dalloc_time_ns);
		emitter_json_array_kv_begin(emitter, "dalloc_trace");
		prof_recent_alloc_dump_bt(emitter, node->dalloc_tctx);
		emitter_json_array_end(emitter);
	}

	emitter_json_object_end(emitter);
}

#define PROF_RECENT_PRINT_BUFSIZE 65536
JEMALLOC_COLD
void
prof_recent_alloc_dump(tsd_t *tsd, write_cb_t *write_cb, void *cbopaque) {
	cassert(config_prof);
	malloc_mutex_lock(tsd_tsdn(tsd), &prof_recent_dump_mtx);
	buf_writer_t buf_writer;
	buf_writer_init(tsd_tsdn(tsd), &buf_writer, write_cb, cbopaque, NULL,
	    PROF_RECENT_PRINT_BUFSIZE);
	emitter_t emitter;
	emitter_init(&emitter, emitter_output_json_compact, buf_writer_cb,
	    &buf_writer);
	prof_recent_list_t temp_list;

	malloc_mutex_lock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	prof_recent_alloc_assert_count(tsd);
	ssize_t dump_max = prof_recent_alloc_max_get(tsd);
	ql_move(&temp_list, &prof_recent_alloc_list);
	ssize_t dump_count = prof_recent_alloc_count;
	prof_recent_alloc_count = 0;
	prof_recent_alloc_assert_count(tsd);
	malloc_mutex_unlock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);

	emitter_begin(&emitter);
	uint64_t sample_interval = (uint64_t)1U << lg_prof_sample;
	emitter_json_kv(&emitter, "sample_interval", emitter_type_uint64,
	    &sample_interval);
	emitter_json_kv(&emitter, "recent_alloc_max", emitter_type_ssize,
	    &dump_max);
	emitter_json_array_kv_begin(&emitter, "recent_alloc");
	prof_recent_t *node;
	ql_foreach(node, &temp_list, link) {
		prof_recent_alloc_dump_node(&emitter, node);
	}
	emitter_json_array_end(&emitter);
	emitter_end(&emitter);

	malloc_mutex_lock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	prof_recent_alloc_assert_count(tsd);
	ql_concat(&temp_list, &prof_recent_alloc_list, link);
	ql_move(&prof_recent_alloc_list, &temp_list);
	prof_recent_alloc_count += dump_count;
	prof_recent_alloc_restore_locked(tsd, &temp_list);
	malloc_mutex_unlock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);

	buf_writer_terminate(tsd_tsdn(tsd), &buf_writer);
	malloc_mutex_unlock(tsd_tsdn(tsd), &prof_recent_dump_mtx);

	prof_recent_alloc_async_cleanup(tsd, &temp_list);
}
#undef PROF_RECENT_PRINT_BUFSIZE

bool
prof_recent_init() {
	cassert(config_prof);
	prof_recent_alloc_max_init();

	if (malloc_mutex_init(&prof_recent_alloc_mtx, "prof_recent_alloc",
	    WITNESS_RANK_PROF_RECENT_ALLOC, malloc_mutex_rank_exclusive)) {
		return true;
	}

	if (malloc_mutex_init(&prof_recent_dump_mtx, "prof_recent_dump",
	    WITNESS_RANK_PROF_RECENT_DUMP, malloc_mutex_rank_exclusive)) {
		return true;
	}

	ql_new(&prof_recent_alloc_list);

	return false;
}
