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

	assert_ptr_null(ql_first(head), "Unexpected element for empty list");
	assert_ptr_null(ql_last(head, link),
	    "Unexpected element for empty list");

	i = 0;
	ql_foreach(t, head, link) {
		i++;
	}
	assert_u_eq(i, 0, "Unexpected element for empty list");

	i = 0;
	ql_reverse_foreach(t, head, link) {
		i++;
	}
	assert_u_eq(i, 0, "Unexpected element for empty list");
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

	assert_c_eq(ql_first(head)->id, entries[0].id, "Element id mismatch");
	assert_c_eq(ql_last(head, link)->id, entries[nentries-1].id,
	    "Element id mismatch");

	i = 0;
	ql_foreach(t, head, link) {
		assert_c_eq(t->id, entries[i].id, "Element id mismatch");
		i++;
	}

	i = 0;
	ql_reverse_foreach(t, head, link) {
		assert_c_eq(t->id, entries[nentries-i-1].id,
		    "Element id mismatch");
		i++;
	}

	for (i = 0; i < nentries-1; i++) {
		t = ql_next(head, &entries[i], link);
		assert_c_eq(t->id, entries[i+1].id, "Element id mismatch");
	}
	assert_ptr_null(ql_next(head, &entries[nentries-1], link),
	    "Unexpected element");

	assert_ptr_null(ql_prev(head, &entries[0], link), "Unexpected element");
	for (i = 1; i < nentries; i++) {
		t = ql_prev(head, &entries[i], link);
		assert_c_eq(t->id, entries[i-1].id, "Element id mismatch");
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

int
main(void) {
	return test(
	    test_ql_empty,
	    test_ql_tail_insert,
	    test_ql_tail_remove,
	    test_ql_head_insert,
	    test_ql_head_remove,
	    test_ql_insert);
}
