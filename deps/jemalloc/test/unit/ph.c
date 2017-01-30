#include "test/jemalloc_test.h"

typedef struct node_s node_t;

struct node_s {
#define	NODE_MAGIC 0x9823af7e
	uint32_t magic;
	phn(node_t) link;
	uint64_t key;
};

static int
node_cmp(const node_t *a, const node_t *b)
{
	int ret;

	ret = (a->key > b->key) - (a->key < b->key);
	if (ret == 0) {
		/*
		 * Duplicates are not allowed in the heap, so force an
		 * arbitrary ordering for non-identical items with equal keys.
		 */
		ret = (((uintptr_t)a) > ((uintptr_t)b))
		    - (((uintptr_t)a) < ((uintptr_t)b));
	}
	return (ret);
}

static int
node_cmp_magic(const node_t *a, const node_t *b) {

	assert_u32_eq(a->magic, NODE_MAGIC, "Bad magic");
	assert_u32_eq(b->magic, NODE_MAGIC, "Bad magic");

	return (node_cmp(a, b));
}

typedef ph(node_t) heap_t;
ph_gen(static, heap_, heap_t, node_t, link, node_cmp_magic);

static void
node_print(const node_t *node, unsigned depth)
{
	unsigned i;
	node_t *leftmost_child, *sibling;

	for (i = 0; i < depth; i++)
		malloc_printf("\t");
	malloc_printf("%2"FMTu64"\n", node->key);

	leftmost_child = phn_lchild_get(node_t, link, node);
	if (leftmost_child == NULL)
		return;
	node_print(leftmost_child, depth + 1);

	for (sibling = phn_next_get(node_t, link, leftmost_child); sibling !=
	    NULL; sibling = phn_next_get(node_t, link, sibling)) {
		node_print(sibling, depth + 1);
	}
}

static void
heap_print(const heap_t *heap)
{
	node_t *auxelm;

	malloc_printf("vvv heap %p vvv\n", heap);
	if (heap->ph_root == NULL)
		goto label_return;

	node_print(heap->ph_root, 0);

	for (auxelm = phn_next_get(node_t, link, heap->ph_root); auxelm != NULL;
	    auxelm = phn_next_get(node_t, link, auxelm)) {
		assert_ptr_eq(phn_next_get(node_t, link, phn_prev_get(node_t,
		    link, auxelm)), auxelm,
		    "auxelm's prev doesn't link to auxelm");
		node_print(auxelm, 0);
	}

label_return:
	malloc_printf("^^^ heap %p ^^^\n", heap);
}

static unsigned
node_validate(const node_t *node, const node_t *parent)
{
	unsigned nnodes = 1;
	node_t *leftmost_child, *sibling;

	if (parent != NULL) {
		assert_d_ge(node_cmp_magic(node, parent), 0,
		    "Child is less than parent");
	}

	leftmost_child = phn_lchild_get(node_t, link, node);
	if (leftmost_child == NULL)
		return (nnodes);
	assert_ptr_eq((void *)phn_prev_get(node_t, link, leftmost_child),
	    (void *)node, "Leftmost child does not link to node");
	nnodes += node_validate(leftmost_child, node);

	for (sibling = phn_next_get(node_t, link, leftmost_child); sibling !=
	    NULL; sibling = phn_next_get(node_t, link, sibling)) {
		assert_ptr_eq(phn_next_get(node_t, link, phn_prev_get(node_t,
		    link, sibling)), sibling,
		    "sibling's prev doesn't link to sibling");
		nnodes += node_validate(sibling, node);
	}
	return (nnodes);
}

static unsigned
heap_validate(const heap_t *heap)
{
	unsigned nnodes = 0;
	node_t *auxelm;

	if (heap->ph_root == NULL)
		goto label_return;

	nnodes += node_validate(heap->ph_root, NULL);

	for (auxelm = phn_next_get(node_t, link, heap->ph_root); auxelm != NULL;
	    auxelm = phn_next_get(node_t, link, auxelm)) {
		assert_ptr_eq(phn_next_get(node_t, link, phn_prev_get(node_t,
		    link, auxelm)), auxelm,
		    "auxelm's prev doesn't link to auxelm");
		nnodes += node_validate(auxelm, NULL);
	}

label_return:
	if (false)
		heap_print(heap);
	return (nnodes);
}

TEST_BEGIN(test_ph_empty)
{
	heap_t heap;

	heap_new(&heap);
	assert_true(heap_empty(&heap), "Heap should be empty");
	assert_ptr_null(heap_first(&heap), "Unexpected node");
}
TEST_END

static void
node_remove(heap_t *heap, node_t *node)
{

	heap_remove(heap, node);

	node->magic = 0;
}

static node_t *
node_remove_first(heap_t *heap)
{
	node_t *node = heap_remove_first(heap);
	node->magic = 0;
	return (node);
}

TEST_BEGIN(test_ph_random)
{
#define	NNODES 25
#define	NBAGS 250
#define	SEED 42
	sfmt_t *sfmt;
	uint64_t bag[NNODES];
	heap_t heap;
	node_t nodes[NNODES];
	unsigned i, j, k;

	sfmt = init_gen_rand(SEED);
	for (i = 0; i < NBAGS; i++) {
		switch (i) {
		case 0:
			/* Insert in order. */
			for (j = 0; j < NNODES; j++)
				bag[j] = j;
			break;
		case 1:
			/* Insert in reverse order. */
			for (j = 0; j < NNODES; j++)
				bag[j] = NNODES - j - 1;
			break;
		default:
			for (j = 0; j < NNODES; j++)
				bag[j] = gen_rand64_range(sfmt, NNODES);
		}

		for (j = 1; j <= NNODES; j++) {
			/* Initialize heap and nodes. */
			heap_new(&heap);
			assert_u_eq(heap_validate(&heap), 0,
			    "Incorrect node count");
			for (k = 0; k < j; k++) {
				nodes[k].magic = NODE_MAGIC;
				nodes[k].key = bag[k];
			}

			/* Insert nodes. */
			for (k = 0; k < j; k++) {
				heap_insert(&heap, &nodes[k]);
				if (i % 13 == 12) {
					/* Trigger merging. */
					assert_ptr_not_null(heap_first(&heap),
					    "Heap should not be empty");
				}
				assert_u_eq(heap_validate(&heap), k + 1,
				    "Incorrect node count");
			}

			assert_false(heap_empty(&heap),
			    "Heap should not be empty");

			/* Remove nodes. */
			switch (i % 4) {
			case 0:
				for (k = 0; k < j; k++) {
					assert_u_eq(heap_validate(&heap), j - k,
					    "Incorrect node count");
					node_remove(&heap, &nodes[k]);
					assert_u_eq(heap_validate(&heap), j - k
					    - 1, "Incorrect node count");
				}
				break;
			case 1:
				for (k = j; k > 0; k--) {
					node_remove(&heap, &nodes[k-1]);
					assert_u_eq(heap_validate(&heap), k - 1,
					    "Incorrect node count");
				}
				break;
			case 2: {
				node_t *prev = NULL;
				for (k = 0; k < j; k++) {
					node_t *node = node_remove_first(&heap);
					assert_u_eq(heap_validate(&heap), j - k
					    - 1, "Incorrect node count");
					if (prev != NULL) {
						assert_d_ge(node_cmp(node,
						    prev), 0,
						    "Bad removal order");
					}
					prev = node;
				}
				break;
			} case 3: {
				node_t *prev = NULL;
				for (k = 0; k < j; k++) {
					node_t *node = heap_first(&heap);
					assert_u_eq(heap_validate(&heap), j - k,
					    "Incorrect node count");
					if (prev != NULL) {
						assert_d_ge(node_cmp(node,
						    prev), 0,
						    "Bad removal order");
					}
					node_remove(&heap, node);
					assert_u_eq(heap_validate(&heap), j - k
					    - 1, "Incorrect node count");
					prev = node;
				}
				break;
			} default:
				not_reached();
			}

			assert_ptr_null(heap_first(&heap),
			    "Heap should be empty");
			assert_true(heap_empty(&heap), "Heap should be empty");
		}
	}
	fini_gen_rand(sfmt);
#undef NNODES
#undef SEED
}
TEST_END

int
main(void)
{

	return (test(
	    test_ph_empty,
	    test_ph_random));
}
