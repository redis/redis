#ifndef JEMALLOC_INTERNAL_RB_H
#define JEMALLOC_INTERNAL_RB_H

/*-
 *******************************************************************************
 *
 * cpp macro implementation of left-leaning 2-3 red-black trees.  Parent
 * pointers are not used, and color bits are stored in the least significant
 * bit of right-child pointers (if RB_COMPACT is defined), thus making node
 * linkage as compact as is possible for red-black trees.
 *
 * Usage:
 *
 *   #include <stdint.h>
 *   #include <stdbool.h>
 *   #define NDEBUG // (Optional, see assert(3).)
 *   #include <assert.h>
 *   #define RB_COMPACT // (Optional, embed color bits in right-child pointers.)
 *   #include <rb.h>
 *   ...
 *
 *******************************************************************************
 */

#ifndef __PGI
#define RB_COMPACT
#endif

/*
 * Each node in the RB tree consumes at least 1 byte of space (for the linkage
 * if nothing else, so there are a maximum of sizeof(void *) << 3 rb tree nodes
 * in any process (and thus, at most sizeof(void *) << 3 nodes in any rb tree).
 * The choice of algorithm bounds the depth of a tree to twice the binary log of
 * the number of elements in the tree; the following bound follows.
 */
#define RB_MAX_DEPTH (sizeof(void *) << 4)

#ifdef RB_COMPACT
/* Node structure. */
#define rb_node(a_type)							\
struct {								\
    a_type *rbn_left;							\
    a_type *rbn_right_red;						\
}
#else
#define rb_node(a_type)							\
struct {								\
    a_type *rbn_left;							\
    a_type *rbn_right;							\
    bool rbn_red;							\
}
#endif

/* Root structure. */
#define rb_tree(a_type)							\
struct {								\
    a_type *rbt_root;							\
}

/* Left accessors. */
#define rbtn_left_get(a_type, a_field, a_node)				\
    ((a_node)->a_field.rbn_left)
#define rbtn_left_set(a_type, a_field, a_node, a_left) do {		\
    (a_node)->a_field.rbn_left = a_left;				\
} while (0)

#ifdef RB_COMPACT
/* Right accessors. */
#define rbtn_right_get(a_type, a_field, a_node)				\
    ((a_type *) (((intptr_t) (a_node)->a_field.rbn_right_red)		\
      & ((ssize_t)-2)))
#define rbtn_right_set(a_type, a_field, a_node, a_right) do {		\
    (a_node)->a_field.rbn_right_red = (a_type *) (((uintptr_t) a_right)	\
      | (((uintptr_t) (a_node)->a_field.rbn_right_red) & ((size_t)1)));	\
} while (0)

/* Color accessors. */
#define rbtn_red_get(a_type, a_field, a_node)				\
    ((bool) (((uintptr_t) (a_node)->a_field.rbn_right_red)		\
      & ((size_t)1)))
#define rbtn_color_set(a_type, a_field, a_node, a_red) do {		\
    (a_node)->a_field.rbn_right_red = (a_type *) ((((intptr_t)		\
      (a_node)->a_field.rbn_right_red) & ((ssize_t)-2))			\
      | ((ssize_t)a_red));						\
} while (0)
#define rbtn_red_set(a_type, a_field, a_node) do {			\
    (a_node)->a_field.rbn_right_red = (a_type *) (((uintptr_t)		\
      (a_node)->a_field.rbn_right_red) | ((size_t)1));			\
} while (0)
#define rbtn_black_set(a_type, a_field, a_node) do {			\
    (a_node)->a_field.rbn_right_red = (a_type *) (((intptr_t)		\
      (a_node)->a_field.rbn_right_red) & ((ssize_t)-2));		\
} while (0)

/* Node initializer. */
#define rbt_node_new(a_type, a_field, a_rbt, a_node) do {		\
    /* Bookkeeping bit cannot be used by node pointer. */		\
    assert(((uintptr_t)(a_node) & 0x1) == 0);				\
    rbtn_left_set(a_type, a_field, (a_node), NULL);	\
    rbtn_right_set(a_type, a_field, (a_node), NULL);	\
    rbtn_red_set(a_type, a_field, (a_node));				\
} while (0)
#else
/* Right accessors. */
#define rbtn_right_get(a_type, a_field, a_node)				\
    ((a_node)->a_field.rbn_right)
#define rbtn_right_set(a_type, a_field, a_node, a_right) do {		\
    (a_node)->a_field.rbn_right = a_right;				\
} while (0)

/* Color accessors. */
#define rbtn_red_get(a_type, a_field, a_node)				\
    ((a_node)->a_field.rbn_red)
#define rbtn_color_set(a_type, a_field, a_node, a_red) do {		\
    (a_node)->a_field.rbn_red = (a_red);				\
} while (0)
#define rbtn_red_set(a_type, a_field, a_node) do {			\
    (a_node)->a_field.rbn_red = true;					\
} while (0)
#define rbtn_black_set(a_type, a_field, a_node) do {			\
    (a_node)->a_field.rbn_red = false;					\
} while (0)

/* Node initializer. */
#define rbt_node_new(a_type, a_field, a_rbt, a_node) do {		\
    rbtn_left_set(a_type, a_field, (a_node), NULL);	\
    rbtn_right_set(a_type, a_field, (a_node), NULL);	\
    rbtn_red_set(a_type, a_field, (a_node));				\
} while (0)
#endif

/* Tree initializer. */
#define rb_new(a_type, a_field, a_rbt) do {				\
    (a_rbt)->rbt_root = NULL;						\
} while (0)

/* Internal utility macros. */
#define rbtn_first(a_type, a_field, a_rbt, a_root, r_node) do {		\
    (r_node) = (a_root);						\
    if ((r_node) != NULL) {						\
	for (;								\
	  rbtn_left_get(a_type, a_field, (r_node)) != NULL;		\
	  (r_node) = rbtn_left_get(a_type, a_field, (r_node))) {	\
	}								\
    }									\
} while (0)

#define rbtn_last(a_type, a_field, a_rbt, a_root, r_node) do {		\
    (r_node) = (a_root);						\
    if ((r_node) != NULL) {						\
	for (; rbtn_right_get(a_type, a_field, (r_node)) != NULL;	\
	  (r_node) = rbtn_right_get(a_type, a_field, (r_node))) {	\
	}								\
    }									\
} while (0)

#define rbtn_rotate_left(a_type, a_field, a_node, r_node) do {		\
    (r_node) = rbtn_right_get(a_type, a_field, (a_node));		\
    rbtn_right_set(a_type, a_field, (a_node),				\
      rbtn_left_get(a_type, a_field, (r_node)));			\
    rbtn_left_set(a_type, a_field, (r_node), (a_node));			\
} while (0)

#define rbtn_rotate_right(a_type, a_field, a_node, r_node) do {		\
    (r_node) = rbtn_left_get(a_type, a_field, (a_node));		\
    rbtn_left_set(a_type, a_field, (a_node),				\
      rbtn_right_get(a_type, a_field, (r_node)));			\
    rbtn_right_set(a_type, a_field, (r_node), (a_node));		\
} while (0)

#define rb_summarized_only_false(...)
#define rb_summarized_only_true(...) __VA_ARGS__
#define rb_empty_summarize(a_node, a_lchild, a_rchild) false

/*
 * The rb_proto() and rb_summarized_proto() macros generate function prototypes
 * that correspond to the functions generated by an equivalently parameterized
 * call to rb_gen() or rb_summarized_gen(), respectively.
 */

#define rb_proto(a_attr, a_prefix, a_rbt_type, a_type)			\
    rb_proto_impl(a_attr, a_prefix, a_rbt_type, a_type, false)
#define rb_summarized_proto(a_attr, a_prefix, a_rbt_type, a_type)	\
    rb_proto_impl(a_attr, a_prefix, a_rbt_type, a_type, true)
#define rb_proto_impl(a_attr, a_prefix, a_rbt_type, a_type,		\
    a_is_summarized)							\
a_attr void								\
a_prefix##new(a_rbt_type *rbtree);					\
a_attr bool								\
a_prefix##empty(a_rbt_type *rbtree);					\
a_attr a_type *								\
a_prefix##first(a_rbt_type *rbtree);					\
a_attr a_type *								\
a_prefix##last(a_rbt_type *rbtree);					\
a_attr a_type *								\
a_prefix##next(a_rbt_type *rbtree, a_type *node);			\
a_attr a_type *								\
a_prefix##prev(a_rbt_type *rbtree, a_type *node);			\
a_attr a_type *								\
a_prefix##search(a_rbt_type *rbtree, const a_type *key);		\
a_attr a_type *								\
a_prefix##nsearch(a_rbt_type *rbtree, const a_type *key);		\
a_attr a_type *								\
a_prefix##psearch(a_rbt_type *rbtree, const a_type *key);		\
a_attr void								\
a_prefix##insert(a_rbt_type *rbtree, a_type *node);			\
a_attr void								\
a_prefix##remove(a_rbt_type *rbtree, a_type *node);			\
a_attr a_type *								\
a_prefix##iter(a_rbt_type *rbtree, a_type *start, a_type *(*cb)(	\
  a_rbt_type *, a_type *, void *), void *arg);				\
a_attr a_type *								\
a_prefix##reverse_iter(a_rbt_type *rbtree, a_type *start,		\
  a_type *(*cb)(a_rbt_type *, a_type *, void *), void *arg);		\
a_attr void								\
a_prefix##destroy(a_rbt_type *rbtree, void (*cb)(a_type *, void *),	\
  void *arg);								\
/* Extended API */							\
rb_summarized_only_##a_is_summarized(					\
a_attr void								\
a_prefix##update_summaries(a_rbt_type *rbtree, a_type *node);		\
a_attr bool								\
a_prefix##empty_filtered(a_rbt_type *rbtree,				\
    bool (*filter_node)(void *, a_type *),				\
    bool (*filter_subtree)(void *, a_type *),				\
    void *filter_ctx);							\
a_attr a_type *								\
a_prefix##first_filtered(a_rbt_type *rbtree,				\
    bool (*filter_node)(void *, a_type *),				\
    bool (*filter_subtree)(void *, a_type *),				\
    void *filter_ctx);							\
a_attr a_type *								\
a_prefix##last_filtered(a_rbt_type *rbtree,				\
    bool (*filter_node)(void *, a_type *),				\
    bool (*filter_subtree)(void *, a_type *),				\
    void *filter_ctx);							\
a_attr a_type *								\
a_prefix##next_filtered(a_rbt_type *rbtree, a_type *node,		\
    bool (*filter_node)(void *, a_type *),				\
    bool (*filter_subtree)(void *, a_type *),				\
    void *filter_ctx);							\
a_attr a_type *								\
a_prefix##prev_filtered(a_rbt_type *rbtree, a_type *node,		\
    bool (*filter_node)(void *, a_type *),				\
    bool (*filter_subtree)(void *, a_type *),				\
    void *filter_ctx);							\
a_attr a_type *								\
a_prefix##search_filtered(a_rbt_type *rbtree, const a_type *key,	\
    bool (*filter_node)(void *, a_type *),				\
    bool (*filter_subtree)(void *, a_type *),				\
    void *filter_ctx);							\
a_attr a_type *								\
a_prefix##nsearch_filtered(a_rbt_type *rbtree, const a_type *key,	\
    bool (*filter_node)(void *, a_type *),				\
    bool (*filter_subtree)(void *, a_type *),				\
    void *filter_ctx);							\
a_attr a_type *								\
a_prefix##psearch_filtered(a_rbt_type *rbtree, const a_type *key,	\
    bool (*filter_node)(void *, a_type *),				\
    bool (*filter_subtree)(void *, a_type *),				\
    void *filter_ctx);							\
a_attr a_type *								\
a_prefix##iter_filtered(a_rbt_type *rbtree, a_type *start,		\
    a_type *(*cb)(a_rbt_type *, a_type *, void *), void *arg,		\
    bool (*filter_node)(void *, a_type *),				\
    bool (*filter_subtree)(void *, a_type *),				\
    void *filter_ctx);							\
a_attr a_type *								\
a_prefix##reverse_iter_filtered(a_rbt_type *rbtree, a_type *start,	\
  a_type *(*cb)(a_rbt_type *, a_type *, void *), void *arg,		\
    bool (*filter_node)(void *, a_type *),				\
    bool (*filter_subtree)(void *, a_type *),				\
    void *filter_ctx);							\
)

/*
 * The rb_gen() macro generates a type-specific red-black tree implementation,
 * based on the above cpp macros.
 * Arguments:
 *
 *   a_attr:
 *     Function attribute for generated functions (ex: static).
 *   a_prefix:
 *     Prefix for generated functions (ex: ex_).
 *   a_rb_type:
 *     Type for red-black tree data structure (ex: ex_t).
 *   a_type:
 *     Type for red-black tree node data structure (ex: ex_node_t).
 *   a_field:
 *     Name of red-black tree node linkage (ex: ex_link).
 *   a_cmp:
 *     Node comparison function name, with the following prototype:
 *
 *     int a_cmp(a_type *a_node, a_type *a_other);
 *                        ^^^^^^
 *                        or a_key
 *     Interpretation of comparison function return values:
 *       -1 : a_node <  a_other
 *        0 : a_node == a_other
 *        1 : a_node >  a_other
 *     In all cases, the a_node or a_key macro argument is the first argument to
 *     the comparison function, which makes it possible to write comparison
 *     functions that treat the first argument specially.  a_cmp must be a total
 *     order on values inserted into the tree -- duplicates are not allowed.
 *
 * Assuming the following setup:
 *
 *   typedef struct ex_node_s ex_node_t;
 *   struct ex_node_s {
 *       rb_node(ex_node_t) ex_link;
 *   };
 *   typedef rb_tree(ex_node_t) ex_t;
 *   rb_gen(static, ex_, ex_t, ex_node_t, ex_link, ex_cmp)
 *
 * The following API is generated:
 *
 *   static void
 *   ex_new(ex_t *tree);
 *       Description: Initialize a red-black tree structure.
 *       Args:
 *         tree: Pointer to an uninitialized red-black tree object.
 *
 *   static bool
 *   ex_empty(ex_t *tree);
 *       Description: Determine whether tree is empty.
 *       Args:
 *         tree: Pointer to an initialized red-black tree object.
 *       Ret: True if tree is empty, false otherwise.
 *
 *   static ex_node_t *
 *   ex_first(ex_t *tree);
 *   static ex_node_t *
 *   ex_last(ex_t *tree);
 *       Description: Get the first/last node in tree.
 *       Args:
 *         tree: Pointer to an initialized red-black tree object.
 *       Ret: First/last node in tree, or NULL if tree is empty.
 *
 *   static ex_node_t *
 *   ex_next(ex_t *tree, ex_node_t *node);
 *   static ex_node_t *
 *   ex_prev(ex_t *tree, ex_node_t *node);
 *       Description: Get node's successor/predecessor.
 *       Args:
 *         tree: Pointer to an initialized red-black tree object.
 *         node: A node in tree.
 *       Ret: node's successor/predecessor in tree, or NULL if node is
 *            last/first.
 *
 *   static ex_node_t *
 *   ex_search(ex_t *tree, const ex_node_t *key);
 *       Description: Search for node that matches key.
 *       Args:
 *         tree: Pointer to an initialized red-black tree object.
 *         key : Search key.
 *       Ret: Node in tree that matches key, or NULL if no match.
 *
 *   static ex_node_t *
 *   ex_nsearch(ex_t *tree, const ex_node_t *key);
 *   static ex_node_t *
 *   ex_psearch(ex_t *tree, const ex_node_t *key);
 *       Description: Search for node that matches key.  If no match is found,
 *                    return what would be key's successor/predecessor, were
 *                    key in tree.
 *       Args:
 *         tree: Pointer to an initialized red-black tree object.
 *         key : Search key.
 *       Ret: Node in tree that matches key, or if no match, hypothetical node's
 *            successor/predecessor (NULL if no successor/predecessor).
 *
 *   static void
 *   ex_insert(ex_t *tree, ex_node_t *node);
 *       Description: Insert node into tree.
 *       Args:
 *         tree: Pointer to an initialized red-black tree object.
 *         node: Node to be inserted into tree.
 *
 *   static void
 *   ex_remove(ex_t *tree, ex_node_t *node);
 *       Description: Remove node from tree.
 *       Args:
 *         tree: Pointer to an initialized red-black tree object.
 *         node: Node in tree to be removed.
 *
 *   static ex_node_t *
 *   ex_iter(ex_t *tree, ex_node_t *start, ex_node_t *(*cb)(ex_t *,
 *     ex_node_t *, void *), void *arg);
 *   static ex_node_t *
 *   ex_reverse_iter(ex_t *tree, ex_node_t *start, ex_node *(*cb)(ex_t *,
 *     ex_node_t *, void *), void *arg);
 *       Description: Iterate forward/backward over tree, starting at node.  If
 *                    tree is modified, iteration must be immediately
 *                    terminated by the callback function that causes the
 *                    modification.
 *       Args:
 *         tree : Pointer to an initialized red-black tree object.
 *         start: Node at which to start iteration, or NULL to start at
 *                first/last node.
 *         cb   : Callback function, which is called for each node during
 *                iteration.  Under normal circumstances the callback function
 *                should return NULL, which causes iteration to continue.  If a
 *                callback function returns non-NULL, iteration is immediately
 *                terminated and the non-NULL return value is returned by the
 *                iterator.  This is useful for re-starting iteration after
 *                modifying tree.
 *         arg  : Opaque pointer passed to cb().
 *       Ret: NULL if iteration completed, or the non-NULL callback return value
 *            that caused termination of the iteration.
 *
 *   static void
 *   ex_destroy(ex_t *tree, void (*cb)(ex_node_t *, void *), void *arg);
 *       Description: Iterate over the tree with post-order traversal, remove
 *                    each node, and run the callback if non-null.  This is
 *                    used for destroying a tree without paying the cost to
 *                    rebalance it.  The tree must not be otherwise altered
 *                    during traversal.
 *       Args:
 *         tree: Pointer to an initialized red-black tree object.
 *         cb  : Callback function, which, if non-null, is called for each node
 *               during iteration.  There is no way to stop iteration once it
 *               has begun.
 *         arg : Opaque pointer passed to cb().
 *
 * The rb_summarized_gen() macro generates all the functions above, but has an
 * expanded interface.  In introduces the notion of summarizing subtrees, and of
 * filtering searches in the tree according to the information contained in
 * those summaries.
 * The extra macro argument is:
 *   a_summarize:
 *     Tree summarization function name, with the following prototype:
 *
 *     bool a_summarize(a_type *a_node, const a_type *a_left_child,
 *         const a_type *a_right_child);
 *
 *     This function should update a_node with the summary of the subtree rooted
 *     there, using the data contained in it and the summaries in a_left_child
 *     and a_right_child.  One or both of them may be NULL.  When the tree
 *     changes due to an insertion or removal, it updates the summaries of all
 *     nodes whose subtrees have changed (always updating the summaries of
 *     children before their parents).  If the user alters a node in the tree in
 *     a way that may change its summary, they can call the generated
 *     update_summaries function to bubble up the summary changes to the root.
 *     It should return true if the summary changed (or may have changed), and
 *     false if it didn't (which will allow the implementation to terminate
 *     "bubbling up" the summaries early).
 *     As the parameter names indicate, the children are ordered as they are in
 *     the tree, a_left_child, if it is not NULL, compares less than a_node,
 *     which in turn compares less than a_right_child (if a_right_child is not
 *     NULL).
 *
 * Using the same setup as above but replacing the macro with
 *   rb_summarized_gen(static, ex_, ex_t, ex_node_t, ex_link, ex_cmp,
 *       ex_summarize)
 *
 * Generates all the previous functions, but adds some more:
 *
 *   static void
 *   ex_update_summaries(ex_t *tree, ex_node_t *node);
 *       Description: Recompute all summaries of ancestors of node.
 *       Args:
 *         tree: Pointer to an initialized red-black tree object.
 *         node: The element of the tree whose summary may have changed.
 *
 * For each of ex_empty, ex_first, ex_last, ex_next, ex_prev, ex_search,
 * ex_nsearch, ex_psearch, ex_iter, and ex_reverse_iter, an additional function
 * is generated as well, with the suffix _filtered (e.g. ex_empty_filtered,
 * ex_first_filtered, etc.).  These use the concept of a "filter"; a binary
 * property some node either satisfies or does not satisfy.  Clever use of the
 * a_summary argument to rb_summarized_gen can allow efficient computation of
 * these predicates across whole subtrees of the tree.
 * The extended API functions accept three additional arguments after the
 * arguments to the corresponding non-extended equivalent.
 *
 * ex_fn(..., bool (*filter_node)(void *, ex_node_t *),
 *     bool (*filter_subtree)(void *, ex_node_t *), void *filter_ctx);
 *         filter_node    : Returns true if the node passes the filter.
 *         filter_subtree : Returns true if some node in the subtree rooted at
 *                          node passes the filter.
 *         filter_ctx     : A context argument passed to the filters.
 *
 * For a more concrete example of summarizing and filtering, suppose we're using
 * the red-black tree to track a set of integers:
 *
 * struct ex_node_s {
 *     rb_node(ex_node_t) ex_link;
 *     unsigned data;
 * };
 *
 * Suppose, for some application-specific reason, we want to be able to quickly
 * find numbers in the set which are divisible by large powers of 2 (say, for
 * aligned allocation purposes).  We augment the node with a summary field:
 *
 * struct ex_node_s {
 *     rb_node(ex_node_t) ex_link;
 *     unsigned data;
 *     unsigned max_subtree_ffs;
 * }
 *
 * and define our summarization function as follows:
 *
 * bool
 * ex_summarize(ex_node_t *node, const ex_node_t *lchild,
 *   const ex_node_t *rchild) {
 *     unsigned new_max_subtree_ffs = ffs(node->data);
 *     if (lchild != NULL && lchild->max_subtree_ffs > new_max_subtree_ffs) {
 *         new_max_subtree_ffs = lchild->max_subtree_ffs;
 *     }
 *     if (rchild != NULL && rchild->max_subtree_ffs > new_max_subtree_ffs) {
 *         new_max_subtree_ffs = rchild->max_subtree_ffs;
 *     }
 *     bool changed = (node->max_subtree_ffs != new_max_subtree_ffs)
 *     node->max_subtree_ffs = new_max_subtree_ffs;
 *     // This could be "return true" without any correctness or big-O
 *     // performance changes; but practically, precisely reporting summary
 *     // changes reduces the amount of work that has to be done when "bubbling
 *     // up" summary changes.
 *     return changed;
 * }
 *
 * We can now implement our filter functions as follows:
 * bool
 * ex_filter_node(void *filter_ctx, ex_node_t *node) {
 *     unsigned required_ffs = *(unsigned *)filter_ctx;
 *     return ffs(node->data) >= required_ffs;
 * }
 * bool
 * ex_filter_subtree(void *filter_ctx, ex_node_t *node) {
 *     unsigned required_ffs = *(unsigned *)filter_ctx;
 *     return node->max_subtree_ffs >= required_ffs;
 * }
 *
 * We can now easily search for, e.g., the smallest integer in the set that's
 * divisible by 128:
 * ex_node_t *
 * find_div_128(ex_tree_t *tree) {
 *     unsigned min_ffs = 7;
 *     return ex_first_filtered(tree, &ex_filter_node, &ex_filter_subtree,
 *         &min_ffs);
 * }
 *
 * We could with similar ease:
 * - Fnd the next multiple of 128 in the set that's larger than 12345 (with
 *   ex_nsearch_filtered)
 * - Iterate over just those multiples of 64 that are in the set (with
 *   ex_iter_filtered)
 * - Determine if the set contains any multiples of 1024 (with
 *   ex_empty_filtered).
 *
 * Some possibly subtle API notes:
 * - The node argument to ex_next_filtered and ex_prev_filtered need not pass
 *   the filter; it will find the next/prev node that passes the filter.
 * - ex_search_filtered will fail even for a node in the tree, if that node does
 *   not pass the filter.  ex_psearch_filtered and ex_nsearch_filtered behave
 *   similarly; they may return a node larger/smaller than the key, even if a
 *   node equivalent to the key is in the tree (but does not pass the filter).
 * - Similarly, if the start argument to a filtered iteration function does not
 *   pass the filter, the callback won't be invoked on it.
 *
 * These should make sense after a moment's reflection; each post-condition is
 * the same as with the unfiltered version, with the added constraint that the
 * returned node must pass the filter.
 */
#define rb_gen(a_attr, a_prefix, a_rbt_type, a_type, a_field, a_cmp)	\
    rb_gen_impl(a_attr, a_prefix, a_rbt_type, a_type, a_field, a_cmp,	\
	rb_empty_summarize, false)
#define rb_summarized_gen(a_attr, a_prefix, a_rbt_type, a_type,		\
    a_field, a_cmp, a_summarize)					\
    rb_gen_impl(a_attr, a_prefix, a_rbt_type, a_type, a_field, a_cmp,	\
	a_summarize, true)

#define rb_gen_impl(a_attr, a_prefix, a_rbt_type, a_type,		\
    a_field, a_cmp, a_summarize, a_is_summarized)			\
typedef struct {							\
    a_type *node;							\
    int cmp;								\
} a_prefix##path_entry_t;						\
static inline void							\
a_prefix##summarize_range(a_prefix##path_entry_t *rfirst,		\
    a_prefix##path_entry_t *rlast) {					\
    while ((uintptr_t)rlast >= (uintptr_t)rfirst) {			\
	a_type *node = rlast->node;					\
	/* Avoid a warning when a_summarize is rb_empty_summarize. */	\
	(void)node;							\
	bool changed = a_summarize(node, rbtn_left_get(a_type, a_field,	\
	    node), rbtn_right_get(a_type, a_field, node));		\
	if (!changed) {							\
		break;							\
	}								\
	rlast--;							\
    }									\
}									\
/* On the remove pathways, we sometimes swap the node being removed   */\
/* and its first successor; in such cases we need to do two range     */\
/* updates; one from the node to its (former) swapped successor, the  */\
/* next from that successor to the root (with either allowed to       */\
/* bail out early if appropriate.                                     */\
static inline void							\
a_prefix##summarize_swapped_range(a_prefix##path_entry_t *rfirst,	\
    a_prefix##path_entry_t *rlast, a_prefix##path_entry_t *swap_loc) {	\
	if (swap_loc == NULL || rlast <= swap_loc) {			\
		a_prefix##summarize_range(rfirst, rlast);		\
	} else {							\
		a_prefix##summarize_range(swap_loc + 1, rlast);		\
		(void)a_summarize(swap_loc->node,			\
		    rbtn_left_get(a_type, a_field, swap_loc->node),	\
		    rbtn_right_get(a_type, a_field, swap_loc->node));	\
		a_prefix##summarize_range(rfirst, swap_loc - 1);	\
	}								\
}									\
a_attr void								\
a_prefix##new(a_rbt_type *rbtree) {					\
    rb_new(a_type, a_field, rbtree);					\
}									\
a_attr bool								\
a_prefix##empty(a_rbt_type *rbtree) {					\
    return (rbtree->rbt_root == NULL);					\
}									\
a_attr a_type *								\
a_prefix##first(a_rbt_type *rbtree) {					\
    a_type *ret;							\
    rbtn_first(a_type, a_field, rbtree, rbtree->rbt_root, ret);		\
    return ret;								\
}									\
a_attr a_type *								\
a_prefix##last(a_rbt_type *rbtree) {					\
    a_type *ret;							\
    rbtn_last(a_type, a_field, rbtree, rbtree->rbt_root, ret);		\
    return ret;								\
}									\
a_attr a_type *								\
a_prefix##next(a_rbt_type *rbtree, a_type *node) {			\
    a_type *ret;							\
    if (rbtn_right_get(a_type, a_field, node) != NULL) {		\
	rbtn_first(a_type, a_field, rbtree, rbtn_right_get(a_type,	\
	  a_field, node), ret);						\
    } else {								\
	a_type *tnode = rbtree->rbt_root;				\
	assert(tnode != NULL);						\
	ret = NULL;							\
	while (true) {							\
	    int cmp = (a_cmp)(node, tnode);				\
	    if (cmp < 0) {						\
		ret = tnode;						\
		tnode = rbtn_left_get(a_type, a_field, tnode);		\
	    } else if (cmp > 0) {					\
		tnode = rbtn_right_get(a_type, a_field, tnode);		\
	    } else {							\
		break;							\
	    }								\
	    assert(tnode != NULL);					\
	}								\
    }									\
    return ret;								\
}									\
a_attr a_type *								\
a_prefix##prev(a_rbt_type *rbtree, a_type *node) {			\
    a_type *ret;							\
    if (rbtn_left_get(a_type, a_field, node) != NULL) {			\
	rbtn_last(a_type, a_field, rbtree, rbtn_left_get(a_type,	\
	  a_field, node), ret);						\
    } else {								\
	a_type *tnode = rbtree->rbt_root;				\
	assert(tnode != NULL);						\
	ret = NULL;							\
	while (true) {							\
	    int cmp = (a_cmp)(node, tnode);				\
	    if (cmp < 0) {						\
		tnode = rbtn_left_get(a_type, a_field, tnode);		\
	    } else if (cmp > 0) {					\
		ret = tnode;						\
		tnode = rbtn_right_get(a_type, a_field, tnode);		\
	    } else {							\
		break;							\
	    }								\
	    assert(tnode != NULL);					\
	}								\
    }									\
    return ret;								\
}									\
a_attr a_type *								\
a_prefix##search(a_rbt_type *rbtree, const a_type *key) {		\
    a_type *ret;							\
    int cmp;								\
    ret = rbtree->rbt_root;						\
    while (ret != NULL							\
      && (cmp = (a_cmp)(key, ret)) != 0) {				\
	if (cmp < 0) {							\
	    ret = rbtn_left_get(a_type, a_field, ret);			\
	} else {							\
	    ret = rbtn_right_get(a_type, a_field, ret);			\
	}								\
    }									\
    return ret;								\
}									\
a_attr a_type *								\
a_prefix##nsearch(a_rbt_type *rbtree, const a_type *key) {		\
    a_type *ret;							\
    a_type *tnode = rbtree->rbt_root;					\
    ret = NULL;								\
    while (tnode != NULL) {						\
	int cmp = (a_cmp)(key, tnode);					\
	if (cmp < 0) {							\
	    ret = tnode;						\
	    tnode = rbtn_left_get(a_type, a_field, tnode);		\
	} else if (cmp > 0) {						\
	    tnode = rbtn_right_get(a_type, a_field, tnode);		\
	} else {							\
	    ret = tnode;						\
	    break;							\
	}								\
    }									\
    return ret;								\
}									\
a_attr a_type *								\
a_prefix##psearch(a_rbt_type *rbtree, const a_type *key) {		\
    a_type *ret;							\
    a_type *tnode = rbtree->rbt_root;					\
    ret = NULL;								\
    while (tnode != NULL) {						\
	int cmp = (a_cmp)(key, tnode);					\
	if (cmp < 0) {							\
	    tnode = rbtn_left_get(a_type, a_field, tnode);		\
	} else if (cmp > 0) {						\
	    ret = tnode;						\
	    tnode = rbtn_right_get(a_type, a_field, tnode);		\
	} else {							\
	    ret = tnode;						\
	    break;							\
	}								\
    }									\
    return ret;								\
}									\
a_attr void								\
a_prefix##insert(a_rbt_type *rbtree, a_type *node) {			\
    a_prefix##path_entry_t path[RB_MAX_DEPTH];			\
    a_prefix##path_entry_t *pathp;					\
    rbt_node_new(a_type, a_field, rbtree, node);			\
    /* Wind. */								\
    path->node = rbtree->rbt_root;					\
    for (pathp = path; pathp->node != NULL; pathp++) {			\
	int cmp = pathp->cmp = a_cmp(node, pathp->node);		\
	assert(cmp != 0);						\
	if (cmp < 0) {							\
	    pathp[1].node = rbtn_left_get(a_type, a_field,		\
	      pathp->node);						\
	} else {							\
	    pathp[1].node = rbtn_right_get(a_type, a_field,		\
	      pathp->node);						\
	}								\
    }									\
    pathp->node = node;							\
    /* A loop invariant we maintain is that all nodes with            */\
    /* out-of-date summaries live in path[0], path[1], ..., *pathp.   */\
    /* To maintain this, we have to summarize node, since we          */\
    /* decrement pathp before the first iteration.                    */\
    assert(rbtn_left_get(a_type, a_field, node) == NULL);		\
    assert(rbtn_right_get(a_type, a_field, node) == NULL);		\
    (void)a_summarize(node, NULL, NULL);				\
    /* Unwind. */							\
    for (pathp--; (uintptr_t)pathp >= (uintptr_t)path; pathp--) {	\
	a_type *cnode = pathp->node;					\
	if (pathp->cmp < 0) {						\
	    a_type *left = pathp[1].node;				\
	    rbtn_left_set(a_type, a_field, cnode, left);		\
	    if (rbtn_red_get(a_type, a_field, left)) {			\
		a_type *leftleft = rbtn_left_get(a_type, a_field, left);\
		if (leftleft != NULL && rbtn_red_get(a_type, a_field,	\
		  leftleft)) {						\
		    /* Fix up 4-node. */				\
		    a_type *tnode;					\
		    rbtn_black_set(a_type, a_field, leftleft);		\
		    rbtn_rotate_right(a_type, a_field, cnode, tnode);	\
		    (void)a_summarize(cnode,				\
			rbtn_left_get(a_type, a_field, cnode),		\
			rbtn_right_get(a_type, a_field, cnode));	\
		    cnode = tnode;					\
		}							\
	    } else {							\
		a_prefix##summarize_range(path, pathp);			\
		return;							\
	    }								\
	} else {							\
	    a_type *right = pathp[1].node;				\
	    rbtn_right_set(a_type, a_field, cnode, right);		\
	    if (rbtn_red_get(a_type, a_field, right)) {			\
		a_type *left = rbtn_left_get(a_type, a_field, cnode);	\
		if (left != NULL && rbtn_red_get(a_type, a_field,	\
		  left)) {						\
		    /* Split 4-node. */					\
		    rbtn_black_set(a_type, a_field, left);		\
		    rbtn_black_set(a_type, a_field, right);		\
		    rbtn_red_set(a_type, a_field, cnode);		\
		} else {						\
		    /* Lean left. */					\
		    a_type *tnode;					\
		    bool tred = rbtn_red_get(a_type, a_field, cnode);	\
		    rbtn_rotate_left(a_type, a_field, cnode, tnode);	\
		    rbtn_color_set(a_type, a_field, tnode, tred);	\
		    rbtn_red_set(a_type, a_field, cnode);		\
		    (void)a_summarize(cnode,				\
			rbtn_left_get(a_type, a_field, cnode),		\
			rbtn_right_get(a_type, a_field, cnode));	\
		    cnode = tnode;					\
		}							\
	    } else {							\
		a_prefix##summarize_range(path, pathp);			\
		return;							\
	    }								\
	}								\
	pathp->node = cnode;						\
	(void)a_summarize(cnode,					\
	    rbtn_left_get(a_type, a_field, cnode),			\
	    rbtn_right_get(a_type, a_field, cnode));			\
    }									\
    /* Set root, and make it black. */					\
    rbtree->rbt_root = path->node;					\
    rbtn_black_set(a_type, a_field, rbtree->rbt_root);			\
}									\
a_attr void								\
a_prefix##remove(a_rbt_type *rbtree, a_type *node) {			\
    a_prefix##path_entry_t path[RB_MAX_DEPTH];				\
    a_prefix##path_entry_t *pathp;					\
    a_prefix##path_entry_t *nodep;					\
    a_prefix##path_entry_t *swap_loc;					\
    /* This is a "real" sentinel -- NULL means we didn't swap the     */\
    /* node to be pruned with one of its successors, and so           */\
    /* summarization can terminate early whenever some summary        */\
    /* doesn't change.                                                */\
    swap_loc = NULL;							\
    /* This is just to silence a compiler warning. */			\
    nodep = NULL;							\
    /* Wind. */								\
    path->node = rbtree->rbt_root;					\
    for (pathp = path; pathp->node != NULL; pathp++) {			\
	int cmp = pathp->cmp = a_cmp(node, pathp->node);		\
	if (cmp < 0) {							\
	    pathp[1].node = rbtn_left_get(a_type, a_field,		\
	      pathp->node);						\
	} else {							\
	    pathp[1].node = rbtn_right_get(a_type, a_field,		\
	      pathp->node);						\
	    if (cmp == 0) {						\
	        /* Find node's successor, in preparation for swap. */	\
		pathp->cmp = 1;						\
		nodep = pathp;						\
		for (pathp++; pathp->node != NULL; pathp++) {		\
		    pathp->cmp = -1;					\
		    pathp[1].node = rbtn_left_get(a_type, a_field,	\
		      pathp->node);					\
		}							\
		break;							\
	    }								\
	}								\
    }									\
    assert(nodep->node == node);					\
    pathp--;								\
    if (pathp->node != node) {						\
	/* Swap node with its successor. */				\
	swap_loc = nodep;						\
	bool tred = rbtn_red_get(a_type, a_field, pathp->node);		\
	rbtn_color_set(a_type, a_field, pathp->node,			\
	  rbtn_red_get(a_type, a_field, node));				\
	rbtn_left_set(a_type, a_field, pathp->node,			\
	  rbtn_left_get(a_type, a_field, node));			\
	/* If node's successor is its right child, the following code */\
	/* will do the wrong thing for the right child pointer.       */\
	/* However, it doesn't matter, because the pointer will be    */\
	/* properly set when the successor is pruned.                 */\
	rbtn_right_set(a_type, a_field, pathp->node,			\
	  rbtn_right_get(a_type, a_field, node));			\
	rbtn_color_set(a_type, a_field, node, tred);			\
	/* The pruned leaf node's child pointers are never accessed   */\
	/* again, so don't bother setting them to nil.                */\
	nodep->node = pathp->node;					\
	pathp->node = node;						\
	if (nodep == path) {						\
	    rbtree->rbt_root = nodep->node;				\
	} else {							\
	    if (nodep[-1].cmp < 0) {					\
		rbtn_left_set(a_type, a_field, nodep[-1].node,		\
		  nodep->node);						\
	    } else {							\
		rbtn_right_set(a_type, a_field, nodep[-1].node,		\
		  nodep->node);						\
	    }								\
	}								\
    } else {								\
	a_type *left = rbtn_left_get(a_type, a_field, node);		\
	if (left != NULL) {						\
	    /* node has no successor, but it has a left child.        */\
	    /* Splice node out, without losing the left child.        */\
	    assert(!rbtn_red_get(a_type, a_field, node));		\
	    assert(rbtn_red_get(a_type, a_field, left));		\
	    rbtn_black_set(a_type, a_field, left);			\
	    if (pathp == path) {					\
		rbtree->rbt_root = left;				\
		/* Nothing to summarize -- the subtree rooted at the  */\
		/* node's left child hasn't changed, and it's now the */\
		/* root.					      */\
	    } else {							\
		if (pathp[-1].cmp < 0) {				\
		    rbtn_left_set(a_type, a_field, pathp[-1].node,	\
		      left);						\
		} else {						\
		    rbtn_right_set(a_type, a_field, pathp[-1].node,	\
		      left);						\
		}							\
		a_prefix##summarize_swapped_range(path, &pathp[-1],	\
		    swap_loc);						\
	    }								\
	    return;							\
	} else if (pathp == path) {					\
	    /* The tree only contained one node. */			\
	    rbtree->rbt_root = NULL;					\
	    return;							\
	}								\
    }									\
    /* We've now established the invariant that the node has no right */\
    /* child (well, morally; we didn't bother nulling it out if we    */\
    /* swapped it with its successor), and that the only nodes with   */\
    /* out-of-date summaries live in path[0], path[1], ..., pathp[-1].*/\
    if (rbtn_red_get(a_type, a_field, pathp->node)) {			\
	/* Prune red node, which requires no fixup. */			\
	assert(pathp[-1].cmp < 0);					\
	rbtn_left_set(a_type, a_field, pathp[-1].node, NULL);		\
	a_prefix##summarize_swapped_range(path, &pathp[-1], swap_loc);	\
	return;								\
    }									\
    /* The node to be pruned is black, so unwind until balance is     */\
    /* restored.                                                      */\
    pathp->node = NULL;							\
    for (pathp--; (uintptr_t)pathp >= (uintptr_t)path; pathp--) {	\
	assert(pathp->cmp != 0);					\
	if (pathp->cmp < 0) {						\
	    rbtn_left_set(a_type, a_field, pathp->node,			\
	      pathp[1].node);						\
	    if (rbtn_red_get(a_type, a_field, pathp->node)) {		\
		a_type *right = rbtn_right_get(a_type, a_field,		\
		  pathp->node);						\
		a_type *rightleft = rbtn_left_get(a_type, a_field,	\
		  right);						\
		a_type *tnode;						\
		if (rightleft != NULL && rbtn_red_get(a_type, a_field,	\
		  rightleft)) {						\
		    /* In the following diagrams, ||, //, and \\      */\
		    /* indicate the path to the removed node.         */\
		    /*                                                */\
		    /*      ||                                        */\
		    /*    pathp(r)                                    */\
		    /*  //        \                                   */\
		    /* (b)        (b)                                 */\
		    /*           /                                    */\
		    /*          (r)                                   */\
		    /*                                                */\
		    rbtn_black_set(a_type, a_field, pathp->node);	\
		    rbtn_rotate_right(a_type, a_field, right, tnode);	\
		    rbtn_right_set(a_type, a_field, pathp->node, tnode);\
		    rbtn_rotate_left(a_type, a_field, pathp->node,	\
		      tnode);						\
		    (void)a_summarize(pathp->node,			\
			rbtn_left_get(a_type, a_field, pathp->node),	\
			rbtn_right_get(a_type, a_field, pathp->node));	\
		    (void)a_summarize(right,				\
			rbtn_left_get(a_type, a_field, right),		\
			rbtn_right_get(a_type, a_field, right));	\
		} else {						\
		    /*      ||                                        */\
		    /*    pathp(r)                                    */\
		    /*  //        \                                   */\
		    /* (b)        (b)                                 */\
		    /*           /                                    */\
		    /*          (b)                                   */\
		    /*                                                */\
		    rbtn_rotate_left(a_type, a_field, pathp->node,	\
		      tnode);						\
		    (void)a_summarize(pathp->node,			\
			rbtn_left_get(a_type, a_field, pathp->node),	\
			rbtn_right_get(a_type, a_field, pathp->node));	\
		}							\
		(void)a_summarize(tnode, rbtn_left_get(a_type, a_field,	\
		    tnode), rbtn_right_get(a_type, a_field, tnode));	\
		/* Balance restored, but rotation modified subtree    */\
		/* root.                                              */\
		assert((uintptr_t)pathp > (uintptr_t)path);		\
		if (pathp[-1].cmp < 0) {				\
		    rbtn_left_set(a_type, a_field, pathp[-1].node,	\
		      tnode);						\
		} else {						\
		    rbtn_right_set(a_type, a_field, pathp[-1].node,	\
		      tnode);						\
		}							\
		a_prefix##summarize_swapped_range(path, &pathp[-1],	\
		    swap_loc);						\
		return;							\
	    } else {							\
		a_type *right = rbtn_right_get(a_type, a_field,		\
		  pathp->node);						\
		a_type *rightleft = rbtn_left_get(a_type, a_field,	\
		  right);						\
		if (rightleft != NULL && rbtn_red_get(a_type, a_field,	\
		  rightleft)) {						\
		    /*      ||                                        */\
		    /*    pathp(b)                                    */\
		    /*  //        \                                   */\
		    /* (b)        (b)                                 */\
		    /*           /                                    */\
		    /*          (r)                                   */\
		    a_type *tnode;					\
		    rbtn_black_set(a_type, a_field, rightleft);		\
		    rbtn_rotate_right(a_type, a_field, right, tnode);	\
		    rbtn_right_set(a_type, a_field, pathp->node, tnode);\
		    rbtn_rotate_left(a_type, a_field, pathp->node,	\
		      tnode);						\
		    (void)a_summarize(pathp->node,			\
			rbtn_left_get(a_type, a_field, pathp->node),	\
			rbtn_right_get(a_type, a_field, pathp->node));	\
		    (void)a_summarize(right,				\
			rbtn_left_get(a_type, a_field, right),		\
			rbtn_right_get(a_type, a_field, right));	\
		    (void)a_summarize(tnode,				\
			rbtn_left_get(a_type, a_field, tnode),		\
			rbtn_right_get(a_type, a_field, tnode));	\
		    /* Balance restored, but rotation modified        */\
		    /* subtree root, which may actually be the tree   */\
		    /* root.                                          */\
		    if (pathp == path) {				\
			/* Set root. */					\
			rbtree->rbt_root = tnode;			\
		    } else {						\
			if (pathp[-1].cmp < 0) {			\
			    rbtn_left_set(a_type, a_field,		\
			      pathp[-1].node, tnode);			\
			} else {					\
			    rbtn_right_set(a_type, a_field,		\
			      pathp[-1].node, tnode);			\
			}						\
			a_prefix##summarize_swapped_range(path,		\
			    &pathp[-1], swap_loc);			\
		    }							\
		    return;						\
		} else {						\
		    /*      ||                                        */\
		    /*    pathp(b)                                    */\
		    /*  //        \                                   */\
		    /* (b)        (b)                                 */\
		    /*           /                                    */\
		    /*          (b)                                   */\
		    a_type *tnode;					\
		    rbtn_red_set(a_type, a_field, pathp->node);		\
		    rbtn_rotate_left(a_type, a_field, pathp->node,	\
		      tnode);						\
		    (void)a_summarize(pathp->node,			\
			rbtn_left_get(a_type, a_field, pathp->node),	\
			rbtn_right_get(a_type, a_field, pathp->node));	\
		    (void)a_summarize(tnode,				\
			rbtn_left_get(a_type, a_field, tnode),		\
			rbtn_right_get(a_type, a_field, tnode));	\
		    pathp->node = tnode;				\
		}							\
	    }								\
	} else {							\
	    a_type *left;						\
	    rbtn_right_set(a_type, a_field, pathp->node,		\
	      pathp[1].node);						\
	    left = rbtn_left_get(a_type, a_field, pathp->node);		\
	    if (rbtn_red_get(a_type, a_field, left)) {			\
		a_type *tnode;						\
		a_type *leftright = rbtn_right_get(a_type, a_field,	\
		  left);						\
		a_type *leftrightleft = rbtn_left_get(a_type, a_field,	\
		  leftright);						\
		if (leftrightleft != NULL && rbtn_red_get(a_type,	\
		  a_field, leftrightleft)) {				\
		    /*      ||                                        */\
		    /*    pathp(b)                                    */\
		    /*   /        \\                                  */\
		    /* (r)        (b)                                 */\
		    /*   \                                            */\
		    /*   (b)                                          */\
		    /*   /                                            */\
		    /* (r)                                            */\
		    a_type *unode;					\
		    rbtn_black_set(a_type, a_field, leftrightleft);	\
		    rbtn_rotate_right(a_type, a_field, pathp->node,	\
		      unode);						\
		    rbtn_rotate_right(a_type, a_field, pathp->node,	\
		      tnode);						\
		    rbtn_right_set(a_type, a_field, unode, tnode);	\
		    rbtn_rotate_left(a_type, a_field, unode, tnode);	\
		    (void)a_summarize(pathp->node,			\
			rbtn_left_get(a_type, a_field, pathp->node),	\
			rbtn_right_get(a_type, a_field, pathp->node));	\
		    (void)a_summarize(unode,				\
			rbtn_left_get(a_type, a_field, unode),		\
			rbtn_right_get(a_type, a_field, unode));	\
		} else {						\
		    /*      ||                                        */\
		    /*    pathp(b)                                    */\
		    /*   /        \\                                  */\
		    /* (r)        (b)                                 */\
		    /*   \                                            */\
		    /*   (b)                                          */\
		    /*   /                                            */\
		    /* (b)                                            */\
		    assert(leftright != NULL);				\
		    rbtn_red_set(a_type, a_field, leftright);		\
		    rbtn_rotate_right(a_type, a_field, pathp->node,	\
		      tnode);						\
		    rbtn_black_set(a_type, a_field, tnode);		\
		    (void)a_summarize(pathp->node,			\
			rbtn_left_get(a_type, a_field, pathp->node),	\
			rbtn_right_get(a_type, a_field, pathp->node));	\
		}							\
		(void)a_summarize(tnode,				\
		    rbtn_left_get(a_type, a_field, tnode),		\
		    rbtn_right_get(a_type, a_field, tnode));		\
		/* Balance restored, but rotation modified subtree    */\
		/* root, which may actually be the tree root.         */\
		if (pathp == path) {					\
		    /* Set root. */					\
		    rbtree->rbt_root = tnode;				\
		} else {						\
		    if (pathp[-1].cmp < 0) {				\
			rbtn_left_set(a_type, a_field, pathp[-1].node,	\
			  tnode);					\
		    } else {						\
			rbtn_right_set(a_type, a_field, pathp[-1].node,	\
			  tnode);					\
		    }							\
		    a_prefix##summarize_swapped_range(path, &pathp[-1],	\
			swap_loc);					\
		}							\
		return;							\
	    } else if (rbtn_red_get(a_type, a_field, pathp->node)) {	\
		a_type *leftleft = rbtn_left_get(a_type, a_field, left);\
		if (leftleft != NULL && rbtn_red_get(a_type, a_field,	\
		  leftleft)) {						\
		    /*        ||                                      */\
		    /*      pathp(r)                                  */\
		    /*     /        \\                                */\
		    /*   (b)        (b)                               */\
		    /*   /                                            */\
		    /* (r)                                            */\
		    a_type *tnode;					\
		    rbtn_black_set(a_type, a_field, pathp->node);	\
		    rbtn_red_set(a_type, a_field, left);		\
		    rbtn_black_set(a_type, a_field, leftleft);		\
		    rbtn_rotate_right(a_type, a_field, pathp->node,	\
		      tnode);						\
		    (void)a_summarize(pathp->node,			\
			rbtn_left_get(a_type, a_field, pathp->node),	\
			rbtn_right_get(a_type, a_field, pathp->node));	\
		    (void)a_summarize(tnode,				\
			rbtn_left_get(a_type, a_field, tnode),		\
			rbtn_right_get(a_type, a_field, tnode));	\
		    /* Balance restored, but rotation modified        */\
		    /* subtree root.                                  */\
		    assert((uintptr_t)pathp > (uintptr_t)path);		\
		    if (pathp[-1].cmp < 0) {				\
			rbtn_left_set(a_type, a_field, pathp[-1].node,	\
			  tnode);					\
		    } else {						\
			rbtn_right_set(a_type, a_field, pathp[-1].node,	\
			  tnode);					\
		    }							\
		    a_prefix##summarize_swapped_range(path, &pathp[-1],	\
			swap_loc);					\
		    return;						\
		} else {						\
		    /*        ||                                      */\
		    /*      pathp(r)                                  */\
		    /*     /        \\                                */\
		    /*   (b)        (b)                               */\
		    /*   /                                            */\
		    /* (b)                                            */\
		    rbtn_red_set(a_type, a_field, left);		\
		    rbtn_black_set(a_type, a_field, pathp->node);	\
		    /* Balance restored. */				\
		    a_prefix##summarize_swapped_range(path, pathp,	\
			swap_loc);					\
		    return;						\
		}							\
	    } else {							\
		a_type *leftleft = rbtn_left_get(a_type, a_field, left);\
		if (leftleft != NULL && rbtn_red_get(a_type, a_field,	\
		  leftleft)) {						\
		    /*               ||                               */\
		    /*             pathp(b)                           */\
		    /*            /        \\                         */\
		    /*          (b)        (b)                        */\
		    /*          /                                     */\
		    /*        (r)                                     */\
		    a_type *tnode;					\
		    rbtn_black_set(a_type, a_field, leftleft);		\
		    rbtn_rotate_right(a_type, a_field, pathp->node,	\
		      tnode);						\
		    (void)a_summarize(pathp->node,			\
			rbtn_left_get(a_type, a_field, pathp->node),	\
			rbtn_right_get(a_type, a_field, pathp->node));	\
		    (void)a_summarize(tnode,				\
			rbtn_left_get(a_type, a_field, tnode),		\
			rbtn_right_get(a_type, a_field, tnode));	\
		    /* Balance restored, but rotation modified        */\
		    /* subtree root, which may actually be the tree   */\
		    /* root.                                          */\
		    if (pathp == path) {				\
			/* Set root. */					\
			rbtree->rbt_root = tnode;			\
		    } else {						\
			if (pathp[-1].cmp < 0) {			\
			    rbtn_left_set(a_type, a_field,		\
			      pathp[-1].node, tnode);			\
			} else {					\
			    rbtn_right_set(a_type, a_field,		\
			      pathp[-1].node, tnode);			\
			}						\
		        a_prefix##summarize_swapped_range(path,		\
			    &pathp[-1], swap_loc);			\
		    }							\
		    return;						\
		} else {						\
		    /*               ||                               */\
		    /*             pathp(b)                           */\
		    /*            /        \\                         */\
		    /*          (b)        (b)                        */\
		    /*          /                                     */\
		    /*        (b)                                     */\
		    rbtn_red_set(a_type, a_field, left);		\
		    (void)a_summarize(pathp->node,			\
			rbtn_left_get(a_type, a_field, pathp->node),	\
			rbtn_right_get(a_type, a_field, pathp->node));	\
		}							\
	    }								\
	}								\
    }									\
    /* Set root. */							\
    rbtree->rbt_root = path->node;					\
    assert(!rbtn_red_get(a_type, a_field, rbtree->rbt_root));		\
}									\
a_attr a_type *								\
a_prefix##iter_recurse(a_rbt_type *rbtree, a_type *node,		\
  a_type *(*cb)(a_rbt_type *, a_type *, void *), void *arg) {		\
    if (node == NULL) {							\
	return NULL;							\
    } else {								\
	a_type *ret;							\
	if ((ret = a_prefix##iter_recurse(rbtree, rbtn_left_get(a_type,	\
	  a_field, node), cb, arg)) != NULL || (ret = cb(rbtree, node,	\
	  arg)) != NULL) {						\
	    return ret;							\
	}								\
	return a_prefix##iter_recurse(rbtree, rbtn_right_get(a_type,	\
	  a_field, node), cb, arg);					\
    }									\
}									\
a_attr a_type *								\
a_prefix##iter_start(a_rbt_type *rbtree, a_type *start, a_type *node,	\
  a_type *(*cb)(a_rbt_type *, a_type *, void *), void *arg) {		\
    int cmp = a_cmp(start, node);					\
    if (cmp < 0) {							\
	a_type *ret;							\
	if ((ret = a_prefix##iter_start(rbtree, start,			\
	  rbtn_left_get(a_type, a_field, node), cb, arg)) != NULL ||	\
	  (ret = cb(rbtree, node, arg)) != NULL) {			\
	    return ret;							\
	}								\
	return a_prefix##iter_recurse(rbtree, rbtn_right_get(a_type,	\
	  a_field, node), cb, arg);					\
    } else if (cmp > 0) {						\
	return a_prefix##iter_start(rbtree, start,			\
	  rbtn_right_get(a_type, a_field, node), cb, arg);		\
    } else {								\
	a_type *ret;							\
	if ((ret = cb(rbtree, node, arg)) != NULL) {			\
	    return ret;							\
	}								\
	return a_prefix##iter_recurse(rbtree, rbtn_right_get(a_type,	\
	  a_field, node), cb, arg);					\
    }									\
}									\
a_attr a_type *								\
a_prefix##iter(a_rbt_type *rbtree, a_type *start, a_type *(*cb)(	\
  a_rbt_type *, a_type *, void *), void *arg) {				\
    a_type *ret;							\
    if (start != NULL) {						\
	ret = a_prefix##iter_start(rbtree, start, rbtree->rbt_root,	\
	  cb, arg);							\
    } else {								\
	ret = a_prefix##iter_recurse(rbtree, rbtree->rbt_root, cb, arg);\
    }									\
    return ret;								\
}									\
a_attr a_type *								\
a_prefix##reverse_iter_recurse(a_rbt_type *rbtree, a_type *node,	\
  a_type *(*cb)(a_rbt_type *, a_type *, void *), void *arg) {		\
    if (node == NULL) {							\
	return NULL;							\
    } else {								\
	a_type *ret;							\
	if ((ret = a_prefix##reverse_iter_recurse(rbtree,		\
	  rbtn_right_get(a_type, a_field, node), cb, arg)) != NULL ||	\
	  (ret = cb(rbtree, node, arg)) != NULL) {			\
	    return ret;							\
	}								\
	return a_prefix##reverse_iter_recurse(rbtree,			\
	  rbtn_left_get(a_type, a_field, node), cb, arg);		\
    }									\
}									\
a_attr a_type *								\
a_prefix##reverse_iter_start(a_rbt_type *rbtree, a_type *start,		\
  a_type *node, a_type *(*cb)(a_rbt_type *, a_type *, void *),		\
  void *arg) {								\
    int cmp = a_cmp(start, node);					\
    if (cmp > 0) {							\
	a_type *ret;							\
	if ((ret = a_prefix##reverse_iter_start(rbtree, start,		\
	  rbtn_right_get(a_type, a_field, node), cb, arg)) != NULL ||	\
	  (ret = cb(rbtree, node, arg)) != NULL) {			\
	    return ret;							\
	}								\
	return a_prefix##reverse_iter_recurse(rbtree,			\
	  rbtn_left_get(a_type, a_field, node), cb, arg);		\
    } else if (cmp < 0) {						\
	return a_prefix##reverse_iter_start(rbtree, start,		\
	  rbtn_left_get(a_type, a_field, node), cb, arg);		\
    } else {								\
	a_type *ret;							\
	if ((ret = cb(rbtree, node, arg)) != NULL) {			\
	    return ret;							\
	}								\
	return a_prefix##reverse_iter_recurse(rbtree,			\
	  rbtn_left_get(a_type, a_field, node), cb, arg);		\
    }									\
}									\
a_attr a_type *								\
a_prefix##reverse_iter(a_rbt_type *rbtree, a_type *start,		\
  a_type *(*cb)(a_rbt_type *, a_type *, void *), void *arg) {		\
    a_type *ret;							\
    if (start != NULL) {						\
	ret = a_prefix##reverse_iter_start(rbtree, start,		\
	  rbtree->rbt_root, cb, arg);					\
    } else {								\
	ret = a_prefix##reverse_iter_recurse(rbtree, rbtree->rbt_root,	\
	  cb, arg);							\
    }									\
    return ret;								\
}									\
a_attr void								\
a_prefix##destroy_recurse(a_rbt_type *rbtree, a_type *node, void (*cb)(	\
  a_type *, void *), void *arg) {					\
    if (node == NULL) {							\
	return;								\
    }									\
    a_prefix##destroy_recurse(rbtree, rbtn_left_get(a_type, a_field,	\
      node), cb, arg);							\
    rbtn_left_set(a_type, a_field, (node), NULL);			\
    a_prefix##destroy_recurse(rbtree, rbtn_right_get(a_type, a_field,	\
      node), cb, arg);							\
    rbtn_right_set(a_type, a_field, (node), NULL);			\
    if (cb) {								\
	cb(node, arg);							\
    }									\
}									\
a_attr void								\
a_prefix##destroy(a_rbt_type *rbtree, void (*cb)(a_type *, void *),	\
  void *arg) {								\
    a_prefix##destroy_recurse(rbtree, rbtree->rbt_root, cb, arg);	\
    rbtree->rbt_root = NULL;						\
}									\
/* BEGIN SUMMARIZED-ONLY IMPLEMENTATION */				\
rb_summarized_only_##a_is_summarized(					\
static inline a_prefix##path_entry_t *					\
a_prefix##wind(a_rbt_type *rbtree,					\
    a_prefix##path_entry_t path[RB_MAX_DEPTH], a_type *node) {		\
    a_prefix##path_entry_t *pathp;					\
    path->node = rbtree->rbt_root;					\
    for (pathp = path; ; pathp++) {					\
	assert((size_t)(pathp - path) < RB_MAX_DEPTH);			\
	pathp->cmp = a_cmp(node, pathp->node);				\
	if (pathp->cmp < 0) {						\
	    pathp[1].node = rbtn_left_get(a_type, a_field,		\
		pathp->node);						\
	} else if (pathp->cmp == 0) {					\
	    return pathp;						\
	} else {							\
	    pathp[1].node = rbtn_right_get(a_type, a_field,		\
		pathp->node);						\
	}								\
    }									\
    unreachable();							\
}									\
a_attr void								\
a_prefix##update_summaries(a_rbt_type *rbtree, a_type *node) {		\
    a_prefix##path_entry_t path[RB_MAX_DEPTH];				\
    a_prefix##path_entry_t *pathp = a_prefix##wind(rbtree, path, node);	\
    a_prefix##summarize_range(path, pathp);				\
}									\
a_attr bool								\
a_prefix##empty_filtered(a_rbt_type *rbtree,				\
  bool (*filter_node)(void *, a_type *),				\
  bool (*filter_subtree)(void *, a_type *),				\
  void *filter_ctx) {							\
    a_type *node = rbtree->rbt_root;					\
    return node == NULL || !filter_subtree(filter_ctx, node);		\
}									\
static inline a_type *							\
a_prefix##first_filtered_from_node(a_type *node,			\
  bool (*filter_node)(void *, a_type *),				\
  bool (*filter_subtree)(void *, a_type *),				\
  void *filter_ctx) {							\
    assert(node != NULL && filter_subtree(filter_ctx, node));		\
    while (true) {							\
	a_type *left = rbtn_left_get(a_type, a_field, node);		\
	a_type *right = rbtn_right_get(a_type, a_field, node);		\
	if (left != NULL && filter_subtree(filter_ctx, left)) {		\
	    node = left;						\
	} else if (filter_node(filter_ctx, node)) {			\
	    return node;						\
	} else {							\
		assert(right != NULL					\
		    && filter_subtree(filter_ctx, right));		\
		node = right;						\
	}								\
    }									\
    unreachable();							\
}									\
a_attr a_type *								\
a_prefix##first_filtered(a_rbt_type *rbtree,				\
  bool (*filter_node)(void *, a_type *),				\
  bool (*filter_subtree)(void *, a_type *),				\
  void *filter_ctx) {							\
    a_type *node = rbtree->rbt_root;					\
    if (node == NULL || !filter_subtree(filter_ctx, node)) {		\
	return NULL;							\
    }									\
    return a_prefix##first_filtered_from_node(node, filter_node,	\
	filter_subtree, filter_ctx);					\
}									\
static inline a_type *							\
a_prefix##last_filtered_from_node(a_type *node,				\
  bool (*filter_node)(void *, a_type *),				\
  bool (*filter_subtree)(void *, a_type *),				\
  void *filter_ctx) {							\
    assert(node != NULL && filter_subtree(filter_ctx, node));		\
    while (true) {							\
	a_type *left = rbtn_left_get(a_type, a_field, node);		\
	a_type *right = rbtn_right_get(a_type, a_field, node);		\
	if (right != NULL && filter_subtree(filter_ctx, right)) {	\
	    node = right;						\
	} else if (filter_node(filter_ctx, node)) {			\
	    return node;						\
	} else {							\
		assert(left != NULL					\
		    && filter_subtree(filter_ctx, left));		\
		node = left;						\
	}								\
    }									\
    unreachable();							\
}									\
a_attr a_type *								\
a_prefix##last_filtered(a_rbt_type *rbtree,				\
  bool (*filter_node)(void *, a_type *),				\
  bool (*filter_subtree)(void *, a_type *),				\
  void *filter_ctx) {							\
    a_type *node = rbtree->rbt_root;					\
    if (node == NULL || !filter_subtree(filter_ctx, node)) {		\
	return NULL;							\
    }									\
    return a_prefix##last_filtered_from_node(node, filter_node,		\
	filter_subtree, filter_ctx);					\
}									\
/* Internal implementation function.  Search for a node comparing     */\
/* equal to key matching the filter.  If such a node is in the tree,  */\
/* return it.  Additionally, the caller has the option to ask for     */\
/* bounds on the next / prev node in the tree passing the filter.     */\
/* If nextbound is true, then this function will do one of the        */\
/* following:                                                         */\
/* - Fill in *nextbound_node with the smallest node in the tree       */\
/*   greater than key passing the filter, and NULL-out                */\
/*   *nextbound_subtree.                                              */\
/* - Fill in *nextbound_subtree with a parent of that node which is   */\
/*   not a parent of the searched-for node, and NULL-out              */\
/*   *nextbound_node.                                                 */\
/* - NULL-out both *nextbound_node and *nextbound_subtree, in which   */\
/*   case no node greater than key but passing the filter is in the   */\
/*   tree.                                                            */\
/* The prevbound case is similar.  If the caller knows that key is in */\
/* the tree and that the subtree rooted at key does not contain a     */\
/* node satisfying the bound being searched for, then they can pass   */\
/* false for include_subtree, in which case we won't bother searching */\
/* there (risking a cache miss).                                      */\
/*                                                                    */\
/* This API is unfortunately complex; but the logic for filtered      */\
/* searches is very subtle, and otherwise we would have to repeat it  */\
/* multiple times for filtered search, nsearch, psearch, next, and    */\
/* prev.                                                              */\
static inline a_type *							\
a_prefix##search_with_filter_bounds(a_rbt_type *rbtree,			\
  const a_type *key,							\
  bool (*filter_node)(void *, a_type *),				\
  bool (*filter_subtree)(void *, a_type *),				\
  void *filter_ctx,							\
  bool include_subtree,							\
  bool nextbound, a_type **nextbound_node, a_type **nextbound_subtree,	\
  bool prevbound, a_type **prevbound_node, a_type **prevbound_subtree) {\
    if (nextbound) {							\
	    *nextbound_node = NULL;					\
	    *nextbound_subtree = NULL;					\
    }									\
    if (prevbound) {							\
	    *prevbound_node = NULL;					\
	    *prevbound_subtree = NULL;					\
    }									\
    a_type *tnode = rbtree->rbt_root;					\
    while (tnode != NULL && filter_subtree(filter_ctx, tnode)) {	\
	int cmp = a_cmp(key, tnode);					\
	a_type *tleft = rbtn_left_get(a_type, a_field, tnode);		\
	a_type *tright = rbtn_right_get(a_type, a_field, tnode);	\
	if (cmp < 0) {							\
	    if (nextbound) {						\
		if (filter_node(filter_ctx, tnode)) {			\
		    *nextbound_node = tnode;				\
		    *nextbound_subtree = NULL;				\
		} else if (tright != NULL && filter_subtree(		\
		    filter_ctx, tright)) {				\
		    *nextbound_node = NULL;				\
		    *nextbound_subtree = tright;			\
		}							\
	    }								\
	    tnode = tleft;						\
	} else if (cmp > 0) {						\
	    if (prevbound) {						\
		if (filter_node(filter_ctx, tnode)) {			\
		    *prevbound_node = tnode;				\
		    *prevbound_subtree = NULL;				\
		} else if (tleft != NULL && filter_subtree(		\
		    filter_ctx, tleft)) {				\
		    *prevbound_node = NULL;				\
		    *prevbound_subtree = tleft;				\
		}							\
	    }								\
	    tnode = tright;						\
	} else {							\
	    if (filter_node(filter_ctx, tnode)) {			\
		return tnode;						\
	    }								\
	    if (include_subtree) {					\
		if (prevbound && tleft != NULL && filter_subtree(	\
		    filter_ctx, tleft)) {				\
		    *prevbound_node = NULL;				\
		    *prevbound_subtree = tleft;				\
		}							\
		if (nextbound && tright != NULL && filter_subtree(	\
		    filter_ctx, tright)) {				\
		    *nextbound_node = NULL;				\
		    *nextbound_subtree = tright;			\
		}							\
	    }								\
	    return NULL;						\
	}								\
    }									\
    return NULL;							\
}									\
a_attr a_type *								\
a_prefix##next_filtered(a_rbt_type *rbtree, a_type *node,		\
  bool (*filter_node)(void *, a_type *),				\
  bool (*filter_subtree)(void *, a_type *),				\
  void *filter_ctx) {							\
    a_type *nright = rbtn_right_get(a_type, a_field, node);		\
    if (nright != NULL && filter_subtree(filter_ctx, nright)) {		\
	return a_prefix##first_filtered_from_node(nright, filter_node,	\
	    filter_subtree, filter_ctx);				\
    }									\
    a_type *node_candidate;						\
    a_type *subtree_candidate;						\
    a_type *search_result = a_prefix##search_with_filter_bounds(	\
	rbtree, node, filter_node, filter_subtree, filter_ctx,		\
	/* include_subtree */ false,					\
	/* nextbound */ true, &node_candidate, &subtree_candidate,	\
	/* prevbound */ false, NULL, NULL);				\
    assert(node == search_result					\
	|| !filter_node(filter_ctx, node));				\
    if (node_candidate != NULL) {					\
	return node_candidate;						\
    }									\
    if (subtree_candidate != NULL) {					\
	return a_prefix##first_filtered_from_node(			\
	    subtree_candidate, filter_node, filter_subtree,		\
	    filter_ctx);						\
    }									\
    return NULL;							\
}									\
a_attr a_type *								\
a_prefix##prev_filtered(a_rbt_type *rbtree, a_type *node,		\
  bool (*filter_node)(void *, a_type *),				\
  bool (*filter_subtree)(void *, a_type *),				\
  void *filter_ctx) {							\
    a_type *nleft = rbtn_left_get(a_type, a_field, node);		\
    if (nleft != NULL && filter_subtree(filter_ctx, nleft)) {		\
	return a_prefix##last_filtered_from_node(nleft, filter_node,	\
	    filter_subtree, filter_ctx);				\
    }									\
    a_type *node_candidate;						\
    a_type *subtree_candidate;						\
    a_type *search_result = a_prefix##search_with_filter_bounds(	\
	rbtree, node, filter_node, filter_subtree, filter_ctx,		\
	/* include_subtree */ false,					\
	/* nextbound */ false, NULL, NULL,				\
	/* prevbound */ true, &node_candidate, &subtree_candidate);	\
    assert(node == search_result					\
	|| !filter_node(filter_ctx, node));				\
    if (node_candidate != NULL) {					\
	return node_candidate;						\
    }									\
    if (subtree_candidate != NULL) {					\
	return a_prefix##last_filtered_from_node(			\
	    subtree_candidate, filter_node, filter_subtree,		\
	    filter_ctx);						\
    }									\
    return NULL;							\
}									\
a_attr a_type *								\
a_prefix##search_filtered(a_rbt_type *rbtree, const a_type *key,	\
  bool (*filter_node)(void *, a_type *),				\
  bool (*filter_subtree)(void *, a_type *),				\
  void *filter_ctx) {							\
    a_type *result = a_prefix##search_with_filter_bounds(rbtree, key,	\
	filter_node, filter_subtree, filter_ctx,			\
	/* include_subtree */ false,					\
	/* nextbound */ false, NULL, NULL,				\
	/* prevbound */ false, NULL, NULL);				\
    return result;							\
}									\
a_attr a_type *								\
a_prefix##nsearch_filtered(a_rbt_type *rbtree, const a_type *key,	\
  bool (*filter_node)(void *, a_type *),				\
  bool (*filter_subtree)(void *, a_type *),				\
  void *filter_ctx) {							\
    a_type *node_candidate;						\
    a_type *subtree_candidate;						\
    a_type *result = a_prefix##search_with_filter_bounds(rbtree, key,	\
	filter_node, filter_subtree, filter_ctx,			\
	/* include_subtree */ true,					\
	/* nextbound */ true, &node_candidate, &subtree_candidate,	\
	/* prevbound */ false, NULL, NULL);				\
    if (result != NULL) {						\
	return result;							\
    }									\
    if (node_candidate != NULL) {					\
	return node_candidate;						\
    }									\
    if (subtree_candidate != NULL) {					\
	return a_prefix##first_filtered_from_node(			\
	    subtree_candidate, filter_node, filter_subtree,		\
	    filter_ctx);						\
    }									\
    return NULL;							\
}									\
a_attr a_type *								\
a_prefix##psearch_filtered(a_rbt_type *rbtree, const a_type *key,	\
  bool (*filter_node)(void *, a_type *),				\
  bool (*filter_subtree)(void *, a_type *),				\
  void *filter_ctx) {							\
    a_type *node_candidate;						\
    a_type *subtree_candidate;						\
    a_type *result = a_prefix##search_with_filter_bounds(rbtree, key,	\
	filter_node, filter_subtree, filter_ctx,			\
	/* include_subtree */ true,					\
	/* nextbound */ false, NULL, NULL,				\
	/* prevbound */ true, &node_candidate, &subtree_candidate);	\
    if (result != NULL) {						\
	return result;							\
    }									\
    if (node_candidate != NULL) {					\
	return node_candidate;						\
    }									\
    if (subtree_candidate != NULL) {					\
	return a_prefix##last_filtered_from_node(			\
	    subtree_candidate, filter_node, filter_subtree,		\
	    filter_ctx);						\
    }									\
    return NULL;							\
}									\
a_attr a_type *								\
a_prefix##iter_recurse_filtered(a_rbt_type *rbtree, a_type *node,	\
  a_type *(*cb)(a_rbt_type *, a_type *, void *), void *arg,		\
  bool (*filter_node)(void *, a_type *),				\
  bool (*filter_subtree)(void *, a_type *),				\
  void *filter_ctx) {							\
    if (node == NULL || !filter_subtree(filter_ctx, node)) {		\
	return NULL;							\
    }									\
    a_type *ret;							\
    a_type *left = rbtn_left_get(a_type, a_field, node);		\
    a_type *right = rbtn_right_get(a_type, a_field, node);		\
    ret = a_prefix##iter_recurse_filtered(rbtree, left, cb, arg,	\
      filter_node, filter_subtree, filter_ctx);				\
    if (ret != NULL) {							\
	return ret;							\
    }									\
    if (filter_node(filter_ctx, node)) {				\
	ret = cb(rbtree, node, arg);					\
    }									\
    if (ret != NULL) {							\
	return ret;							\
    }									\
    return a_prefix##iter_recurse_filtered(rbtree, right, cb, arg,	\
      filter_node, filter_subtree, filter_ctx);				\
}									\
a_attr a_type *								\
a_prefix##iter_start_filtered(a_rbt_type *rbtree, a_type *start,	\
  a_type *node, a_type *(*cb)(a_rbt_type *, a_type *, void *),		\
  void *arg, bool (*filter_node)(void *, a_type *),			\
  bool (*filter_subtree)(void *, a_type *),				\
  void *filter_ctx) {							\
    if (!filter_subtree(filter_ctx, node)) {				\
	return NULL;							\
    }									\
    int cmp = a_cmp(start, node);					\
    a_type *ret;							\
    a_type *left = rbtn_left_get(a_type, a_field, node);		\
    a_type *right = rbtn_right_get(a_type, a_field, node);		\
    if (cmp < 0) {							\
	ret = a_prefix##iter_start_filtered(rbtree, start, left, cb,	\
	    arg, filter_node, filter_subtree, filter_ctx);		\
	if (ret != NULL) {						\
	    return ret;							\
	}								\
	if (filter_node(filter_ctx, node)) {				\
	    ret = cb(rbtree, node, arg);				\
	    if (ret != NULL) {						\
		return ret;						\
	    }								\
	}								\
	return a_prefix##iter_recurse_filtered(rbtree, right, cb, arg,	\
	    filter_node, filter_subtree, filter_ctx);			\
    } else if (cmp > 0) {						\
	return a_prefix##iter_start_filtered(rbtree, start, right,	\
	  cb, arg, filter_node, filter_subtree, filter_ctx);		\
    } else {								\
	if (filter_node(filter_ctx, node)) {				\
	    ret = cb(rbtree, node, arg);				\
	    if (ret != NULL) {						\
		return ret;						\
	    }								\
	}								\
	return a_prefix##iter_recurse_filtered(rbtree, right, cb, arg,	\
	  filter_node, filter_subtree, filter_ctx);			\
    }									\
}									\
a_attr a_type *								\
a_prefix##iter_filtered(a_rbt_type *rbtree, a_type *start,		\
  a_type *(*cb)(a_rbt_type *, a_type *, void *), void *arg,		\
  bool (*filter_node)(void *, a_type *),				\
  bool (*filter_subtree)(void *, a_type *),				\
  void *filter_ctx) {							\
    a_type *ret;							\
    if (start != NULL) {						\
	ret = a_prefix##iter_start_filtered(rbtree, start,		\
	    rbtree->rbt_root, cb, arg, filter_node, filter_subtree,	\
	    filter_ctx);						\
    } else {								\
	ret = a_prefix##iter_recurse_filtered(rbtree, rbtree->rbt_root,	\
	    cb, arg, filter_node, filter_subtree, filter_ctx);		\
    }									\
    return ret;								\
}									\
a_attr a_type *								\
a_prefix##reverse_iter_recurse_filtered(a_rbt_type *rbtree,		\
  a_type *node, a_type *(*cb)(a_rbt_type *, a_type *, void *),		\
  void *arg,								\
  bool (*filter_node)(void *, a_type *),				\
  bool (*filter_subtree)(void *, a_type *),				\
  void *filter_ctx) {							\
    if (node == NULL || !filter_subtree(filter_ctx, node)) {		\
	return NULL;							\
    }									\
    a_type *ret;							\
    a_type *left = rbtn_left_get(a_type, a_field, node);		\
    a_type *right = rbtn_right_get(a_type, a_field, node);		\
    ret = a_prefix##reverse_iter_recurse_filtered(rbtree, right, cb,	\
	arg, filter_node, filter_subtree, filter_ctx);			\
    if (ret != NULL) {							\
	return ret;							\
    }									\
    if (filter_node(filter_ctx, node)) {				\
	ret = cb(rbtree, node, arg);					\
    }									\
    if (ret != NULL) {							\
	return ret;							\
    }									\
    return a_prefix##reverse_iter_recurse_filtered(rbtree, left, cb,	\
      arg, filter_node, filter_subtree, filter_ctx);			\
}									\
a_attr a_type *								\
a_prefix##reverse_iter_start_filtered(a_rbt_type *rbtree, a_type *start,\
  a_type *node, a_type *(*cb)(a_rbt_type *, a_type *, void *),		\
  void *arg, bool (*filter_node)(void *, a_type *),			\
  bool (*filter_subtree)(void *, a_type *),				\
  void *filter_ctx) {							\
    if (!filter_subtree(filter_ctx, node)) {				\
	return NULL;							\
    }									\
    int cmp = a_cmp(start, node);					\
    a_type *ret;							\
    a_type *left = rbtn_left_get(a_type, a_field, node);		\
    a_type *right = rbtn_right_get(a_type, a_field, node);		\
    if (cmp > 0) {							\
	ret = a_prefix##reverse_iter_start_filtered(rbtree, start,	\
	    right, cb, arg, filter_node, filter_subtree, filter_ctx);	\
	if (ret != NULL) {						\
	    return ret;							\
	}								\
	if (filter_node(filter_ctx, node)) {				\
	    ret = cb(rbtree, node, arg);				\
	    if (ret != NULL) {						\
		return ret;						\
	    }								\
	}								\
	return a_prefix##reverse_iter_recurse_filtered(rbtree, left, cb,\
	    arg, filter_node, filter_subtree, filter_ctx);		\
    } else if (cmp < 0) {						\
	return a_prefix##reverse_iter_start_filtered(rbtree, start,	\
	  left, cb, arg, filter_node, filter_subtree, filter_ctx);	\
    } else {								\
	if (filter_node(filter_ctx, node)) {				\
	    ret = cb(rbtree, node, arg);				\
	    if (ret != NULL) {						\
		return ret;						\
	    }								\
	}								\
	return a_prefix##reverse_iter_recurse_filtered(rbtree, left, cb,\
	  arg, filter_node, filter_subtree, filter_ctx);		\
    }									\
}									\
a_attr a_type *								\
a_prefix##reverse_iter_filtered(a_rbt_type *rbtree, a_type *start,	\
  a_type *(*cb)(a_rbt_type *, a_type *, void *), void *arg,		\
  bool (*filter_node)(void *, a_type *),				\
  bool (*filter_subtree)(void *, a_type *),				\
  void *filter_ctx) {							\
    a_type *ret;							\
    if (start != NULL) {						\
	ret = a_prefix##reverse_iter_start_filtered(rbtree, start,	\
	    rbtree->rbt_root, cb, arg, filter_node, filter_subtree,	\
	    filter_ctx);						\
    } else {								\
	ret = a_prefix##reverse_iter_recurse_filtered(rbtree,		\
	    rbtree->rbt_root, cb, arg, filter_node, filter_subtree,	\
	    filter_ctx);						\
    }									\
    return ret;								\
}									\
) /* end rb_summarized_only */

#endif /* JEMALLOC_INTERNAL_RB_H */
