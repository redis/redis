#include "test/jemalloc_test.h"

#include "jemalloc/internal/mpsc_queue.h"

typedef struct elem_s elem_t;
typedef ql_head(elem_t) elem_list_t;
typedef mpsc_queue(elem_t) elem_mpsc_queue_t;
struct elem_s {
	int thread;
	int idx;
	ql_elm(elem_t) link;
};

/* Include both proto and gen to make sure they match up. */
mpsc_queue_proto(static, elem_mpsc_queue_, elem_mpsc_queue_t, elem_t,
    elem_list_t);
mpsc_queue_gen(static, elem_mpsc_queue_, elem_mpsc_queue_t, elem_t,
    elem_list_t, link);

static void
init_elems_simple(elem_t *elems, int nelems, int thread) {
	for (int i = 0; i < nelems; i++) {
		elems[i].thread = thread;
		elems[i].idx = i;
		ql_elm_new(&elems[i], link);
	}
}

static void
check_elems_simple(elem_list_t *list, int nelems, int thread) {
	elem_t *elem;
	int next_idx = 0;
	ql_foreach(elem, list, link) {
		expect_d_lt(next_idx, nelems, "Too many list items");
		expect_d_eq(thread, elem->thread, "");
		expect_d_eq(next_idx, elem->idx, "List out of order");
		next_idx++;
	}
}

TEST_BEGIN(test_simple) {
	enum {NELEMS = 10};
	elem_t elems[NELEMS];
	elem_list_t list;
	elem_mpsc_queue_t queue;

	/* Pop empty queue onto empty list -> empty list */
	ql_new(&list);
	elem_mpsc_queue_new(&queue);
	elem_mpsc_queue_pop_batch(&queue, &list);
	expect_true(ql_empty(&list), "");

	/* Pop empty queue onto nonempty list -> list unchanged */
	ql_new(&list);
	elem_mpsc_queue_new(&queue);
	init_elems_simple(elems, NELEMS, 0);
	for (int i = 0; i < NELEMS; i++) {
		ql_tail_insert(&list, &elems[i], link);
	}
	elem_mpsc_queue_pop_batch(&queue, &list);
	check_elems_simple(&list, NELEMS, 0);

	/* Pop nonempty queue onto empty list -> list takes queue contents */
	ql_new(&list);
	elem_mpsc_queue_new(&queue);
	init_elems_simple(elems, NELEMS, 0);
	for (int i = 0; i < NELEMS; i++) {
		elem_mpsc_queue_push(&queue, &elems[i]);
	}
	elem_mpsc_queue_pop_batch(&queue, &list);
	check_elems_simple(&list, NELEMS, 0);

	/* Pop nonempty queue onto nonempty list -> list gains queue contents */
	ql_new(&list);
	elem_mpsc_queue_new(&queue);
	init_elems_simple(elems, NELEMS, 0);
	for (int i = 0; i < NELEMS / 2; i++) {
		ql_tail_insert(&list, &elems[i], link);
	}
	for (int i = NELEMS / 2; i < NELEMS; i++) {
		elem_mpsc_queue_push(&queue, &elems[i]);
	}
	elem_mpsc_queue_pop_batch(&queue, &list);
	check_elems_simple(&list, NELEMS, 0);

}
TEST_END

TEST_BEGIN(test_push_single_or_batch) {
	enum {
		BATCH_MAX = 10,
		/*
		 * We'll push i items one-at-a-time, then i items as a batch,
		 * then i items as a batch again, as i ranges from 1 to
		 * BATCH_MAX.  So we need 3 times the sum of the numbers from 1
		 * to BATCH_MAX elements total.
		 */
		NELEMS = 3 * BATCH_MAX * (BATCH_MAX - 1) / 2
	};
	elem_t elems[NELEMS];
	init_elems_simple(elems, NELEMS, 0);
	elem_list_t list;
	ql_new(&list);
	elem_mpsc_queue_t queue;
	elem_mpsc_queue_new(&queue);
	int next_idx = 0;
	for (int i = 1; i < 10; i++) {
		/* Push i items 1 at a time. */
		for (int j = 0; j < i; j++) {
			elem_mpsc_queue_push(&queue, &elems[next_idx]);
			next_idx++;
		}
		/* Push i items in batch. */
		for (int j = 0; j < i; j++) {
			ql_tail_insert(&list, &elems[next_idx], link);
			next_idx++;
		}
		elem_mpsc_queue_push_batch(&queue, &list);
		expect_true(ql_empty(&list), "Batch push should empty source");
		/*
		 * Push i items in batch, again.  This tests two batches
		 * proceeding one after the other.
		 */
		for (int j = 0; j < i; j++) {
			ql_tail_insert(&list, &elems[next_idx], link);
			next_idx++;
		}
		elem_mpsc_queue_push_batch(&queue, &list);
		expect_true(ql_empty(&list), "Batch push should empty source");
	}
	expect_d_eq(NELEMS, next_idx, "Miscomputed number of elems to push.");

	expect_true(ql_empty(&list), "");
	elem_mpsc_queue_pop_batch(&queue, &list);
	check_elems_simple(&list, NELEMS, 0);
}
TEST_END

TEST_BEGIN(test_multi_op) {
	enum {NELEMS = 20};
	elem_t elems[NELEMS];
	init_elems_simple(elems, NELEMS, 0);
	elem_list_t push_list;
	ql_new(&push_list);
	elem_list_t result_list;
	ql_new(&result_list);
	elem_mpsc_queue_t queue;
	elem_mpsc_queue_new(&queue);

	int next_idx = 0;
	/* Push first quarter 1-at-a-time. */
	for (int i = 0; i < NELEMS / 4; i++) {
		elem_mpsc_queue_push(&queue, &elems[next_idx]);
		next_idx++;
	}
	/* Push second quarter in batch. */
	for (int i = NELEMS / 4; i < NELEMS / 2; i++) {
		ql_tail_insert(&push_list, &elems[next_idx], link);
		next_idx++;
	}
	elem_mpsc_queue_push_batch(&queue, &push_list);
	/* Batch pop all pushed elements. */
	elem_mpsc_queue_pop_batch(&queue, &result_list);
	/* Push third quarter in batch. */
	for (int i = NELEMS / 2; i < 3 * NELEMS / 4; i++) {
		ql_tail_insert(&push_list, &elems[next_idx], link);
		next_idx++;
	}
	elem_mpsc_queue_push_batch(&queue, &push_list);
	/* Push last quarter one-at-a-time. */
	for (int i = 3 * NELEMS / 4; i < NELEMS; i++) {
		elem_mpsc_queue_push(&queue, &elems[next_idx]);
		next_idx++;
	}
	/* Pop them again.  Order of existing list should be preserved. */
	elem_mpsc_queue_pop_batch(&queue, &result_list);

	check_elems_simple(&result_list, NELEMS, 0);

}
TEST_END

typedef struct pusher_arg_s pusher_arg_t;
struct pusher_arg_s {
	elem_mpsc_queue_t *queue;
	int thread;
	elem_t *elems;
	int nelems;
};

typedef struct popper_arg_s popper_arg_t;
struct popper_arg_s {
	elem_mpsc_queue_t *queue;
	int npushers;
	int nelems_per_pusher;
	int *pusher_counts;
};

static void *
thd_pusher(void *void_arg) {
	pusher_arg_t *arg = (pusher_arg_t *)void_arg;
	int next_idx = 0;
	while (next_idx < arg->nelems) {
		/* Push 10 items in batch. */
		elem_list_t list;
		ql_new(&list);
		int limit = next_idx + 10;
		while (next_idx < arg->nelems && next_idx < limit) {
			ql_tail_insert(&list, &arg->elems[next_idx], link);
			next_idx++;
		}
		elem_mpsc_queue_push_batch(arg->queue, &list);
		/* Push 10 items one-at-a-time. */
		limit = next_idx + 10;
		while (next_idx < arg->nelems && next_idx < limit) {
			elem_mpsc_queue_push(arg->queue, &arg->elems[next_idx]);
			next_idx++;
		}

	}
	return NULL;
}

static void *
thd_popper(void *void_arg) {
	popper_arg_t *arg = (popper_arg_t *)void_arg;
	int done_pushers = 0;
	while (done_pushers < arg->npushers) {
		elem_list_t list;
		ql_new(&list);
		elem_mpsc_queue_pop_batch(arg->queue, &list);
		elem_t *elem;
		ql_foreach(elem, &list, link) {
			int thread = elem->thread;
			int idx = elem->idx;
			expect_d_eq(arg->pusher_counts[thread], idx,
			    "Thread's pushes reordered");
			arg->pusher_counts[thread]++;
			if (arg->pusher_counts[thread]
			    == arg->nelems_per_pusher) {
				done_pushers++;
			}
		}
	}
	return NULL;
}

TEST_BEGIN(test_multiple_threads) {
	enum {
		NPUSHERS = 4,
		NELEMS_PER_PUSHER = 1000*1000,
	};
	thd_t pushers[NPUSHERS];
	pusher_arg_t pusher_arg[NPUSHERS];

	thd_t popper;
	popper_arg_t popper_arg;

	elem_mpsc_queue_t queue;
	elem_mpsc_queue_new(&queue);

	elem_t *elems = calloc(NPUSHERS * NELEMS_PER_PUSHER, sizeof(elem_t));
	elem_t *elem_iter = elems;
	for (int i = 0; i < NPUSHERS; i++) {
		pusher_arg[i].queue = &queue;
		pusher_arg[i].thread = i;
		pusher_arg[i].elems = elem_iter;
		pusher_arg[i].nelems = NELEMS_PER_PUSHER;

		init_elems_simple(elem_iter, NELEMS_PER_PUSHER, i);
		elem_iter += NELEMS_PER_PUSHER;
	}
	popper_arg.queue = &queue;
	popper_arg.npushers = NPUSHERS;
	popper_arg.nelems_per_pusher = NELEMS_PER_PUSHER;
	int pusher_counts[NPUSHERS] = {0};
	popper_arg.pusher_counts = pusher_counts;

	thd_create(&popper, thd_popper, (void *)&popper_arg);
	for (int i = 0; i < NPUSHERS; i++) {
		thd_create(&pushers[i], thd_pusher, &pusher_arg[i]);
	}

	thd_join(popper, NULL);
	for (int i = 0; i < NPUSHERS; i++) {
		thd_join(pushers[i], NULL);
	}

	for (int i = 0; i < NPUSHERS; i++) {
		expect_d_eq(NELEMS_PER_PUSHER, pusher_counts[i], "");
	}

	free(elems);
}
TEST_END

int
main(void) {
	return test_no_reentrancy(
	    test_simple,
	    test_push_single_or_batch,
	    test_multi_op,
	    test_multiple_threads);
}
