#include "test/jemalloc_test.h"

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

typedef struct node_s node_t;

struct node_s {
#define NODE_MAGIC 0x9823af7e
	uint32_t magic;
	rb_node(node_t) link;
	uint64_t key;
};

static int
node_cmp(const node_t *a, const node_t *b) {
	int ret;

	assert_u32_eq(a->magic, NODE_MAGIC, "Bad magic");
	assert_u32_eq(b->magic, NODE_MAGIC, "Bad magic");

	ret = (a->key > b->key) - (a->key < b->key);
	if (ret == 0) {
		/*
		 * Duplicates are not allowed in the tree, so force an
		 * arbitrary ordering for non-identical items with equal keys.
		 */
		ret = (((uintptr_t)a) > ((uintptr_t)b))
		    - (((uintptr_t)a) < ((uintptr_t)b));
	}
	return ret;
}

typedef rb_tree(node_t) tree_t;
rb_gen(static, tree_, tree_t, node_t, link, node_cmp);

TEST_BEGIN(test_rb_empty) {
	tree_t tree;
	node_t key;

	tree_new(&tree);

	assert_true(tree_empty(&tree), "Tree should be empty");
	assert_ptr_null(tree_first(&tree), "Unexpected node");
	assert_ptr_null(tree_last(&tree), "Unexpected node");

	key.key = 0;
	key.magic = NODE_MAGIC;
	assert_ptr_null(tree_search(&tree, &key), "Unexpected node");

	key.key = 0;
	key.magic = NODE_MAGIC;
	assert_ptr_null(tree_nsearch(&tree, &key), "Unexpected node");

	key.key = 0;
	key.magic = NODE_MAGIC;
	assert_ptr_null(tree_psearch(&tree, &key), "Unexpected node");
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

	if (!rbtn_red_get(node_t, link, node)) {
		black_depth++;
	}

	/* Red nodes must be interleaved with black nodes. */
	if (rbtn_red_get(node_t, link, node)) {
		if (left_node != NULL) {
			assert_false(rbtn_red_get(node_t, link, left_node),
				"Node should be black");
		}
		if (right_node != NULL) {
			assert_false(rbtn_red_get(node_t, link, right_node),
			    "Node should be black");
		}
	}

	/* Self. */
	assert_u32_eq(node->magic, NODE_MAGIC, "Bad magic");

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

static node_t *
tree_iterate_cb(tree_t *tree, node_t *node, void *data) {
	unsigned *i = (unsigned *)data;
	node_t *search_node;

	assert_u32_eq(node->magic, NODE_MAGIC, "Bad magic");

	/* Test rb_search(). */
	search_node = tree_search(tree, node);
	assert_ptr_eq(search_node, node,
	    "tree_search() returned unexpected node");

	/* Test rb_nsearch(). */
	search_node = tree_nsearch(tree, node);
	assert_ptr_eq(search_node, node,
	    "tree_nsearch() returned unexpected node");

	/* Test rb_psearch(). */
	search_node = tree_psearch(tree, node);
	assert_ptr_eq(search_node, node,
	    "tree_psearch() returned unexpected node");

	(*i)++;

	return NULL;
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
		assert_u64_ge(search_node->key, node->key,
		    "Key ordering error");
	}

	/* Test rb_psearch(). */
	search_node = tree_psearch(tree, node);
	if (search_node != NULL) {
		assert_u64_le(search_node->key, node->key,
		    "Key ordering error");
	}

	node->magic = 0;

	rbtn_black_height(node_t, link, tree, black_height);
	imbalances = tree_recurse(tree->rbt_root, black_height, 0);
	assert_u_eq(imbalances, 0, "Tree is unbalanced");
	assert_u_eq(tree_iterate(tree), nnodes-1,
	    "Unexpected node iteration count");
	assert_u_eq(tree_iterate_reverse(tree), nnodes-1,
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

	assert_u_gt(*nnodes, 0, "Destruction removed too many nodes");
	(*nnodes)--;
}

TEST_BEGIN(test_rb_random) {
#define NNODES 25
#define NBAGS 250
#define SEED 42
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

		for (j = 1; j <= NNODES; j++) {
			/* Initialize tree and nodes. */
			tree_new(&tree);
			for (k = 0; k < j; k++) {
				nodes[k].magic = NODE_MAGIC;
				nodes[k].key = bag[k];
			}

			/* Insert nodes. */
			for (k = 0; k < j; k++) {
				tree_insert(&tree, &nodes[k]);

				rbtn_black_height(node_t, link, &tree,
				    black_height);
				imbalances = tree_recurse(tree.rbt_root,
				    black_height, 0);
				assert_u_eq(imbalances, 0,
				    "Tree is unbalanced");

				assert_u_eq(tree_iterate(&tree), k+1,
				    "Unexpected node iteration count");
				assert_u_eq(tree_iterate_reverse(&tree), k+1,
				    "Unexpected node iteration count");

				assert_false(tree_empty(&tree),
				    "Tree should not be empty");
				assert_ptr_not_null(tree_first(&tree),
				    "Tree should not be empty");
				assert_ptr_not_null(tree_last(&tree),
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
				assert_u_eq(nnodes, 0,
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
				assert_u_eq(nnodes, 0,
				    "Removal terminated early");
				break;
			} case 4: {
				unsigned nnodes = j;
				tree_destroy(&tree, destroy_cb, &nnodes);
				assert_u_eq(nnodes, 0,
				    "Destruction terminated early");
				break;
			} default:
				not_reached();
			}
		}
	}
	fini_gen_rand(sfmt);
#undef NNODES
#undef NBAGS
#undef SEED
}
TEST_END

int
main(void) {
	return test(
	    test_rb_empty,
	    test_rb_random);
}
