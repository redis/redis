#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/buf_writer.h"
#include "jemalloc/internal/ckh.h"
#include "jemalloc/internal/emitter.h"
#include "jemalloc/internal/hash.h"
#include "jemalloc/internal/malloc_io.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/prof_data.h"
#include "jemalloc/internal/prof_log.h"
#include "jemalloc/internal/prof_sys.h"

bool opt_prof_log = false;
typedef enum prof_logging_state_e prof_logging_state_t;
enum prof_logging_state_e {
	prof_logging_state_stopped,
	prof_logging_state_started,
	prof_logging_state_dumping
};

/*
 * - stopped: log_start never called, or previous log_stop has completed.
 * - started: log_start called, log_stop not called yet. Allocations are logged.
 * - dumping: log_stop called but not finished; samples are not logged anymore.
 */
prof_logging_state_t prof_logging_state = prof_logging_state_stopped;

/* Used in unit tests. */
static bool prof_log_dummy = false;

/* Incremented for every log file that is output. */
static uint64_t log_seq = 0;
static char log_filename[
    /* Minimize memory bloat for non-prof builds. */
#ifdef JEMALLOC_PROF
    PATH_MAX +
#endif
    1];

/* Timestamp for most recent call to log_start(). */
static nstime_t log_start_timestamp;

/* Increment these when adding to the log_bt and log_thr linked lists. */
static size_t log_bt_index = 0;
static size_t log_thr_index = 0;

/* Linked list node definitions. These are only used in this file. */
typedef struct prof_bt_node_s prof_bt_node_t;

struct prof_bt_node_s {
	prof_bt_node_t *next;
	size_t index;
	prof_bt_t bt;
	/* Variable size backtrace vector pointed to by bt. */
	void *vec[1];
};

typedef struct prof_thr_node_s prof_thr_node_t;

struct prof_thr_node_s {
	prof_thr_node_t *next;
	size_t index;
	uint64_t thr_uid;
	/* Variable size based on thr_name_sz. */
	char name[1];
};

typedef struct prof_alloc_node_s prof_alloc_node_t;

/* This is output when logging sampled allocations. */
struct prof_alloc_node_s {
	prof_alloc_node_t *next;
	/* Indices into an array of thread data. */
	size_t alloc_thr_ind;
	size_t free_thr_ind;

	/* Indices into an array of backtraces. */
	size_t alloc_bt_ind;
	size_t free_bt_ind;

	uint64_t alloc_time_ns;
	uint64_t free_time_ns;

	size_t usize;
};

/*
 * Created on the first call to prof_try_log and deleted on prof_log_stop.
 * These are the backtraces and threads that have already been logged by an
 * allocation.
 */
static bool log_tables_initialized = false;
static ckh_t log_bt_node_set;
static ckh_t log_thr_node_set;

/* Store linked lists for logged data. */
static prof_bt_node_t *log_bt_first = NULL;
static prof_bt_node_t *log_bt_last = NULL;
static prof_thr_node_t *log_thr_first = NULL;
static prof_thr_node_t *log_thr_last = NULL;
static prof_alloc_node_t *log_alloc_first = NULL;
static prof_alloc_node_t *log_alloc_last = NULL;

/* Protects the prof_logging_state and any log_{...} variable. */
malloc_mutex_t log_mtx;

/******************************************************************************/
/*
 * Function prototypes for static functions that are referenced prior to
 * definition.
 */

/* Hashtable functions for log_bt_node_set and log_thr_node_set. */
static void prof_thr_node_hash(const void *key, size_t r_hash[2]);
static bool prof_thr_node_keycomp(const void *k1, const void *k2);
static void prof_bt_node_hash(const void *key, size_t r_hash[2]);
static bool prof_bt_node_keycomp(const void *k1, const void *k2);

/******************************************************************************/

static size_t
prof_log_bt_index(tsd_t *tsd, prof_bt_t *bt) {
	assert(prof_logging_state == prof_logging_state_started);
	malloc_mutex_assert_owner(tsd_tsdn(tsd), &log_mtx);

	prof_bt_node_t dummy_node;
	dummy_node.bt = *bt;
	prof_bt_node_t *node;

	/* See if this backtrace is already cached in the table. */
	if (ckh_search(&log_bt_node_set, (void *)(&dummy_node),
	    (void **)(&node), NULL)) {
		size_t sz = offsetof(prof_bt_node_t, vec) +
			        (bt->len * sizeof(void *));
		prof_bt_node_t *new_node = (prof_bt_node_t *)
		    iallocztm(tsd_tsdn(tsd), sz, sz_size2index(sz), false, NULL,
		    true, arena_get(TSDN_NULL, 0, true), true);
		if (log_bt_first == NULL) {
			log_bt_first = new_node;
			log_bt_last = new_node;
		} else {
			log_bt_last->next = new_node;
			log_bt_last = new_node;
		}

		new_node->next = NULL;
		new_node->index = log_bt_index;
		/*
		 * Copy the backtrace: bt is inside a tdata or gctx, which
		 * might die before prof_log_stop is called.
		 */
		new_node->bt.len = bt->len;
		memcpy(new_node->vec, bt->vec, bt->len * sizeof(void *));
		new_node->bt.vec = new_node->vec;

		log_bt_index++;
		ckh_insert(tsd, &log_bt_node_set, (void *)new_node, NULL);
		return new_node->index;
	} else {
		return node->index;
	}
}

static size_t
prof_log_thr_index(tsd_t *tsd, uint64_t thr_uid, const char *name) {
	assert(prof_logging_state == prof_logging_state_started);
	malloc_mutex_assert_owner(tsd_tsdn(tsd), &log_mtx);

	prof_thr_node_t dummy_node;
	dummy_node.thr_uid = thr_uid;
	prof_thr_node_t *node;

	/* See if this thread is already cached in the table. */
	if (ckh_search(&log_thr_node_set, (void *)(&dummy_node),
	    (void **)(&node), NULL)) {
		size_t sz = offsetof(prof_thr_node_t, name) + strlen(name) + 1;
		prof_thr_node_t *new_node = (prof_thr_node_t *)
		    iallocztm(tsd_tsdn(tsd), sz, sz_size2index(sz), false, NULL,
		    true, arena_get(TSDN_NULL, 0, true), true);
		if (log_thr_first == NULL) {
			log_thr_first = new_node;
			log_thr_last = new_node;
		} else {
			log_thr_last->next = new_node;
			log_thr_last = new_node;
		}

		new_node->next = NULL;
		new_node->index = log_thr_index;
		new_node->thr_uid = thr_uid;
		strcpy(new_node->name, name);

		log_thr_index++;
		ckh_insert(tsd, &log_thr_node_set, (void *)new_node, NULL);
		return new_node->index;
	} else {
		return node->index;
	}
}

JEMALLOC_COLD
void
prof_try_log(tsd_t *tsd, size_t usize, prof_info_t *prof_info) {
	cassert(config_prof);
	prof_tctx_t *tctx = prof_info->alloc_tctx;
	malloc_mutex_assert_owner(tsd_tsdn(tsd), tctx->tdata->lock);

	prof_tdata_t *cons_tdata = prof_tdata_get(tsd, false);
	if (cons_tdata == NULL) {
		/*
		 * We decide not to log these allocations. cons_tdata will be
		 * NULL only when the current thread is in a weird state (e.g.
		 * it's being destroyed).
		 */
		return;
	}

	malloc_mutex_lock(tsd_tsdn(tsd), &log_mtx);

	if (prof_logging_state != prof_logging_state_started) {
		goto label_done;
	}

	if (!log_tables_initialized) {
		bool err1 = ckh_new(tsd, &log_bt_node_set, PROF_CKH_MINITEMS,
				prof_bt_node_hash, prof_bt_node_keycomp);
		bool err2 = ckh_new(tsd, &log_thr_node_set, PROF_CKH_MINITEMS,
				prof_thr_node_hash, prof_thr_node_keycomp);
		if (err1 || err2) {
			goto label_done;
		}
		log_tables_initialized = true;
	}

	nstime_t alloc_time = prof_info->alloc_time;
	nstime_t free_time;
	nstime_prof_init_update(&free_time);

	size_t sz = sizeof(prof_alloc_node_t);
	prof_alloc_node_t *new_node = (prof_alloc_node_t *)
	    iallocztm(tsd_tsdn(tsd), sz, sz_size2index(sz), false, NULL, true,
	    arena_get(TSDN_NULL, 0, true), true);

	const char *prod_thr_name = (tctx->tdata->thread_name == NULL)?
				        "" : tctx->tdata->thread_name;
	const char *cons_thr_name = prof_thread_name_get(tsd);

	prof_bt_t bt;
	/* Initialize the backtrace, using the buffer in tdata to store it. */
	bt_init(&bt, cons_tdata->vec);
	prof_backtrace(tsd, &bt);
	prof_bt_t *cons_bt = &bt;

	/* We haven't destroyed tctx yet, so gctx should be good to read. */
	prof_bt_t *prod_bt = &tctx->gctx->bt;

	new_node->next = NULL;
	new_node->alloc_thr_ind = prof_log_thr_index(tsd, tctx->tdata->thr_uid,
				      prod_thr_name);
	new_node->free_thr_ind = prof_log_thr_index(tsd, cons_tdata->thr_uid,
				     cons_thr_name);
	new_node->alloc_bt_ind = prof_log_bt_index(tsd, prod_bt);
	new_node->free_bt_ind = prof_log_bt_index(tsd, cons_bt);
	new_node->alloc_time_ns = nstime_ns(&alloc_time);
	new_node->free_time_ns = nstime_ns(&free_time);
	new_node->usize = usize;

	if (log_alloc_first == NULL) {
		log_alloc_first = new_node;
		log_alloc_last = new_node;
	} else {
		log_alloc_last->next = new_node;
		log_alloc_last = new_node;
	}

label_done:
	malloc_mutex_unlock(tsd_tsdn(tsd), &log_mtx);
}

static void
prof_bt_node_hash(const void *key, size_t r_hash[2]) {
	const prof_bt_node_t *bt_node = (prof_bt_node_t *)key;
	prof_bt_hash((void *)(&bt_node->bt), r_hash);
}

static bool
prof_bt_node_keycomp(const void *k1, const void *k2) {
	const prof_bt_node_t *bt_node1 = (prof_bt_node_t *)k1;
	const prof_bt_node_t *bt_node2 = (prof_bt_node_t *)k2;
	return prof_bt_keycomp((void *)(&bt_node1->bt),
	    (void *)(&bt_node2->bt));
}

static void
prof_thr_node_hash(const void *key, size_t r_hash[2]) {
	const prof_thr_node_t *thr_node = (prof_thr_node_t *)key;
	hash(&thr_node->thr_uid, sizeof(uint64_t), 0x94122f35U, r_hash);
}

static bool
prof_thr_node_keycomp(const void *k1, const void *k2) {
	const prof_thr_node_t *thr_node1 = (prof_thr_node_t *)k1;
	const prof_thr_node_t *thr_node2 = (prof_thr_node_t *)k2;
	return thr_node1->thr_uid == thr_node2->thr_uid;
}

/* Used in unit tests. */
size_t
prof_log_bt_count(void) {
	cassert(config_prof);
	size_t cnt = 0;
	prof_bt_node_t *node = log_bt_first;
	while (node != NULL) {
		cnt++;
		node = node->next;
	}
	return cnt;
}

/* Used in unit tests. */
size_t
prof_log_alloc_count(void) {
	cassert(config_prof);
	size_t cnt = 0;
	prof_alloc_node_t *node = log_alloc_first;
	while (node != NULL) {
		cnt++;
		node = node->next;
	}
	return cnt;
}

/* Used in unit tests. */
size_t
prof_log_thr_count(void) {
	cassert(config_prof);
	size_t cnt = 0;
	prof_thr_node_t *node = log_thr_first;
	while (node != NULL) {
		cnt++;
		node = node->next;
	}
	return cnt;
}

/* Used in unit tests. */
bool
prof_log_is_logging(void) {
	cassert(config_prof);
	return prof_logging_state == prof_logging_state_started;
}

/* Used in unit tests. */
bool
prof_log_rep_check(void) {
	cassert(config_prof);
	if (prof_logging_state == prof_logging_state_stopped
	    && log_tables_initialized) {
		return true;
	}

	if (log_bt_last != NULL && log_bt_last->next != NULL) {
		return true;
	}
	if (log_thr_last != NULL && log_thr_last->next != NULL) {
		return true;
	}
	if (log_alloc_last != NULL && log_alloc_last->next != NULL) {
		return true;
	}

	size_t bt_count = prof_log_bt_count();
	size_t thr_count = prof_log_thr_count();
	size_t alloc_count = prof_log_alloc_count();


	if (prof_logging_state == prof_logging_state_stopped) {
		if (bt_count != 0 || thr_count != 0 || alloc_count || 0) {
			return true;
		}
	}

	prof_alloc_node_t *node = log_alloc_first;
	while (node != NULL) {
		if (node->alloc_bt_ind >= bt_count) {
			return true;
		}
		if (node->free_bt_ind >= bt_count) {
			return true;
		}
		if (node->alloc_thr_ind >= thr_count) {
			return true;
		}
		if (node->free_thr_ind >= thr_count) {
			return true;
		}
		if (node->alloc_time_ns > node->free_time_ns) {
			return true;
		}
		node = node->next;
	}

	return false;
}

/* Used in unit tests. */
void
prof_log_dummy_set(bool new_value) {
	cassert(config_prof);
	prof_log_dummy = new_value;
}

/* Used as an atexit function to stop logging on exit. */
static void
prof_log_stop_final(void) {
	tsd_t *tsd = tsd_fetch();
	prof_log_stop(tsd_tsdn(tsd));
}

JEMALLOC_COLD
bool
prof_log_start(tsdn_t *tsdn, const char *filename) {
	cassert(config_prof);

	if (!opt_prof) {
		return true;
	}

	bool ret = false;

	malloc_mutex_lock(tsdn, &log_mtx);

	static bool prof_log_atexit_called = false;
	if (!prof_log_atexit_called) {
		prof_log_atexit_called = true;
		if (atexit(prof_log_stop_final) != 0) {
			malloc_write("<jemalloc>: Error in atexit() "
			    "for logging\n");
			if (opt_abort) {
				abort();
			}
			ret = true;
			goto label_done;
		}
	}

	if (prof_logging_state != prof_logging_state_stopped) {
		ret = true;
	} else if (filename == NULL) {
		/* Make default name. */
		prof_get_default_filename(tsdn, log_filename, log_seq);
		log_seq++;
		prof_logging_state = prof_logging_state_started;
	} else if (strlen(filename) >= PROF_DUMP_FILENAME_LEN) {
		ret = true;
	} else {
		strcpy(log_filename, filename);
		prof_logging_state = prof_logging_state_started;
	}

	if (!ret) {
		nstime_prof_init_update(&log_start_timestamp);
	}
label_done:
	malloc_mutex_unlock(tsdn, &log_mtx);

	return ret;
}

struct prof_emitter_cb_arg_s {
	int fd;
	ssize_t ret;
};

static void
prof_emitter_write_cb(void *opaque, const char *to_write) {
	struct prof_emitter_cb_arg_s *arg =
	    (struct prof_emitter_cb_arg_s *)opaque;
	size_t bytes = strlen(to_write);
	if (prof_log_dummy) {
		return;
	}
	arg->ret = malloc_write_fd(arg->fd, to_write, bytes);
}

/*
 * prof_log_emit_{...} goes through the appropriate linked list, emitting each
 * node to the json and deallocating it.
 */
static void
prof_log_emit_threads(tsd_t *tsd, emitter_t *emitter) {
	emitter_json_array_kv_begin(emitter, "threads");
	prof_thr_node_t *thr_node = log_thr_first;
	prof_thr_node_t *thr_old_node;
	while (thr_node != NULL) {
		emitter_json_object_begin(emitter);

		emitter_json_kv(emitter, "thr_uid", emitter_type_uint64,
		    &thr_node->thr_uid);

		char *thr_name = thr_node->name;

		emitter_json_kv(emitter, "thr_name", emitter_type_string,
		    &thr_name);

		emitter_json_object_end(emitter);
		thr_old_node = thr_node;
		thr_node = thr_node->next;
		idalloctm(tsd_tsdn(tsd), thr_old_node, NULL, NULL, true, true);
	}
	emitter_json_array_end(emitter);
}

static void
prof_log_emit_traces(tsd_t *tsd, emitter_t *emitter) {
	emitter_json_array_kv_begin(emitter, "stack_traces");
	prof_bt_node_t *bt_node = log_bt_first;
	prof_bt_node_t *bt_old_node;
	/*
	 * Calculate how many hex digits we need: twice number of bytes, two for
	 * "0x", and then one more for terminating '\0'.
	 */
	char buf[2 * sizeof(intptr_t) + 3];
	size_t buf_sz = sizeof(buf);
	while (bt_node != NULL) {
		emitter_json_array_begin(emitter);
		size_t i;
		for (i = 0; i < bt_node->bt.len; i++) {
			malloc_snprintf(buf, buf_sz, "%p", bt_node->bt.vec[i]);
			char *trace_str = buf;
			emitter_json_value(emitter, emitter_type_string,
			    &trace_str);
		}
		emitter_json_array_end(emitter);

		bt_old_node = bt_node;
		bt_node = bt_node->next;
		idalloctm(tsd_tsdn(tsd), bt_old_node, NULL, NULL, true, true);
	}
	emitter_json_array_end(emitter);
}

static void
prof_log_emit_allocs(tsd_t *tsd, emitter_t *emitter) {
	emitter_json_array_kv_begin(emitter, "allocations");
	prof_alloc_node_t *alloc_node = log_alloc_first;
	prof_alloc_node_t *alloc_old_node;
	while (alloc_node != NULL) {
		emitter_json_object_begin(emitter);

		emitter_json_kv(emitter, "alloc_thread", emitter_type_size,
		    &alloc_node->alloc_thr_ind);

		emitter_json_kv(emitter, "free_thread", emitter_type_size,
		    &alloc_node->free_thr_ind);

		emitter_json_kv(emitter, "alloc_trace", emitter_type_size,
		    &alloc_node->alloc_bt_ind);

		emitter_json_kv(emitter, "free_trace", emitter_type_size,
		    &alloc_node->free_bt_ind);

		emitter_json_kv(emitter, "alloc_timestamp",
		    emitter_type_uint64, &alloc_node->alloc_time_ns);

		emitter_json_kv(emitter, "free_timestamp", emitter_type_uint64,
		    &alloc_node->free_time_ns);

		emitter_json_kv(emitter, "usize", emitter_type_uint64,
		    &alloc_node->usize);

		emitter_json_object_end(emitter);

		alloc_old_node = alloc_node;
		alloc_node = alloc_node->next;
		idalloctm(tsd_tsdn(tsd), alloc_old_node, NULL, NULL, true,
		    true);
	}
	emitter_json_array_end(emitter);
}

static void
prof_log_emit_metadata(emitter_t *emitter) {
	emitter_json_object_kv_begin(emitter, "info");

	nstime_t now;

	nstime_prof_init_update(&now);
	uint64_t ns = nstime_ns(&now) - nstime_ns(&log_start_timestamp);
	emitter_json_kv(emitter, "duration", emitter_type_uint64, &ns);

	char *vers = JEMALLOC_VERSION;
	emitter_json_kv(emitter, "version",
	    emitter_type_string, &vers);

	emitter_json_kv(emitter, "lg_sample_rate",
	    emitter_type_int, &lg_prof_sample);

	const char *res_type = prof_time_res_mode_names[opt_prof_time_res];
	emitter_json_kv(emitter, "prof_time_resolution", emitter_type_string,
	    &res_type);

	int pid = prof_getpid();
	emitter_json_kv(emitter, "pid", emitter_type_int, &pid);

	emitter_json_object_end(emitter);
}

#define PROF_LOG_STOP_BUFSIZE PROF_DUMP_BUFSIZE
JEMALLOC_COLD
bool
prof_log_stop(tsdn_t *tsdn) {
	cassert(config_prof);
	if (!opt_prof || !prof_booted) {
		return true;
	}

	tsd_t *tsd = tsdn_tsd(tsdn);
	malloc_mutex_lock(tsdn, &log_mtx);

	if (prof_logging_state != prof_logging_state_started) {
		malloc_mutex_unlock(tsdn, &log_mtx);
		return true;
	}

	/*
	 * Set the state to dumping. We'll set it to stopped when we're done.
	 * Since other threads won't be able to start/stop/log when the state is
	 * dumping, we don't have to hold the lock during the whole method.
	 */
	prof_logging_state = prof_logging_state_dumping;
	malloc_mutex_unlock(tsdn, &log_mtx);


	emitter_t emitter;

	/* Create a file. */

	int fd;
	if (prof_log_dummy) {
		fd = 0;
	} else {
		fd = creat(log_filename, 0644);
	}

	if (fd == -1) {
		malloc_printf("<jemalloc>: creat() for log file \"%s\" "
			      " failed with %d\n", log_filename, errno);
		if (opt_abort) {
			abort();
		}
		return true;
	}

	struct prof_emitter_cb_arg_s arg;
	arg.fd = fd;

	buf_writer_t buf_writer;
	buf_writer_init(tsdn, &buf_writer, prof_emitter_write_cb, &arg, NULL,
	    PROF_LOG_STOP_BUFSIZE);
	emitter_init(&emitter, emitter_output_json_compact, buf_writer_cb,
	    &buf_writer);

	emitter_begin(&emitter);
	prof_log_emit_metadata(&emitter);
	prof_log_emit_threads(tsd, &emitter);
	prof_log_emit_traces(tsd, &emitter);
	prof_log_emit_allocs(tsd, &emitter);
	emitter_end(&emitter);

	buf_writer_terminate(tsdn, &buf_writer);

	/* Reset global state. */
	if (log_tables_initialized) {
		ckh_delete(tsd, &log_bt_node_set);
		ckh_delete(tsd, &log_thr_node_set);
	}
	log_tables_initialized = false;
	log_bt_index = 0;
	log_thr_index = 0;
	log_bt_first = NULL;
	log_bt_last = NULL;
	log_thr_first = NULL;
	log_thr_last = NULL;
	log_alloc_first = NULL;
	log_alloc_last = NULL;

	malloc_mutex_lock(tsdn, &log_mtx);
	prof_logging_state = prof_logging_state_stopped;
	malloc_mutex_unlock(tsdn, &log_mtx);

	if (prof_log_dummy) {
		return false;
	}
	return close(fd) || arg.ret == -1;
}
#undef PROF_LOG_STOP_BUFSIZE

JEMALLOC_COLD
bool
prof_log_init(tsd_t *tsd) {
	cassert(config_prof);
	if (malloc_mutex_init(&log_mtx, "prof_log",
	    WITNESS_RANK_PROF_LOG, malloc_mutex_rank_exclusive)) {
		return true;
	}

	if (opt_prof_log) {
		prof_log_start(tsd_tsdn(tsd), NULL);
	}

	return false;
}

/******************************************************************************/
