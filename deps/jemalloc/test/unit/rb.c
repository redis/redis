#include "test/jemalloc_test.h"

#include <stdlib.h>

#include "jemalloc/internal/rb.h"

#define rbtn_black_height(a_type, a_field, a_rbt, r_height) do {	\
	a_type *rbp_bh_t;						\
	for (rbp_bh_t = (a_rbt)->rbt_root, (r_height) = 0; rbp_bh_t !=	\
	    NULL; rbp_bh_t = rbtn_left_get(a_type, a_field,		\
	    rbp_bh_t)) {						\
		if (!rbtn_red_get(a_type, a_field, rbp_bh_t)) {		\
		(r_height)++;						\
		}							\
	}								\
} while (0)

static bool summarize_always_returns_true = false;

typedef struct node_s node_t;
struct node_s {
#define NODE_MAGIC 0x9823af7e
	uint32_t magic;
	rb_node(node_t) link;
	/* Order used by nodes. */
	uint64_t key;
	/*
	 * Our made-up summary property is "specialness", with summarization
	 * taking the max.
	 */
	uint64_t specialness;

	/*
	 * Used by some of the test randomization to avoid double-removing
	 * nodes.
	 */
	bool mid_remove;

	/*
	 * To test searching functionality, we want to temporarily weaken the
	 * ordering to allow non-equal nodes that nevertheless compare equal.
	 */
	bool allow_duplicates;

	/*
	 * In check_consistency, it's handy to know a node's rank in the tree;
	 * this tracks it (but only there; not all tests use this).
	 */
	int rank;
	int filtered_rank;

	/*
	 * Replicate the internal structure of the tree, to make sure the
	 * implementation doesn't miss any updates.
	 */
	const node_t *summary_lchild;
	const node_t *summary_rchild;
	uint64_t summary_max_specialness;
};

static int
node_cmp(const node_t *a, const node_t *b) {
	int ret;

	expect_u32_eq(a->magic, NODE_MAGIC, "Bad magic");
	expect_u32_eq(b->magic, NODE_MAGIC, "Bad magic");

	ret = (a->key > b->key) - (a->key < b->key);
	if (ret == 0 && !a->allow_duplicates) {
		/*
		 * Duplicates are not allowed in the tree, so force an
		 * arbitrary ordering for non-identical items with equal keys,
		 * unless the user is searching and wants to allow the
		 * duplicate.
		 */
		ret = (((uintptr_t)a) > ((uintptr_t)b))
		    - (((uintptr_t)a) < ((uintptr_t)b));
	}
	return ret;
}

static uint64_t
node_subtree_specialness(node_t *n, const node_t *lchild,
    const node_t *rchild) {
	uint64_t subtree_specialness = n->specialness;
	if (lchild != NULL
	    && lchild->summary_max_specialness > subtree_specialness) {
		subtree_specialness = lchild->summary_max_specialness;
	}
	if (rchild != NULL
	    && rchild->summary_max_specialness > subtree_specialness) {
		subtree_specialness = rchild->summary_max_specialness;
	}
	return subtree_specialness;
}

static bool
node_summarize(node_t *a, const node_t *lchild, const node_t *rchild) {
	uint64_t new_summary_max_specialness = node_subtree_specialness(
	    a, lchild, rchild);
	bool changed = (a->summary_lchild != lchild)
	    || (a->summary_rchild != rchild)
	    || (new_summary_max_specialness != a->summary_max_specialness);
	a->summary_max_specialness = new_summary_max_specialness;
	a->summary_lchild = lchild;
	a->summary_rchild = rchild;
	return changed || summarize_always_returns_true;
}

typedef rb_tree(node_t) tree_t;
rb_summarized_proto(static, tree_, tree_t, node_t);
rb_summarized_gen(static, tree_, tree_t, node_t, link, node_cmp,
    node_summarize);

static bool
specialness_filter_node(void *ctx, node_t *node) {
	uint64_t specialness = *(uint64_t *)ctx;
	return node->specialness >= specialness;
}

static bool
specialness_filter_subtree(void *ctx, node_t *node) {
	uint64_t specialness = *(uint64_t *)ctx;
	return node->summary_max_specialness >= specialness;
}

static node_t *
tree_iterate_cb(tree_t *tree, node_t *node, void *data) {
	unsigned *i = (unsigned *)data;
	node_t *search_node;

	expect_u32_eq(node->magic, NODE_MAGIC, "Bad magic");

	/* Test rb_search(). */
	search_node = tree_search(tree, node);
	expect_ptr_eq(search_node, node,
	    "tree_search() returned unexpected node");

	/* Test rb_nsearch(). */
	search_node = tree_nsearch(tree, node);
	expect_ptr_eq(search_node, node,
	    "tree_nsearch() returned unexpected node");

	/* Test rb_psearch(). */
	search_node = tree_psearch(tree, node);
	expect_ptr_eq(search_node, node,
	    "tree_psearch() returned unexpected node");

	(*i)++;

	return NULL;
}

TEST_BEGIN(test_rb_empty) {
	tree_t tree;
	node_t key;

	tree_new(&tree);

	expect_true(tree_empty(&tree), "Tree should be empty");
	expect_ptr_null(tree_first(&tree), "Unexpected node");
	expect_ptr_null(tree_last(&tree), "Unexpected node");

	key.key = 0;
	key.magic = NODE_MAGIC;
	expect_ptr_null(tree_search(&tree, &key), "Unexpected node");

	key.key = 0;
	key.magic = NODE_MAGIC;
	expect_ptr_null(tree_nsearch(&tree, &key), "Unexpected node");

	key.key = 0;
	key.magic = NODE_MAGIC;
	expect_ptr_null(tree_psearch(&tree, &key), "Unexpected node");

	unsigned nodes = 0;
	tree_iter_filtered(&tree, NULL, &tree_iterate_cb,
	    &nodes, &specialness_filter_node, &specialness_filter_subtree,
	    NULL);
	expect_u_eq(0, nodes, "");

	nodes = 0;
	tree_reverse_iter_filtered(&tree, NULL, &tree_iterate_cb,
	    &nodes, &specialness_filter_node, &specialness_filter_subtree,
	    NULL);
	expect_u_eq(0, nodes, "");

	expect_ptr_null(tree_first_filtered(&tree, &specialness_filter_node,
	    &specialness_filter_subtree, NULL), "");
	expect_ptr_null(tree_last_filtered(&tree, &specialness_filter_node,
	    &specialness_filter_subtree, NULL), "");

	key.key = 0;
	key.magic = NODE_MAGIC;
	expect_ptr_null(tree_search_filtered(&tree, &key,
	    &specialness_filter_node, &specialness_filter_subtree, NULL), "");
	expect_ptr_null(tree_nsearch_filtered(&tree, &key,
	    &specialness_filter_node, &specialness_filter_subtree, NULL), "");
	expect_ptr_null(tree_psearch_filtered(&tree, &key,
	    &specialness_filter_node, &specialness_filter_subtree, NULL), "");
}
TEST_END

static unsigned
tree_recurse(node_t *node, unsigned black_height, unsigned black_depth) {
	unsigned ret = 0;
	node_t *left_node;
	node_t *right_node;

	if (node == NULL) {
		return ret;
	}

	left_node = rbtn_left_get(node_t, link, node);
	right_node = rbtn_right_get(node_t, link, node);

	expect_ptr_eq(left_node, node->summary_lchild,
	    "summary missed a tree update");
	expect_ptr_eq(right_node, node->summary_rchild,
	    "summary missed a tree update");

	uint64_t expected_subtree_specialness = node_subtree_specialness(node,
	    left_node, right_node);
	expect_u64_eq(expected_subtree_specialness,
	    node->summary_max_specialness, "Incorrect summary");

	if (!rbtn_red_get(node_t, link, node)) {
		black_depth++;
	}

	/* Red nodes must be interleaved with black nodes. */
	if (rbtn_red_get(node_t, link, node)) {
		if (left_node != NULL) {
			expect_false(rbtn_red_get(node_t, link, left_node),
				"Node should be black");
		}
		if (right_node != NULL) {
			expect_false(rbtn_red_get(node_t, link, right_node),
			    "Node should be black");
		}
	}

	/* Self. */
	expect_u32_eq(node->magic, NODE_MAGIC, "Bad magic");

	/* Left subtree. */
	if (left_node != NULL) {
		ret += tree_recurse(left_node, black_height, black_depth);
	} else {
		ret += (black_depth != black_height);
	}

	/* Right subtree. */
	if (right_node != NULL) {
		ret += tree_recurse(right_node, black_height, black_depth);
	} else {
		ret += (black_depth != black_height);
	}

	return ret;
}

static unsigned
tree_iterate(tree_t *tree) {
	unsigned i;

	i = 0;
	tree_iter(tree, NULL, tree_iterate_cb, (void *)&i);

	return i;
}

static unsigned
tree_iterate_reverse(tree_t *tree) {
	unsigned i;

	i = 0;
	tree_reverse_iter(tree, NULL, tree_iterate_cb, (void *)&i);

	return i;
}

static void
node_remove(tree_t *tree, node_t *node, unsigned nnodes) {
	node_t *search_node;
	unsigned black_height, imbalances;

	tree_remove(tree, node);

	/* Test rb_nsearch(). */
	search_node = tree_nsearch(tree, node);
	if (search_node != NULL) {
		expect_u64_ge(search_node->key, node->key,
		    "Key ordering error");
	}

	/* Test rb_psearch(). */
	search_node = tree_psearch(tree, node);
	if (search_node != NULL) {
		expect_u64_le(search_node->key, node->key,
		    "Key ordering error");
	}

	node->magic = 0;

	rbtn_black_height(node_t, link, tree, black_height);
	imbalances = tree_recurse(tree->rbt_root, black_height, 0);
	expect_u_eq(imbalances, 0, "Tree is unbalanced");
	expect_u_eq(tree_iterate(tree), nnodes-1,
	    "Unexpected node iteration count");
	expect_u_eq(tree_iterate_reverse(tree), nnodes-1,
	    "Unexpected node iteration count");
}

static node_t *
remove_iterate_cb(tree_t *tree, node_t *node, void *data) {
	unsigned *nnodes = (unsigned *)data;
	node_t *ret = tree_next(tree, node);

	node_remove(tree, node, *nnodes);

	return ret;
}

static node_t *
remove_reverse_iterate_cb(tree_t *tree, node_t *node, void *data) {
	unsigned *nnodes = (unsigned *)data;
	node_t *ret = tree_prev(tree, node);

	node_remove(tree, node, *nnodes);

	return ret;
}

static void
destroy_cb(node_t *node, void *data) {
	unsigned *nnodes = (unsigned *)data;

	expect_u_gt(*nnodes, 0, "Destruction removed too many nodes");
	(*nnodes)--;
}

TEST_BEGIN(test_rb_random) {
	enum {
		NNODES = 25,
		NBAGS = 500,
		SEED = 42
	};
	sfmt_t *sfmt;
	uint64_t bag[NNODES];
	tree_t tree;
	node_t nodes[NNODES];
	unsigned i, j, k, black_height, imbalances;

	sfmt = init_gen_rand(SEED);
	for (i = 0; i < NBAGS; i++) {
		switch (i) {
		case 0:
			/* Insert in order. */
			for (j = 0; j < NNODES; j++) {
				bag[j] = j;
			}
			break;
		case 1:
			/* Insert in reverse order. */
			for (j = 0; j < NNODES; j++) {
				bag[j] = NNODES - j - 1;
			}
			break;
		default:
			for (j = 0; j < NNODES; j++) {
				bag[j] = gen_rand64_range(sfmt, NNODES);
			}
		}

		/*
		 * We alternate test behavior with a period of 2 here, and a
		 * period of 5 down below, so there's no cycle in which certain
		 * combinations get omitted.
		 */
		summarize_always_returns_true = (i % 2 == 0);

		for (j = 1; j <= NNODES; j++) {
			/* Initialize tree and nodes. */
			tree_new(&tree);
			for (k = 0; k < j; k++) {
				nodes[k].magic = NODE_MAGIC;
				nodes[k].key = bag[k];
				nodes[k].specialness = gen_rand64_range(sfmt,
				    NNODES);
				nodes[k].mid_remove = false;
				nodes[k].allow_duplicates = false;
				nodes[k].summary_lchild = NULL;
				nodes[k].summary_rchild = NULL;
				nodes[k].summary_max_specialness = 0;
			}

			/* Insert nodes. */
			for (k = 0; k < j; k++) {
				tree_insert(&tree, &nodes[k]);

				rbtn_black_height(node_t, link, &tree,
				    black_height);
				imbalances = tree_recurse(tree.rbt_root,
				    black_height, 0);
				expect_u_eq(imbalances, 0,
				    "Tree is unbalanced");

				expect_u_eq(tree_iterate(&tree), k+1,
				    "Unexpected node iteration count");
				expect_u_eq(tree_iterate_reverse(&tree), k+1,
				    "Unexpected node iteration count");

				expect_false(tree_empty(&tree),
				    "Tree should not be empty");
				expect_ptr_not_null(tree_first(&tree),
				    "Tree should not be empty");
				expect_ptr_not_null(tree_last(&tree),
				    "Tree should not be empty");

				tree_next(&tree, &nodes[k]);
				tree_prev(&tree, &nodes[k]);
			}

			/* Remove nodes. */
			switch (i % 5) {
			case 0:
				for (k = 0; k < j; k++) {
					node_remove(&tree, &nodes[k], j - k);
				}
				break;
			case 1:
				for (k = j; k > 0; k--) {
					node_remove(&tree, &nodes[k-1], k);
				}
				break;
			case 2: {
				node_t *start;
				unsigned nnodes = j;

				start = NULL;
				do {
					start = tree_iter(&tree, start,
					    remove_iterate_cb, (void *)&nnodes);
					nnodes--;
				} while (start != NULL);
				expect_u_eq(nnodes, 0,
				    "Removal terminated early");
				break;
			} case 3: {
				node_t *start;
				unsigned nnodes = j;

				start = NULL;
				do {
					start = tree_reverse_iter(&tree, start,
					    remove_reverse_iterate_cb,
					    (void *)&nnodes);
					nnodes--;
				} while (start != NULL);
				expect_u_eq(nnodes, 0,
				    "Removal terminated early");
				break;
			} case 4: {
				unsigned nnodes = j;
				tree_destroy(&tree, destroy_cb, &nnodes);
				expect_u_eq(nnodes, 0,
				    "Destruction terminated early");
				break;
			} default:
				not_reached();
			}
		}
	}
	fini_gen_rand(sfmt);
}
TEST_END

static void
expect_simple_consistency(tree_t *tree, uint64_t specialness,
    bool expected_empty, node_t *expected_first, node_t *expected_last) {
	bool empty;
	node_t *first;
	node_t *last;

	empty = tree_empty_filtered(tree, &specialness_filter_node,
	    &specialness_filter_subtree, &specialness);
	expect_b_eq(expected_empty, empty, "");

	first = tree_first_filtered(tree,
	    &specialness_filter_node, &specialness_filter_subtree,
	    (void *)&specialness);
	expect_ptr_eq(expected_first, first, "");

	last = tree_last_filtered(tree,
	    &specialness_filter_node, &specialness_filter_subtree,
	    (void *)&specialness);
	expect_ptr_eq(expected_last, last, "");
}

TEST_BEGIN(test_rb_filter_simple) {
	enum {FILTER_NODES = 10};
	node_t nodes[FILTER_NODES];
	for (unsigned i = 0; i < FILTER_NODES; i++) {
		nodes[i].magic = NODE_MAGIC;
		nodes[i].key = i;
		if (i == 0) {
			nodes[i].specialness = 0;
		} else {
			nodes[i].specialness = ffs_u(i);
		}
		nodes[i].mid_remove = false;
		nodes[i].allow_duplicates = false;
		nodes[i].summary_lchild = NULL;
		nodes[i].summary_rchild = NULL;
		nodes[i].summary_max_specialness = 0;
	}

	summarize_always_returns_true = false;

	tree_t tree;
	tree_new(&tree);

	/* Should be empty */
	expect_simple_consistency(&tree, /* specialness */ 0, /* empty */ true,
	    /* first */ NULL, /* last */ NULL);

	/* Fill in just the odd nodes. */
	for (int i = 1; i < FILTER_NODES; i += 2) {
		tree_insert(&tree, &nodes[i]);
	}

	/* A search for an odd node should succeed. */
	expect_simple_consistency(&tree, /* specialness */ 0, /* empty */ false,
	    /* first */ &nodes[1], /* last */ &nodes[9]);

	/* But a search for an even one should fail. */
	expect_simple_consistency(&tree, /* specialness */ 1, /* empty */ true,
	    /* first */ NULL, /* last */ NULL);

	/* Now we add an even. */
	tree_insert(&tree, &nodes[4]);
	expect_simple_consistency(&tree, /* specialness */ 1, /* empty */ false,
	    /* first */ &nodes[4], /* last */ &nodes[4]);

	/* A smaller even, and a larger even. */
	tree_insert(&tree, &nodes[2]);
	tree_insert(&tree, &nodes[8]);

	/*
	 * A first-search (resp. last-search) for an even should switch to the
	 * lower (higher) one, now that it's been added.
	 */
	expect_simple_consistency(&tree, /* specialness */ 1, /* empty */ false,
	    /* first */ &nodes[2], /* last */ &nodes[8]);

	/*
	 * If we remove 2, a first-search we should go back to 4, while a
	 * last-search should remain unchanged.
	 */
	tree_remove(&tree, &nodes[2]);
	expect_simple_consistency(&tree, /* specialness */ 1, /* empty */ false,
	    /* first */ &nodes[4], /* last */ &nodes[8]);

	/* Reinsert 2, then find it again. */
	tree_insert(&tree, &nodes[2]);
	expect_simple_consistency(&tree, /* specialness */ 1, /* empty */ false,
	    /* first */ &nodes[2], /* last */ &nodes[8]);

	/* Searching for a multiple of 4 should not have changed. */
	expect_simple_consistency(&tree, /* specialness */ 2, /* empty */ false,
	    /* first */ &nodes[4], /* last */ &nodes[8]);

	/* And a multiple of 8 */
	expect_simple_consistency(&tree, /* specialness */ 3, /* empty */ false,
	    /* first */ &nodes[8], /* last */ &nodes[8]);

	/* But not a multiple of 16 */
	expect_simple_consistency(&tree, /* specialness */ 4, /* empty */ true,
	    /* first */ NULL, /* last */ NULL);
}
TEST_END

typedef struct iter_ctx_s iter_ctx_t;
struct iter_ctx_s {
	int ncalls;
	node_t *last_node;

	int ncalls_max;
	bool forward;
};

static node_t *
tree_iterate_filtered_cb(tree_t *tree, node_t *node, void *arg) {
	iter_ctx_t *ctx = (iter_ctx_t *)arg;
	ctx->ncalls++;
	expect_u64_ge(node->specialness, 1,
	    "Should only invoke cb on nodes that pass the filter");
	if (ctx->last_node != NULL) {
		if (ctx->forward) {
			expect_d_lt(node_cmp(ctx->last_node, node), 0,
			    "Incorrect iteration order");
		} else {
			expect_d_gt(node_cmp(ctx->last_node, node), 0,
			    "Incorrect iteration order");
		}
	}
	ctx->last_node = node;
	if (ctx->ncalls == ctx->ncalls_max) {
		return node;
	}
	return NULL;
}

static int
qsort_node_cmp(const void *ap, const void *bp) {
	node_t *a = *(node_t **)ap;
	node_t *b = *(node_t **)bp;
	return node_cmp(a, b);
}

#define UPDATE_TEST_MAX 100
static void
check_consistency(tree_t *tree, node_t nodes[UPDATE_TEST_MAX], int nnodes) {
	uint64_t specialness = 1;

	bool empty;
	bool real_empty = true;
	node_t *first;
	node_t *real_first = NULL;
	node_t *last;
	node_t *real_last = NULL;
	for (int i = 0; i < nnodes; i++) {
		if (nodes[i].specialness >= specialness) {
			real_empty = false;
			if (real_first == NULL
			    || node_cmp(&nodes[i], real_first) < 0) {
				real_first = &nodes[i];
			}
			if (real_last == NULL
			    || node_cmp(&nodes[i], real_last) > 0) {
				real_last = &nodes[i];
			}
		}
	}

	empty = tree_empty_filtered(tree, &specialness_filter_node,
	    &specialness_filter_subtree, &specialness);
	expect_b_eq(real_empty, empty, "");

	first = tree_first_filtered(tree, &specialness_filter_node,
	    &specialness_filter_subtree, &specialness);
	expect_ptr_eq(real_first, first, "");

	last = tree_last_filtered(tree, &specialness_filter_node,
	    &specialness_filter_subtree, &specialness);
	expect_ptr_eq(real_last, last, "");

	for (int i = 0; i < nnodes; i++) {
		node_t *next_filtered;
		node_t *real_next_filtered = NULL;
		node_t *prev_filtered;
		node_t *real_prev_filtered = NULL;
		for (int j = 0; j < nnodes; j++) {
			if (nodes[j].specialness < specialness) {
				continue;
			}
			if (node_cmp(&nodes[j], &nodes[i]) < 0
			    && (real_prev_filtered == NULL
			    || node_cmp(&nodes[j], real_prev_filtered) > 0)) {
				real_prev_filtered = &nodes[j];
			}
			if (node_cmp(&nodes[j], &nodes[i]) > 0
			    && (real_next_filtered == NULL
			    || node_cmp(&nodes[j], real_next_filtered) < 0)) {
				real_next_filtered = &nodes[j];
			}
		}
		next_filtered = tree_next_filtered(tree, &nodes[i],
		    &specialness_filter_node, &specialness_filter_subtree,
		    &specialness);
		expect_ptr_eq(real_next_filtered, next_filtered, "");

		prev_filtered = tree_prev_filtered(tree, &nodes[i],
		    &specialness_filter_node, &specialness_filter_subtree,
		    &specialness);
		expect_ptr_eq(real_prev_filtered, prev_filtered, "");

		node_t *search_filtered;
		node_t *real_search_filtered;
		node_t *nsearch_filtered;
		node_t *real_nsearch_filtered;
		node_t *psearch_filtered;
		node_t *real_psearch_filtered;

		/*
		 * search, nsearch, psearch from a node before nodes[i] in the
		 * ordering.
		 */
		node_t before;
		before.magic = NODE_MAGIC;
		before.key = nodes[i].key - 1;
		before.allow_duplicates = false;
		real_search_filtered = NULL;
		search_filtered = tree_search_filtered(tree, &before,
		    &specialness_filter_node, &specialness_filter_subtree,
		    &specialness);
		expect_ptr_eq(real_search_filtered, search_filtered, "");

		real_nsearch_filtered = (nodes[i].specialness >= specialness ?
		    &nodes[i] : real_next_filtered);
		nsearch_filtered = tree_nsearch_filtered(tree, &before,
		    &specialness_filter_node, &specialness_filter_subtree,
		    &specialness);
		expect_ptr_eq(real_nsearch_filtered, nsearch_filtered, "");

		real_psearch_filtered = real_prev_filtered;
		psearch_filtered = tree_psearch_filtered(tree, &before,
		    &specialness_filter_node, &specialness_filter_subtree,
		    &specialness);
		expect_ptr_eq(real_psearch_filtered, psearch_filtered, "");

		/* search, nsearch, psearch from nodes[i] */
		real_search_filtered = (nodes[i].specialness >= specialness ?
		    &nodes[i] : NULL);
		search_filtered = tree_search_filtered(tree, &nodes[i],
		    &specialness_filter_node, &specialness_filter_subtree,
		    &specialness);
		expect_ptr_eq(real_search_filtered, search_filtered, "");

		real_nsearch_filtered = (nodes[i].specialness >= specialness ?
		    &nodes[i] : real_next_filtered);
		nsearch_filtered = tree_nsearch_filtered(tree, &nodes[i],
		    &specialness_filter_node, &specialness_filter_subtree,
		    &specialness);
		expect_ptr_eq(real_nsearch_filtered, nsearch_filtered, "");

		real_psearch_filtered = (nodes[i].specialness >= specialness ?
		    &nodes[i] : real_prev_filtered);
		psearch_filtered = tree_psearch_filtered(tree, &nodes[i],
		    &specialness_filter_node, &specialness_filter_subtree,
		    &specialness);
		expect_ptr_eq(real_psearch_filtered, psearch_filtered, "");

		/*
		 * search, nsearch, psearch from a node equivalent to but
		 * distinct from nodes[i].
		 */
		node_t equiv;
		equiv.magic = NODE_MAGIC;
		equiv.key = nodes[i].key;
		equiv.allow_duplicates = true;
		real_search_filtered = (nodes[i].specialness >= specialness ?
		    &nodes[i] : NULL);
		search_filtered = tree_search_filtered(tree, &equiv,
		    &specialness_filter_node, &specialness_filter_subtree,
		    &specialness);
		expect_ptr_eq(real_search_filtered, search_filtered, "");

		real_nsearch_filtered = (nodes[i].specialness >= specialness ?
		    &nodes[i] : real_next_filtered);
		nsearch_filtered = tree_nsearch_filtered(tree, &equiv,
		    &specialness_filter_node, &specialness_filter_subtree,
		    &specialness);
		expect_ptr_eq(real_nsearch_filtered, nsearch_filtered, "");

		real_psearch_filtered = (nodes[i].specialness >= specialness ?
		    &nodes[i] : real_prev_filtered);
		psearch_filtered = tree_psearch_filtered(tree, &equiv,
		    &specialness_filter_node, &specialness_filter_subtree,
		    &specialness);
		expect_ptr_eq(real_psearch_filtered, psearch_filtered, "");

		/*
		 * search, nsearch, psearch from a node after nodes[i] in the
		 * ordering.
		 */
		node_t after;
		after.magic = NODE_MAGIC;
		after.key = nodes[i].key + 1;
		after.allow_duplicates = false;
		real_search_filtered = NULL;
		search_filtered = tree_search_filtered(tree, &after,
		    &specialness_filter_node, &specialness_filter_subtree,
		    &specialness);
		expect_ptr_eq(real_search_filtered, search_filtered, "");

		real_nsearch_filtered = real_next_filtered;
		nsearch_filtered = tree_nsearch_filtered(tree, &after,
		    &specialness_filter_node, &specialness_filter_subtree,
		    &specialness);
		expect_ptr_eq(real_nsearch_filtered, nsearch_filtered, "");

		real_psearch_filtered = (nodes[i].specialness >= specialness ?
		    &nodes[i] : real_prev_filtered);
		psearch_filtered = tree_psearch_filtered(tree, &after,
		    &specialness_filter_node, &specialness_filter_subtree,
		    &specialness);
		expect_ptr_eq(real_psearch_filtered, psearch_filtered, "");
	}

	/* Filtered iteration test setup. */
	int nspecial = 0;
	node_t *sorted_nodes[UPDATE_TEST_MAX];
	node_t *sorted_filtered_nodes[UPDATE_TEST_MAX];
	for (int i = 0; i < nnodes; i++) {
		sorted_nodes[i] = &nodes[i];
	}
	qsort(sorted_nodes, nnodes, sizeof(node_t *), &qsort_node_cmp);
	for (int i = 0; i < nnodes; i++) {
		sorted_nodes[i]->rank = i;
		sorted_nodes[i]->filtered_rank = nspecial;
		if (sorted_nodes[i]->specialness >= 1) {
			sorted_filtered_nodes[nspecial] = sorted_nodes[i];
			nspecial++;
		}
	}

	node_t *iter_result;

	iter_ctx_t ctx;
	ctx.ncalls = 0;
	ctx.last_node = NULL;
	ctx.ncalls_max = INT_MAX;
	ctx.forward = true;

	/* Filtered forward iteration from the beginning. */
	iter_result = tree_iter_filtered(tree, NULL, &tree_iterate_filtered_cb,
	    &ctx, &specialness_filter_node, &specialness_filter_subtree,
	    &specialness);
	expect_ptr_null(iter_result, "");
	expect_d_eq(nspecial, ctx.ncalls, "");
	/* Filtered forward iteration from a starting point. */
	for (int i = 0; i < nnodes; i++) {
		ctx.ncalls = 0;
		ctx.last_node = NULL;
		iter_result = tree_iter_filtered(tree, &nodes[i],
		    &tree_iterate_filtered_cb, &ctx, &specialness_filter_node,
		    &specialness_filter_subtree, &specialness);
		expect_ptr_null(iter_result, "");
		expect_d_eq(nspecial - nodes[i].filtered_rank, ctx.ncalls, "");
	}
	/* Filtered forward iteration from the beginning, with stopping */
	for (int i = 0; i < nspecial; i++) {
		ctx.ncalls = 0;
		ctx.last_node = NULL;
		ctx.ncalls_max = i + 1;
		iter_result = tree_iter_filtered(tree, NULL,
		    &tree_iterate_filtered_cb, &ctx, &specialness_filter_node,
		    &specialness_filter_subtree, &specialness);
		expect_ptr_eq(sorted_filtered_nodes[i], iter_result, "");
		expect_d_eq(ctx.ncalls, i + 1, "");
	}
	/* Filtered forward iteration from a starting point, with stopping. */
	for (int i = 0; i < nnodes; i++) {
		for (int j = 0; j < nspecial - nodes[i].filtered_rank; j++) {
			ctx.ncalls = 0;
			ctx.last_node = NULL;
			ctx.ncalls_max = j + 1;
			iter_result = tree_iter_filtered(tree, &nodes[i],
			    &tree_iterate_filtered_cb, &ctx,
			    &specialness_filter_node,
			    &specialness_filter_subtree, &specialness);
			expect_d_eq(j + 1, ctx.ncalls, "");
			expect_ptr_eq(sorted_filtered_nodes[
			    nodes[i].filtered_rank + j], iter_result, "");
		}
	}

	/* Backwards iteration. */
	ctx.ncalls = 0;
	ctx.last_node = NULL;
	ctx.ncalls_max = INT_MAX;
	ctx.forward = false;

	/* Filtered backward iteration from the end. */
	iter_result = tree_reverse_iter_filtered(tree, NULL,
	    &tree_iterate_filtered_cb, &ctx, &specialness_filter_node,
	    &specialness_filter_subtree, &specialness);
	expect_ptr_null(iter_result, "");
	expect_d_eq(nspecial, ctx.ncalls, "");
	/* Filtered backward iteration from a starting point. */
	for (int i = 0; i < nnodes; i++) {
		ctx.ncalls = 0;
		ctx.last_node = NULL;
		iter_result = tree_reverse_iter_filtered(tree, &nodes[i],
		    &tree_iterate_filtered_cb, &ctx, &specialness_filter_node,
		    &specialness_filter_subtree, &specialness);
		expect_ptr_null(iter_result, "");
		int surplus_rank = (nodes[i].specialness >= 1 ? 1 : 0);
		expect_d_eq(nodes[i].filtered_rank + surplus_rank, ctx.ncalls,
		    "");
	}
	/* Filtered backward iteration from the end, with stopping */
	for (int i = 0; i < nspecial; i++) {
		ctx.ncalls = 0;
		ctx.last_node = NULL;
		ctx.ncalls_max = i + 1;
		iter_result = tree_reverse_iter_filtered(tree, NULL,
		    &tree_iterate_filtered_cb, &ctx, &specialness_filter_node,
		    &specialness_filter_subtree, &specialness);
		expect_ptr_eq(sorted_filtered_nodes[nspecial - i - 1],
		    iter_result, "");
		expect_d_eq(ctx.ncalls, i + 1, "");
	}
	/* Filtered backward iteration from a starting point, with stopping. */
	for (int i = 0; i < nnodes; i++) {
		int surplus_rank = (nodes[i].specialness >= 1 ? 1 : 0);
		for (int j = 0; j < nodes[i].filtered_rank + surplus_rank;
		    j++) {
			ctx.ncalls = 0;
			ctx.last_node = NULL;
			ctx.ncalls_max = j + 1;
			iter_result = tree_reverse_iter_filtered(tree,
			    &nodes[i], &tree_iterate_filtered_cb, &ctx,
			    &specialness_filter_node,
			    &specialness_filter_subtree, &specialness);
			expect_d_eq(j + 1, ctx.ncalls, "");
			expect_ptr_eq(sorted_filtered_nodes[
			    nodes[i].filtered_rank - j - 1 + surplus_rank],
			    iter_result, "");
		}
	}
}

static void
do_update_search_test(int nnodes, int ntrees, int nremovals,
    int nupdates) {
	node_t nodes[UPDATE_TEST_MAX];
	assert(nnodes <= UPDATE_TEST_MAX);

	sfmt_t *sfmt = init_gen_rand(12345);
	for (int i = 0; i < ntrees; i++) {
		tree_t tree;
		tree_new(&tree);
		for (int j = 0; j < nnodes; j++) {
			nodes[j].magic = NODE_MAGIC;
			/*
			 * In consistency checking, we increment or decrement a
			 * key and assume that the result is not a key in the
			 * tree.  This isn't a *real* concern with 64-bit keys
			 * and a good PRNG, but why not be correct anyways?
			 */
			nodes[j].key = 2 * gen_rand64(sfmt);
			nodes[j].specialness = 0;
			nodes[j].mid_remove = false;
			nodes[j].allow_duplicates = false;
			nodes[j].summary_lchild = NULL;
			nodes[j].summary_rchild = NULL;
			nodes[j].summary_max_specialness = 0;
			tree_insert(&tree, &nodes[j]);
		}
		for (int j = 0; j < nremovals; j++) {
			int victim = (int)gen_rand64_range(sfmt, nnodes);
			if (!nodes[victim].mid_remove) {
				tree_remove(&tree, &nodes[victim]);
				nodes[victim].mid_remove = true;
			}
		}
		for (int j = 0; j < nnodes; j++) {
			if (nodes[j].mid_remove) {
				nodes[j].mid_remove = false;
				nodes[j].key = 2 * gen_rand64(sfmt);
				tree_insert(&tree, &nodes[j]);
			}
		}
		for (int j = 0; j < nupdates; j++) {
			uint32_t ind = gen_rand32_range(sfmt, nnodes);
			nodes[ind].specialness = 1 - nodes[ind].specialness;
			tree_update_summaries(&tree, &nodes[ind]);
			check_consistency(&tree, nodes, nnodes);
		}
	}
}

TEST_BEGIN(test_rb_update_search) {
	summarize_always_returns_true = false;
	do_update_search_test(2, 100, 3, 50);
	do_update_search_test(5, 100, 3, 50);
	do_update_search_test(12, 100, 5, 1000);
	do_update_search_test(100, 1, 50, 500);
}
TEST_END

typedef rb_tree(node_t) unsummarized_tree_t;
rb_gen(static UNUSED, unsummarized_tree_, unsummarized_tree_t, node_t, link,
    node_cmp);

static node_t *
unsummarized_tree_iterate_cb(unsummarized_tree_t *tree, node_t *node,
    void *data) {
	unsigned *i = (unsigned *)data;
	(*i)++;
	return NULL;
}
/*
 * The unsummarized and summarized funtionality is implemented via the same
 * functions; we don't really need to do much more than test that we can exclude
 * the filtered functionality without anything breaking.
 */
TEST_BEGIN(test_rb_unsummarized) {
	unsummarized_tree_t tree;
	unsummarized_tree_new(&tree);
	unsigned nnodes = 0;
	unsummarized_tree_iter(&tree, NULL, &unsummarized_tree_iterate_cb,
	    &nnodes);
	expect_u_eq(0, nnodes, "");
}
TEST_END

int
main(void) {
	return test_no_reentrancy(
	    test_rb_empty,
	    test_rb_random,
	    test_rb_filter_simple,
	    test_rb_update_search,
	    test_rb_unsummarized);
}
