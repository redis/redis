#include "test/jemalloc_test.h"

#include "jemalloc/internal/ql.h"

/* Number of ring entries, in [2..26]. */
#define NENTRIES 9

typedef struct list_s list_t;
typedef ql_head(list_t) list_head_t;

struct list_s {
	ql_elm(list_t) link;
	char id;
};

static void
test_empty_list(list_head_t *head) {
	list_t *t;
	unsigned i;

	expect_true(ql_empty(head), "Unexpected element for empty list");
	expect_ptr_null(ql_first(head), "Unexpected element for empty list");
	expect_ptr_null(ql_last(head, link),
	    "Unexpected element for empty list");

	i = 0;
	ql_foreach(t, head, link) {
		i++;
	}
	expect_u_eq(i, 0, "Unexpected element for empty list");

	i = 0;
	ql_reverse_foreach(t, head, link) {
		i++;
	}
	expect_u_eq(i, 0, "Unexpected element for empty list");
}

TEST_BEGIN(test_ql_empty) {
	list_head_t head;

	ql_new(&head);
	test_empty_list(&head);
}
TEST_END

static void
init_entries(list_t *entries, unsigned nentries) {
	unsigned i;

	for (i = 0; i < nentries; i++) {
		entries[i].id = 'a' + i;
		ql_elm_new(&entries[i], link);
	}
}

static void
test_entries_list(list_head_t *head, list_t *entries, unsigned nentries) {
	list_t *t;
	unsigned i;

	expect_false(ql_empty(head), "List should not be empty");
	expect_c_eq(ql_first(head)->id, entries[0].id, "Element id mismatch");
	expect_c_eq(ql_last(head, link)->id, entries[nentries-1].id,
	    "Element id mismatch");

	i = 0;
	ql_foreach(t, head, link) {
		expect_c_eq(t->id, entries[i].id, "Element id mismatch");
		i++;
	}

	i = 0;
	ql_reverse_foreach(t, head, link) {
		expect_c_eq(t->id, entries[nentries-i-1].id,
		    "Element id mismatch");
		i++;
	}

	for (i = 0; i < nentries-1; i++) {
		t = ql_next(head, &entries[i], link);
		expect_c_eq(t->id, entries[i+1].id, "Element id mismatch");
	}
	expect_ptr_null(ql_next(head, &entries[nentries-1], link),
	    "Unexpected element");

	expect_ptr_null(ql_prev(head, &entries[0], link), "Unexpected element");
	for (i = 1; i < nentries; i++) {
		t = ql_prev(head, &entries[i], link);
		expect_c_eq(t->id, entries[i-1].id, "Element id mismatch");
	}
}

TEST_BEGIN(test_ql_tail_insert) {
	list_head_t head;
	list_t entries[NENTRIES];
	unsigned i;

	ql_new(&head);
	init_entries(entries, sizeof(entries)/sizeof(list_t));
	for (i = 0; i < NENTRIES; i++) {
		ql_tail_insert(&head, &entries[i], link);
	}

	test_entries_list(&head, entries, NENTRIES);
}
TEST_END

TEST_BEGIN(test_ql_tail_remove) {
	list_head_t head;
	list_t entries[NENTRIES];
	unsigned i;

	ql_new(&head);
	init_entries(entries, sizeof(entries)/sizeof(list_t));
	for (i = 0; i < NENTRIES; i++) {
		ql_tail_insert(&head, &entries[i], link);
	}

	for (i = 0; i < NENTRIES; i++) {
		test_entries_list(&head, entries, NENTRIES-i);
		ql_tail_remove(&head, list_t, link);
	}
	test_empty_list(&head);
}
TEST_END

TEST_BEGIN(test_ql_head_insert) {
	list_head_t head;
	list_t entries[NENTRIES];
	unsigned i;

	ql_new(&head);
	init_entries(entries, sizeof(entries)/sizeof(list_t));
	for (i = 0; i < NENTRIES; i++) {
		ql_head_insert(&head, &entries[NENTRIES-i-1], link);
	}

	test_entries_list(&head, entries, NENTRIES);
}
TEST_END

TEST_BEGIN(test_ql_head_remove) {
	list_head_t head;
	list_t entries[NENTRIES];
	unsigned i;

	ql_new(&head);
	init_entries(entries, sizeof(entries)/sizeof(list_t));
	for (i = 0; i < NENTRIES; i++) {
		ql_head_insert(&head, &entries[NENTRIES-i-1], link);
	}

	for (i = 0; i < NENTRIES; i++) {
		test_entries_list(&head, &entries[i], NENTRIES-i);
		ql_head_remove(&head, list_t, link);
	}
	test_empty_list(&head);
}
TEST_END

TEST_BEGIN(test_ql_insert) {
	list_head_t head;
	list_t entries[8];
	list_t *a, *b, *c, *d, *e, *f, *g, *h;

	ql_new(&head);
	init_entries(entries, sizeof(entries)/sizeof(list_t));
	a = &entries[0];
	b = &entries[1];
	c = &entries[2];
	d = &entries[3];
	e = &entries[4];
	f = &entries[5];
	g = &entries[6];
	h = &entries[7];

	/*
	 * ql_remove(), ql_before_insert(), and ql_after_insert() are used
	 * internally by other macros that are already tested, so there's no
	 * need to test them completely.  However, insertion/deletion from the
	 * middle of lists is not otherwise tested; do so here.
	 */
	ql_tail_insert(&head, f, link);
	ql_before_insert(&head, f, b, link);
	ql_before_insert(&head, f, c, link);
	ql_after_insert(f, h, link);
	ql_after_insert(f, g, link);
	ql_before_insert(&head, b, a, link);
	ql_after_insert(c, d, link);
	ql_before_insert(&head, f, e, link);

	test_entries_list(&head, entries, sizeof(entries)/sizeof(list_t));
}
TEST_END

static void
test_concat_split_entries(list_t *entries, unsigned nentries_a,
    unsigned nentries_b) {
	init_entries(entries, nentries_a + nentries_b);

	list_head_t head_a;
	ql_new(&head_a);
	for (unsigned i = 0; i < nentries_a; i++) {
		ql_tail_insert(&head_a, &entries[i], link);
	}
	if (nentries_a == 0) {
		test_empty_list(&head_a);
	} else {
		test_entries_list(&head_a, entries, nentries_a);
	}

	list_head_t head_b;
	ql_new(&head_b);
	for (unsigned i = 0; i < nentries_b; i++) {
		ql_tail_insert(&head_b, &entries[nentries_a + i], link);
	}
	if (nentries_b == 0) {
		test_empty_list(&head_b);
	} else {
		test_entries_list(&head_b, entries + nentries_a, nentries_b);
	}

	ql_concat(&head_a, &head_b, link);
	if (nentries_a + nentries_b == 0) {
		test_empty_list(&head_a);
	} else {
		test_entries_list(&head_a, entries, nentries_a + nentries_b);
	}
	test_empty_list(&head_b);

	if (nentries_b == 0) {
		return;
	}

	list_head_t head_c;
	ql_split(&head_a, &entries[nentries_a], &head_c, link);
	if (nentries_a == 0) {
		test_empty_list(&head_a);
	} else {
		test_entries_list(&head_a, entries, nentries_a);
	}
	test_entries_list(&head_c, entries + nentries_a, nentries_b);
}

TEST_BEGIN(test_ql_concat_split) {
	list_t entries[NENTRIES];

	test_concat_split_entries(entries, 0, 0);

	test_concat_split_entries(entries, 0, 1);
	test_concat_split_entries(entries, 1, 0);

	test_concat_split_entries(entries, 0, NENTRIES);
	test_concat_split_entries(entries, 1, NENTRIES - 1);
	test_concat_split_entries(entries, NENTRIES / 2,
	    NENTRIES - NENTRIES / 2);
	test_concat_split_entries(entries, NENTRIES - 1, 1);
	test_concat_split_entries(entries, NENTRIES, 0);
}
TEST_END

TEST_BEGIN(test_ql_rotate) {
	list_head_t head;
	list_t entries[NENTRIES];
	unsigned i;

	ql_new(&head);
	init_entries(entries, sizeof(entries)/sizeof(list_t));
	for (i = 0; i < NENTRIES; i++) {
		ql_tail_insert(&head, &entries[i], link);
	}

	char head_id = ql_first(&head)->id;
	for (i = 0; i < NENTRIES; i++) {
		assert_c_eq(ql_first(&head)->id, head_id, "");
		ql_rotate(&head, link);
		assert_c_eq(ql_last(&head, link)->id, head_id, "");
		head_id++;
	}
	test_entries_list(&head, entries, NENTRIES);
}
TEST_END

TEST_BEGIN(test_ql_move) {
	list_head_t head_dest, head_src;
	list_t entries[NENTRIES];
	unsigned i;

	ql_new(&head_src);
	ql_move(&head_dest, &head_src);
	test_empty_list(&head_src);
	test_empty_list(&head_dest);

	init_entries(entries, sizeof(entries)/sizeof(list_t));
	for (i = 0; i < NENTRIES; i++) {
		ql_tail_insert(&head_src, &entries[i], link);
	}
	ql_move(&head_dest, &head_src);
	test_empty_list(&head_src);
	test_entries_list(&head_dest, entries, NENTRIES);
}
TEST_END

int
main(void) {
	return test(
	    test_ql_empty,
	    test_ql_tail_insert,
	    test_ql_tail_remove,
	    test_ql_head_insert,
	    test_ql_head_remove,
	    test_ql_insert,
	    test_ql_concat_split,
	    test_ql_rotate,
	    test_ql_move);
}
