/* quicklist.c - A doubly linked list of listpacks
 *
 * Copyright (c) 2014, Matt Stancliff <matt@genges.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must start the above copyright notice,
 *     this quicklist of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this quicklist of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * Note: every function start with quicklist_p_ has view on one partition.
 * 
 * Note: every function start with quicklist_n_ has view on one node. 
 * 
 * Note: every function start with quicklist_p_ should keep merge strategy and
 * partition compress strategy.
 */

#include <stdio.h>
#include <string.h> /* for memcpy */
#include <limits.h>
#include "quicklist.h"
#include "zmalloc.h"
#include "config.h"
#include "listpack.h"
#include "util.h" /* for ll2string */
#include "lzf.h"

/**
 * Optimized levels for size-based fill.
 * Note that the largest possible limit is (64k-1),
 * so even if each record takes just one byte,
 * it still won't overflow the 16 bit count field.
 */
static size_t QUICKLIST_OPTIMIZED_LEVEL[5] = {4096, 8192, 16384, 32768, 65535};
static_assert(65535 < 1<<QUICKLIST_PACK_SIZE_BITS, "invalid level");

/**
 * quicklist_transform_index - Transform index @i to no negative.
 * @length: the length of the array that @i index to
 * 
 * Return: the no negative represent of @i. -1 is returned if @i is not valid.
 * 
 * Note: negative @i is used to index from tail. -1 is index to the last
 * element of the array.
 */
static long quicklist_transform_index(long length, long i)
{
	if (i < 0)
		i += length;
	if (i < 0 || i >= length)
		return -1;
	return i;
}

/**
 * __quicklist_fill_new - Create a new limitation for packed node.
 * @pack_max_count: maximum number of elements within a packed node
 * @pack_max_size: maximum size of a packed node
 */
static struct quicklist_fill *__quicklist_fill_new(int pack_max_count,
						   int pack_max_size)
{
	struct quicklist_fill *f = zmalloc(sizeof(*f));
	f->pack_max_count = pack_max_count;
	f->pack_max_size = pack_max_size;
	return f;
}

#define FILL_MAX ((1 << (QUICKLIST_FILL_BITS-1))-1)

/**
 * Maximum size in bytes of element within a listpack that is limited by count.
 */
#define QUICKLIST_SIZE_SAFETY_LIMIT 8192

/**
 * quicklist_fill_new - Create a new limitation for packed node base on @fill.
 * @fill: packed node is limited by size if fill < 0, Otherwise,
 * node is limited by count.
 * 
 * Note: see @QUICKLIST_OPTIMIZED_LEVEL and @QUICKLIST_SIZE_SAFETY_LIMIT.
 * Note: if (fill==0), every node is a plain node.
 * Note: free function is zfree().
 */
struct quicklist_fill *quicklist_fill_new(int fill)
{
	if (fill >= 0) {
		if (fill > FILL_MAX)
			fill = FILL_MAX;
		return __quicklist_fill_new(fill, QUICKLIST_SIZE_SAFETY_LIMIT);
	}

	size_t i = (-fill) - 1;
	size_t max_level = sizeof(QUICKLIST_OPTIMIZED_LEVEL) /
			   sizeof(*QUICKLIST_OPTIMIZED_LEVEL);
	if (i >= max_level)
		i = max_level - 1;
	return __quicklist_fill_new(INT_MAX, QUICKLIST_OPTIMIZED_LEVEL[i]);
}

/**
 * quicklist_fill_dup - Duplicate @fill.
 */
static struct quicklist_fill *quicklist_fill_dup(struct quicklist_fill *fill)
{
	return __quicklist_fill_new(fill->pack_max_count, fill->pack_max_size);
}

/** encoding + data bytes + total bytes */
#define QUICKLIST_PACK_ENTRY_SIZE_OVERHEAD (1+4+5)

/** total bytes + element length + end byte */
#define QUICKLIST_PACK_MERGE_SIZE_REDUCE (4+2+1)

/**
 * quicklist_is_large_element - Test element with size @sz is a large element
 * or not based on limitation @fill.
 * 
 * Return: 1 if element is large, 0 if not.
 */
static int quicklist_is_large_element(struct quicklist_fill *fill, size_t sz)
{
	if (fill->pack_max_count == 0)
		return 1;

	return sz + QUICKLIST_PACK_MERGE_SIZE_REDUCE + 
		QUICKLIST_PACK_ENTRY_SIZE_OVERHEAD > fill->pack_max_size;
}

/**
 * quicklist_n_new_raw - Create a new raw node which contains one element
 * with value @value and size @sz based on limitation @fill.
 * 
 * Note: the returned node's prev and next is undefined.
 * Note: free function is quicklist_n_free().
 */
static struct quicklist_node *quicklist_n_new_raw(struct quicklist_fill *fill,
							void *value, size_t sz)
{
	struct quicklist_node *node = zmalloc(sizeof(*node));
	if (quicklist_is_large_element(fill, sz)) {
		node->carry = zmalloc(sz);
		memcpy(node->carry, value, sz);
		node->raw_sz = sz;
		node->container = QUICKLIST_NODE_CONTAINER_PLAIN;
	} else {
		node->carry = lpPrepend(lpNew(0), value, sz);
		node->raw_sz = lpBytes(node->carry);
		node->container = QUICKLIST_NODE_CONTAINER_PACKED;
	}
	node->count = 1;
	node->raw = 1;
	return node;
}

/**
 * quicklist_n_free - Deallocates the space related to @node.
 */
static void quicklist_n_free(struct quicklist_node *node)
{
	zfree(node->carry);
	zfree(node);
}

/**
 * quicklist_n_dup - Duplicate @node.
 * 
 * Note: the returned node's prev and next is undefined.
 */
static struct quicklist_node *quicklist_n_dup(const struct quicklist_node *node)
{
	struct quicklist_node *new = zmalloc(sizeof(*new));
	if (node->raw) {
		new->carry = zmalloc(node->raw_sz);
		memcpy(new->carry, node->carry, node->raw_sz);
	} else {
		struct quicklist_lzf *lzf = (struct quicklist_lzf *)node->carry;
		size_t carry_sz = sizeof(*lzf) + lzf->sz;
		new->carry = zmalloc(carry_sz);
		memcpy(new->carry, node->carry, carry_sz);
	}
	new->raw_sz = node->raw_sz;
	new->container = node->container;
	new->count = node->count;
	new->raw = node->raw;
	return new;
}

/**
 * It's used for compress, we will not compress a small node.
 */
#define QUICKLIST_MIN_COMPRESS_BYTES 48

/**
 * Minimum bytes of reduction should compress perform.
 */
#define QUICKLIST_MIN_COMPRESS_IMPROVE 8

/**
 * quicklist_n_compress_raw - Perform compress on @node.
 * 
 * Note: caller should make sure @node is a raw node. 
 * Note: node will keep uncompressed if it can't compress small enough.
 */
static void quicklist_n_compress_raw(struct quicklist_node *node)
{
	if (node->raw_sz < QUICKLIST_MIN_COMPRESS_BYTES)
		return;

	assert(QUICKLIST_MIN_COMPRESS_BYTES > QUICKLIST_MIN_COMPRESS_IMPROVE);
	size_t max_compressed_sz = node->raw_sz - QUICKLIST_MIN_COMPRESS_IMPROVE;
	struct quicklist_lzf *lzf = zmalloc(sizeof(*lzf) + max_compressed_sz);
	lzf->sz = lzf_compress(node->carry, node->raw_sz,
				lzf->compressed, max_compressed_sz);
	if (lzf->sz == 0) {
		zfree(lzf);
		return;
	}

	lzf = zrealloc(lzf, sizeof(*lzf) + lzf->sz);
	zfree(node->carry);
	node->carry = lzf;
	node->raw = 0;
}

/**
 * quicklist_n_decompress - Decompress @node if it is compressed.
 * 
 * Return: 1 if decompress performed, 0 if not.
 */
static int quicklist_n_decompress(struct quicklist_node *node)
{
	if (node->raw)
		return 0;
	
	void *raw = zmalloc(node->raw_sz);
	struct quicklist_lzf *lzf = (struct quicklist_lzf *)node->carry;
	assert(lzf_decompress(lzf->compressed, lzf->sz, raw, node->raw_sz)); 
	zfree(lzf);
	node->carry = raw;
	node->raw = 1;
	return 1;
}

/**
 * quicklist_n_debug_print - Print @node's information for debug.
 * @element: 1 for print element information, 0 for not
 */
static void quicklist_n_debug_print(struct quicklist_node *node, int element)
{
	printf("{raw-sz: %zu}\n", node->raw_sz);
	printf("{container: %d}\n", node->container);
	printf("{count: %d}\n", node->count);
	printf("{raw: %d}\n", node->raw);
	if (!node->raw) {
		struct quicklist_lzf *lzf = node->carry;
		printf("{lzf sz: %zu}\n", lzf->sz);
	}
	if (!element)
		return;
	
	printf("{carry}");
	int recompress = quicklist_n_decompress(node);

	if (node->container == QUICKLIST_NODE_CONTAINER_PLAIN)
		printf("{%s}\n", (char *)node->carry);
	else
		lpRepr(node->carry);

	if (recompress)
		quicklist_n_compress_raw(node);
}

/**
 * quicklist_n_add - Add node @new between adjacent nodes @prev and @next.
 */
static void quicklist_n_add(struct quicklist_node *new,
			    struct quicklist_node *prev,
			    struct quicklist_node *next)
{
	next->prev = new;
	new->next = next;
	new->prev = prev;
	prev->next = new;				
}

/**
 * quicklist_n_add_after - Add node @new after @node.
 */
static void quicklist_n_add_after(struct quicklist_node *new,
				  struct quicklist_node *node)
{
	quicklist_n_add(new, node, node->next);
}

/**
 * quicklist_n_add_before - Add node @new before @node.
 */
static void quicklist_n_add_before(struct quicklist_node *new,
				   struct quicklist_node *node)
{
	quicklist_n_add(new, node->prev, node);
}

/**
 * quicklist_n_remove - Remove a node from the partition.
 * @prev: node->prev
 * @next: node->next
 */
static void quicklist_n_remove(struct quicklist_node *prev,
			       struct quicklist_node *next)
{
	next->prev = prev;
	prev->next = next;				
}

/**
 * quicklist_n_remove_entry - Remove @node from the partition.
 */
static void quicklist_n_remove_entry(struct quicklist_node *node)
{
	quicklist_n_remove(node->prev, node->next);
}

/**
 * quicklist_n_move_after - Remove @from from the partition and add it after @to.
 */
static void quicklist_n_move_after(struct quicklist_node *from,
				   struct quicklist_node *to)
{
	quicklist_n_remove_entry(from);
	quicklist_n_add_after(from, to);
}

/**
 * quicklist_n_move_before - Remove @from from the partition and add it before @to.
 */
static void quicklist_n_move_before(struct quicklist_node *from,
				    struct quicklist_node *to)
{
	quicklist_n_remove_entry(from);
	quicklist_n_add_before(from, to);
}

/**
 * quicklist_n_allow_add_carry - Test if @node is allowed to add a new
 * element with size of @sz.
 * @fill: limitation of packed node
 * 
 * Return: 1 if addition is allowed, 0 if not.
 */
static int quicklist_n_allow_add_carry(struct quicklist_fill *fill,
				       struct quicklist_node *node, size_t sz)
{
	if (node->container == QUICKLIST_NODE_CONTAINER_PLAIN)
		return 0;

	if (node->count >= fill->pack_max_count)
		return 0;

	size_t new_sz = node->raw_sz + sz + QUICKLIST_PACK_ENTRY_SIZE_OVERHEAD;
	return new_sz <= fill->pack_max_size;
}

/**
 * quicklist_n_allow_merge - Test if node @a and @b is allowed to merge. 
 * @fill: limitation of packed node
 */
static int quicklist_n_allow_merge(struct quicklist_fill *fill,
				   struct quicklist_node *a,
				   struct quicklist_node *b) {
	if (a->container == QUICKLIST_NODE_CONTAINER_PLAIN)
		return 0;

	if (b->container == QUICKLIST_NODE_CONTAINER_PLAIN)
		return 0;

	assert(QUICKLIST_FILL_BITS * 2 <= 32);
	unsigned int new_count = a->count + b->count;
	size_t new_size = a->raw_sz + b->raw_sz - QUICKLIST_PACK_MERGE_SIZE_REDUCE;
	return new_size <= fill->pack_max_size && new_count <= fill->pack_max_count;
}

/**
 * quicklist_n_try_add_carry - Try to add a new element with value @value and
 * size @sz as @node's head or tail.
 * @tail: 0 for add as head, 1 for add as tail
 * @fill: limitation of packed node
 * @compress: 1 for compress @node after operation, 0 for not
 * 
 * Return: 1 if element is added, 0 if not.
 */
static int quicklist_n_try_add_carry(int tail, struct quicklist_fill *fill,
				struct quicklist_node *node, int compress,
				void *value, size_t sz)
{
	if(!quicklist_n_allow_add_carry(fill, node, sz))
		return 0;

	quicklist_n_decompress(node);

	if (tail)
        	node->carry = lpAppend(node->carry, value, sz);
	else 
        	node->carry = lpPrepend(node->carry, value, sz);

	node->raw_sz = lpBytes(node->carry);
	node->count++;
	if (compress)
		quicklist_n_compress_raw(node);
	return 1;
}

/**
 * quicklist_n_try_add_carry_head - Try to add a new element with
 * value @value and size @sz as @node's head.
 * @fill: limitation of packed node
 * @compress: 1 for compress @node after operation, 0 for not
 * 
 * Return: 1 if element is added, 0 if not.
 */
static int quicklist_n_try_add_carry_head(struct quicklist_fill *fill,
				struct quicklist_node *node, int compress,
				void *value, size_t sz)
{
	return quicklist_n_try_add_carry(0, fill, node, compress, value, sz);
}

/**
 * quicklist_n_try_add_carry_tail - Try to add a new element with
 * value @value and size @sz as @node's tail.
 * @fill: limitation of packed node
 * @compress: 1 for compress @node after operation, 0 for not
 * 
 * Return: 1 if element is added, 0 if not.
 */
static int quicklist_n_try_add_carry_tail(struct quicklist_fill *fill,
				struct quicklist_node *node, int compress,
				void *value, size_t sz)
{
	return quicklist_n_try_add_carry(1, fill, node, compress, value, sz);
}

/**
 * quicklist_n_del_element - Delete exactly @n elements from @node start
 * from @from and working towards to tail.
 * @compress: 1 for compress @node after operation, 0 for not
 * 
 * Note: caller should make sure @from and @n is valid.
 */
static void quicklist_n_del_element(struct quicklist_node *node, int compress,
					long from, long n)
{
	assert(0 <= from && from < node->count && n <= node->count - from);

	quicklist_n_decompress(node);
	node->carry = lpDeleteRange(node->carry, from, n);
	node->raw_sz = lpBytes(node->carry);
	node->count -= n;
	if (compress)
		quicklist_n_compress_raw(node); 
}

/**
 * quicklist_n_del_element_forward - Delete at most @n elements from @node
 * start from @from and working towards to tail.
 * @compress: 1 for compress @node after operation, 0 for not
 * 
 * Return: number of elements is deleted.
 * 
 * Note: caller should make sure @from and @n is valid.
 */
static long quicklist_n_del_element_forward(struct quicklist_node *node,
						int compress, long from, long n)
{
	assert( 0 <= from && from < node->count && n > 0);

	long deletable = node->count - from;
	long deleted = n <= deletable ? n : deletable;
	quicklist_n_del_element(node, compress, from, deleted);
	return deleted;
}

/**
 * quicklist_n_del_element_backward - Delete at most @n elements from @node
 * start from @from and working towards to head.
 * @compress: is @node requires compress
 * 
 * Return: number of elements is deleted.
 * 
 * Note: caller should make sure @from and @n is valid.
 */
static long quicklist_n_del_element_backward(struct quicklist_node *node,
						int compress, long from, long n)
{
	assert( 0 <= from && from < node->count-1 && n > 0);

	long deletable = from + 1;
	long deleted = n <= deletable ? n : deletable;
	quicklist_n_del_element(node, compress, deletable-deleted, deleted);
	return deleted;
}

/**
 * quicklist_n_split_raw - Split @node into two nodes, @node holds the
 * first @n elements, and the node holds the rest elements is returned.
 * 
 * Return: the node holds the elements split from @node.
 * 
 * Note: caller should make sure @node is a raw node and @n is valid.
 * TODO: liskpack should implement split.
 */
static struct quicklist_node *quicklist_n_split_raw(struct quicklist_node *node,
							long n)
{
	assert(node->raw && n < node->count);

	unsigned char *carry = zmalloc(node->raw_sz);
	memcpy(carry, node->carry, node->raw_sz);
	carry = lpDeleteRange(carry, 0, n);

	struct quicklist_node *new = zmalloc(sizeof(*new));
	new->carry = carry;
	new->raw_sz = lpBytes(carry);
	new->container = QUICKLIST_NODE_CONTAINER_PACKED;
	new->count = node->count - n;
	new->raw = 1;

	node->carry = lpDeleteRange(node->carry, n, new->count);
	node->raw_sz = lpBytes(node->carry);
	node->count = n;

	return new;
}

/**
 * quicklist_p_init - Init partition @p.
 * 
 * Note: free function is quicklist_p_free().
 */
static void quicklist_p_init(struct quicklist_partition *p,
			     struct quicklist_partition *prev,
			     struct quicklist_partition *next,
				int which, unsigned long capacity)
{
	struct quicklist_node *guard = zmalloc(sizeof(*guard));
	guard->next = guard;
	guard->prev = guard;

	p->which = which;
	p->capacity = capacity;
	p->guard = guard;
	p->prev = prev;
	p->next = next;
	p->length = 0;
}

/**
 * quicklist_p_free - Deallocates the space related to @p.
 */
static void quicklist_p_free(struct quicklist_partition *p)
{	
	struct quicklist_node *guard = p->guard;
	struct quicklist_node *node = guard->next;
	while (node != guard) {
		struct quicklist_node *next = node->next;
		quicklist_n_free(node);
		node = next;
	}
	zfree(p->guard);
	zfree(p);
}

/**
 * quicklist_p_copy - Copy nodes form @from to @to.
 */
static void quicklist_p_copy(struct quicklist_partition *to,
			     struct quicklist_partition *from)
{
	to->length = from->length;

	struct quicklist_node *guard = from->guard;
	struct quicklist_node *node = guard->next;
	while (node != guard) {
		struct quicklist_node *new = quicklist_n_dup(node);
		quicklist_n_add_before(new, to->guard);
		node = node->next;
	}
}

/**
 * quicklist_p_debug_print - Print @p's information for debug.
 * @i: the index of @p's first node
 * @element: 1 for print element information, 0 for not
 * 
 * Return: number of nodes in @p.
 */
static long quicklist_p_debug_print(struct quicklist_partition *p,
					long i, int element)
{
	printf("{which: %d}\n", p->which);
	printf("{length: %ld}\n", p->length);
	printf("{capacity: %ld}\n", p->capacity);
	
	struct quicklist_node *guard = p->guard;
	struct quicklist_node *node = guard->next;
	long count = 0;
	long node_count = 0;
	while (node != guard) {
		count += node->count;
		node_count++;
		printf("{node[%ld]}\n", i++);
		quicklist_n_debug_print(node, element);
		node = node->next;
	}
	printf("{length from node: %ld}\n", node_count);
	return count;
}

/**
 * quicklist_p_is_empty - Test @p is empty or not.
 * 
 * Return: 1 if @p is empty, 0 if not.
 */
static int quicklist_p_is_empty(struct quicklist_partition *p)
{
	return p->length == 0;
}

/**
 * quicklist_p_is_full - Test @p is full or not.
 * 
 * Return: 1 if @p is full, 0 if not.
 */
static int quicklist_p_is_full(struct quicklist_partition *p)
{
	return p->length >= p->capacity;
}

/**
 * quicklist_p_is_overflow - Test @p is overflow or not.
 * 
 * Return: 1 if @p is overflow, 0 if not.
 */
static int quicklist_p_is_overflow(struct quicklist_partition *p)
{
	return p->length > p->capacity;
}

/**
 * quicklist_p_is_head - Test @p is a quicklist's head partition or not.
 * 
 * Return: 1 if @p is a head partition, 0 if not.
 */
static int quicklist_p_is_head(struct quicklist_partition *p)
{
	return p->which == QUICKLIST_P_HEAD;
}

/**
 * quicklist_p_is_middle - Test @p is a quicklist's middle partition or not.
 * 
 * Return: 1 if @p is a middle partition, 0 if not.
 */
static int quicklist_p_is_middle(struct quicklist_partition *p)
{
	return p->which == QUICKLIST_P_MIDDLE;
}

/**
 * quicklist_p_is_tail - Test @p is a quicklist's tail partition or not.
 * 
 * Return: 1 if @p is a tail partition, 0 if not.
 */
static int quicklist_p_is_tail(struct quicklist_partition *p)
{
	return p->which == QUICKLIST_P_TAIL;
}

/**
 * quicklist_p_first - Get first node in @p.
 * 
 * Note: caller should make sure @p is not empty.
 */
static struct quicklist_node *quicklist_p_first(struct quicklist_partition *p)
{
	assert(!quicklist_p_is_empty(p));
	return p->guard->next;
}

/**
 * quicklist_p_last - Get last node in @p.
 * 
 * Note: caller should make sure @p is not empty.
 */
static struct quicklist_node *quicklist_p_last(struct quicklist_partition *p)
{
	assert(!quicklist_p_is_empty(p));
	return p->guard->prev;
}

/**
 * quicklist_p_add_node - Add raw node @new_raw to @p between @prev and @next.
 * @keep_raw: 1 for keep @new_raw a raw node, 0 for following partition
 * compress strategy.
 * 
 * Note: guard node is acceptable for @prev and @next.
 */
static void quicklist_p_add_node(struct quicklist_partition *p,
		struct quicklist_node *prev, struct quicklist_node *next,
		struct quicklist_node *new_raw, int keep_raw)
{
	quicklist_n_add(new_raw, prev, next);
	p->length++;
	if (!keep_raw && quicklist_p_is_middle(p))
		quicklist_n_compress_raw(new_raw);
}

/**
 * quicklist_p_move_forward - Move @from's tail to @to's head.
 * 
 * Note: caller should make sure @from is not empty.
 */
static void quicklist_p_move_forward(struct quicklist_partition *from,
				     struct quicklist_partition *to)
{
	struct quicklist_node *last = quicklist_p_last(from);
	quicklist_n_decompress(last);
	if (quicklist_p_is_middle(to))
		quicklist_n_compress_raw(last);
	
	quicklist_n_move_after(last, to->guard);
	from->length--;
	to->length++;
}

/**
 * __quicklist_p_move_forward - Move @from's tail to @to's head, and
 * monitor @node's partition.
 * @p: partation that @node belongs to before moving.
 * 
 * Return: partation that @node belongs to after moving.
 * 
 * Note: caller should make sure @from is not empty.
 * Note: if @node is raw, @node will keet raw, otherwise @node will following
 * partition compress strategy. This is to avoid unnecessary compression.
 */
struct quicklist_partition *__quicklist_p_move_forward(
					struct quicklist_partition *from,
					struct quicklist_partition *to,
					struct quicklist_partition *p,
					struct quicklist_node *node)
{
	struct quicklist_node *last = quicklist_p_last(from);
	quicklist_n_decompress(last);
	if (last != node && quicklist_p_is_middle(to))
		quicklist_n_compress_raw(last);
	
	quicklist_n_move_after(last, to->guard);
	from->length--;
	to->length++;

	if (last == node)
		return to;
	return p;
}

/**
 * quicklist_p_move_backward - Move @from's head to @to's tail.
 * 
 * Note: caller should make sure @from is not empty.
 */
static void quicklist_p_move_backward(struct quicklist_partition *from,
				      struct quicklist_partition *to)
{
	struct quicklist_node *first = quicklist_p_first(from);
	quicklist_n_decompress(first);
	if (quicklist_p_is_middle(to))
		quicklist_n_compress_raw(first);
	
	quicklist_n_move_before(first, to->guard);
	from->length--;
	to->length++;
}

/**
 * __quicklist_p_move_backward - Move @from's head to @to's tail, and
 * monitor @node's partition.
 * @p: partation that @node belongs to before moving.
 * 
 * Return: @node's partition after moving.
 * 
 * Note: caller should make sure @from is not empty.
 * Note: if @node is raw, @node will kept raw, otherwise @node will following
 * partition compress strategy. This is to avoid unnecessary compression.
 */
struct quicklist_partition *__quicklist_p_move_backward(
					struct quicklist_partition *from,
					struct quicklist_partition *to,
					struct quicklist_partition *p,
					struct quicklist_node *node)
{
	struct quicklist_node *first = quicklist_p_first(from);
	quicklist_n_decompress(first);
	if (first != node && quicklist_p_is_middle(to))
		quicklist_n_compress_raw(first);
	
	quicklist_n_move_before(first, to->guard);
	from->length--;
	to->length++;

	if (first == node)
		return to;
	return p;
}

/**
 * __quicklist_p_del_node - Remove @node from @p and free @node.
 * 
 * Note: don't forget to update bookmark before delete the node.
 */
static void __quicklist_p_del_node(struct quicklist_partition *p,
					struct quicklist_node *node)
{
	quicklist_n_remove_entry(node);
	quicklist_n_free(node);
	p->length--;
}

/**
 * quicklist_bm_clear - Clear all bookmarks in @quicklist.
 * 
 * Note: we do not shrink (realloc) @quicklist,
 * it is called just before free @quicklist.
 */
static void quicklist_bm_clear(struct quicklist *quicklist)
{
	while (quicklist->bookmark_count)
		zfree(quicklist->bookmarks[--quicklist->bookmark_count].name);
}

/**
 * quicklist_bm_find_by_name - Find bookmark with name @name.
 * @quicklist: the quicklist that the bookmark belongs to
 * 
 * Return: the bookmark is found, NULL if not found.
 */
static struct quicklist_bookmark *quicklist_bm_find_by_name(
				struct quicklist *quicklist, char *name) {
	for (int i = 0; i < quicklist->bookmark_count; i++) {
		if (strcmp(quicklist->bookmarks[i].name, name) == 0)
			return &quicklist->bookmarks[i];
	}
	return NULL;
}

/**
 * quicklist_bm_find_by_name - Find the node which the bookmark
 * with name @name marked.
 * @quicklist: the quicklist that the bookmark belongs to
 * 
 * Return: the node is found, NULL if not found.
 */
struct quicklist_node *quicklist_bm_find(struct quicklist *quicklist, char *name)
{
    struct quicklist_bookmark *bm = quicklist_bm_find_by_name(quicklist, name);
    if (!bm)
    	return NULL;
    return bm->node;
}

/**
 * __quicklist_bm_delete - Delete @bm.
 * @quicklist: the quicklist that the bookmark belongs to
 * 
 * Note: we do not shrink (realloc) @quicklist yet (to avoid resonance),
 * it may be re-used later (a call to realloc may NOP).
 */
static void __quicklist_bm_delete(struct quicklist *quicklist,
					struct quicklist_bookmark *bm) {
	int index = bm - quicklist->bookmarks;
	zfree(bm->name);
	quicklist->bookmark_count--;
	memmove(bm, bm+1, (quicklist->bookmark_count - index)* sizeof(*bm));
}

/**
 * quicklist_bm_delete - Delete the bookmark with name @name.
 * @quicklist: the quicklist that the bookmark belongs to
 * 
 * Return: 1 if the bookmark is found, 0 if not.
 */
int quicklist_bm_delete(struct quicklist *ql, char *name)
{
	struct quicklist_bookmark *bm = quicklist_bm_find_by_name(ql, name);
	if (!bm)
		return 0;
	__quicklist_bm_delete(ql, bm);
	return 1;
}

#define QUICKLIST_MAX_BM 15

/**
 * quicklist_bm_create - Create a new bookmark with name @name.
 * @ql_ref: reference of the quicklist that the bookmark belongs to
 * @node: the node marked
 * 
 * Return: 1 if the bookmark is created, 0 if not.
 * 
 * Note: the marked node will replaced with @node if the bookmark
 * is already exist.
 */
int quicklist_bm_create(struct quicklist **ql_ref, char *name,
			struct quicklist_node *node)
{
	struct quicklist *ql = *ql_ref;
	if (ql->bookmark_count >= QUICKLIST_MAX_BM)
		return 0;

	struct quicklist_bookmark *bm = quicklist_bm_find_by_name(ql, name);
	if (bm) {
		bm->node = node;
		return 1;
	}

	size_t bm_sz = (ql->bookmark_count+1) * sizeof(struct quicklist_bookmark);
	ql = zrealloc(ql, sizeof(struct quicklist) + bm_sz);
	*ql_ref = ql;
	ql->bookmarks[ql->bookmark_count].node = node;
	ql->bookmarks[ql->bookmark_count].name = zstrdup(name);
	ql->bookmark_count++;
	return 1;
}

/**
 * quicklist_bm_replace - Replace all the marked node @old with @new.
 * @quicklist: the quicklist that the bookmark belongs to
 * 
 * Return: the bookmark is found, NULL if not.
 * 
 * Note: caller should make sure @new is not NULL.
 */
static void quicklist_bm_replace(struct quicklist *quicklist,
			struct quicklist_node *old, struct quicklist_node *new)
{
	for (int i = 0; i < quicklist->bookmark_count; i++) {
		if (quicklist->bookmarks[i].node == old)
			quicklist->bookmarks[i].node = new;
	}
}

/**
 * quicklist_bm_move_next - Move all the marked node @node to its next.
 * @quicklist: the quicklist that the bookmark belongs to
 * 
 * Note: the bookmark will be deleted if @node is the last node.
 */
static void quicklist_bm_move_next(struct quicklist *quicklist,
					struct quicklist_node *node)
{
	struct quicklist_bookmark *bms = quicklist->bookmarks;
	for (int i = 0; i < quicklist->bookmark_count; i++) {
		if (bms[i].node == node) {
			bms[i].node = quicklist_next_for_bookmark(quicklist, node);
			if (!bms[i].node)
				__quicklist_bm_delete(quicklist, &bms[i]);
		}
	}
}

/**
 * quicklist_new_head - Create a new quicklist->head.
 * @raw_cap: capacity of head and tail partition
 * @compress_cap: capacity of middle partition
 */
static struct quicklist_partition *quicklist_new_head(long raw_cap, long compress_cap)
{
	struct quicklist_partition *head = zmalloc(sizeof(*head));
	struct quicklist_partition *middle = zmalloc(sizeof(*middle));
	struct quicklist_partition *tail = zmalloc(sizeof(*tail));
	quicklist_p_init(head, tail, middle, QUICKLIST_P_HEAD, raw_cap);
	quicklist_p_init(middle, head, tail, QUICKLIST_P_MIDDLE, compress_cap);
	quicklist_p_init(tail, middle, head, QUICKLIST_P_TAIL, raw_cap);
	return head;
}

/**
 * quicklist_new - Create a new quicklist.
 * @fill: the limitation for packed node, See quicklist_fill_new()
 * @compress: the compression strategy. No node will be compressed
 * if 0 is specified. Otherwise, it specifies the depth of nodes on either
 * side of the quicklist that will not be compressed.
 * 
 * Note: if (fill==0), every node is a plain node.
 * Note: free function is quicklist_free().
 */
struct quicklist *quicklist_new(int fill, int compress)
{
	long raw_cap;
	long compress_cap;
	if (compress == 0) {
		raw_cap = LONG_MAX;
		compress_cap = 0;
	} else {
		raw_cap = compress;
		compress_cap = LONG_MAX;
	}

	struct quicklist *quicklist = zmalloc(sizeof(*quicklist));
	quicklist->head = quicklist_new_head(raw_cap, compress_cap);
	quicklist->fill = quicklist_fill_new(fill);
	quicklist->count = 0;
	quicklist->bookmark_count = 0;
	return quicklist;
}

/**
 * quicklist_free - Deallocates the space related to @quicklist.
 */
void quicklist_free(struct quicklist *quicklist)
{
	struct quicklist_partition *p = quicklist->head;
	quicklist_p_free(p->prev);
	quicklist_p_free(p->next);
	quicklist_p_free(p);
	zfree(quicklist->fill);
	quicklist_bm_clear(quicklist);
	zfree(quicklist);
}

/**
 * quicklist_dup - Duplicate @quicklist.
 * 
 * Note: bookmark is not copied.
 */
struct quicklist *quicklist_dup(struct quicklist *quicklist)
{
	long raw_cap = quicklist->head->capacity;
	long compress_cap = quicklist->head->next->capacity;

	struct quicklist *new = zmalloc(sizeof(*new));
	new->head = quicklist_new_head(raw_cap, compress_cap);
	new->fill = quicklist_fill_dup(quicklist->fill);
	new->count = quicklist->count;
	new->bookmark_count = 0;

	quicklist_p_copy(new->head, quicklist->head);
	quicklist_p_copy(new->head->next, quicklist->head->next);
	quicklist_p_copy(new->head->prev, quicklist->head->prev);

	return new;
}

/**
 * quicklist_debug_print - Print @quicklist's information for debug.
 * @element: 1 for print element information, 0 for not.
 */
void quicklist_debug_print(struct quicklist *quicklist, int element)
{
	long i = 0;
	long count = 0;
	printf("{partition head}\n");
	count += quicklist_p_debug_print(quicklist->head, i, element);

	i += quicklist->head->length;
	printf("{partition middle}\n");
	count += quicklist_p_debug_print(quicklist->head->next, i, element);

	i += quicklist->head->next->length;
	printf("{partition tail}\n");
	count += quicklist_p_debug_print(quicklist->head->prev, i, element);

	printf("{count: %ld}\n", quicklist->count);
	printf("{count from partition: %ld}\n", count);
	printf("{fill pack_max_count: %u}\n", quicklist->fill->pack_max_count);
	printf("{fill pack_max_size: %u}\n", quicklist->fill->pack_max_size);
}

/**
 * quicklist_count - Get the number of elements in @quicklist.
 */
long quicklist_count(struct quicklist *quicklist)
{
	return quicklist->count;
}

/**
 * quicklist_node_count - Get the number of nodes in @quicklist.
 */
long quicklist_node_count(struct quicklist *quicklist)
{
	struct quicklist_partition *p = quicklist->head; 
	return p->length + p->prev->length + p->next->length;
}

/**
 * quicklist_first_node - Get the first node of @quicklist.
 * @p: if quicklist is not empty first node's partition will store in *@p,
 * otherwise, *@p will set to NULL.
 * @node: if quicklist is not empty first node will store in *@node,
 * otherwise, *@node will set to NULL.
 */
void quicklist_first_node(struct quicklist *quicklist,
		struct quicklist_partition **p, struct quicklist_node **node)
{
	if (quicklist->count == 0) {
		*p = NULL;
		*node = NULL;
		return;
	}

	struct quicklist_partition *_p = quicklist->head;
	while (_p->length == 0)
		_p = _p->next;
	*p = _p;
	*node = quicklist_p_first(_p);
}

/**
 * quicklist_last_node - Get the last node of @quicklist.
 * @p: if quicklist is not empty last node's partition will store in *@p,
 * otherwise, *@p will set to NULL.
 * @node: if quicklist is not empty last node will store in *@node,
 * otherwise, *@node will set to NULL.
 */
static void quicklist_last_node(struct quicklist *quicklist,
		struct quicklist_partition **p, struct quicklist_node **node)
{
	if (quicklist->count == 0) {
		*p = NULL;
		*node = NULL;
		return;
	}

	struct quicklist_partition *_p = quicklist->head->prev;
	while (_p->length == 0)
		_p = _p->prev;
	*p = _p;
	*node = quicklist_p_last(_p);
}

/**
 * quicklist_prev - Find the previous real node of @node, and store the
 * partition to *@prev_p, and store the node to *@prev_node. If previous node
 * is not found, NULL is stored to *@prev_p and *@prev_node.
 * @p: partition @node belongs to
 */
static void quicklist_prev(struct quicklist_partition *p,
				struct quicklist_node *node,
				struct quicklist_partition **prev_p,
				struct quicklist_node **prev_node)
{
	if (node->prev != p->guard) {
		*prev_p = p;
		*prev_node = node->prev;
		return;
	}

	struct quicklist_partition *p_prev = p->prev;
	while (!quicklist_p_is_tail(p_prev) && p_prev->length == 0)
		p_prev = p_prev->prev;
	if (quicklist_p_is_tail(p_prev)) {
		*prev_p = NULL;
		*prev_node = NULL;
	} else {
		*prev_p = p_prev;
		*prev_node = quicklist_p_last(p_prev);
	}
}

/**
 * quicklist_next - Find the next real node of @node, and store the
 * partition to *@next_p, and store the node to *@next_node. If next node
 * is not found, NULL is stored to *@next_p and *@next_node.
 * @p: partition @node belongs to
 */
void quicklist_next(struct quicklist_partition *p, struct quicklist_node *node,
	struct quicklist_partition **next_p, struct quicklist_node **next)
{
	if (node->next != p->guard) {
		*next_p = p;
		*next = node->next;
		return;
	}

	struct quicklist_partition *p_next = p->next;
	while (!quicklist_p_is_head(p_next) && p_next->length == 0)
		p_next = p_next->next;
	if (quicklist_p_is_head(p_next)) {
		*next_p = NULL;
		*next = NULL;
	} else {
		*next_p = p_next;
		*next = quicklist_p_first(p_next);
	}
}

/**
 * quicklist_next_for_bookmark - Find the next real node of @node.
 * @quicklist: the quicklist that @node belongs to
 * 
 * Return: the next node, NULL if @node is the last node.
 */
struct quicklist_node *quicklist_next_for_bookmark(struct quicklist *quicklist,
						struct quicklist_node *node)
{
	struct quicklist_partition *head = quicklist->head;
	struct quicklist_partition *middle = head->next;
	struct quicklist_partition *tail = middle->next;

	struct quicklist_partition *next_p;
	struct quicklist_node *next;

	if (node->next == head->guard) {
		quicklist_next(head, node, &next_p, &next);
		return next;
	}

	if (node->next == middle->guard) {
		quicklist_next(middle, node, &next_p, &next);
		return next;
	}

	if (node->next == tail->guard)
		return NULL;

	return node->next;
}

/**
 * quicklist_fix_compress - Keep @quicklist fit compress strategy.
 */
static void quicklist_fix_compress(struct quicklist *quicklist) {
	struct quicklist_partition *head = quicklist->head;
	struct quicklist_partition *middle = head->next;
	struct quicklist_partition *tail = middle->next;

	while (!quicklist_p_is_full(head) && !quicklist_p_is_empty(middle))
		quicklist_p_move_backward(middle, head);

	while (!quicklist_p_is_full(head) && quicklist_p_is_overflow(tail))
		quicklist_p_move_backward(tail, head);

	while (!quicklist_p_is_full(tail) && !quicklist_p_is_empty(middle))
		quicklist_p_move_forward(middle, tail);

	while (!quicklist_p_is_full(tail) && quicklist_p_is_overflow(head))
		quicklist_p_move_forward(head, tail);

	while (quicklist_p_is_overflow(head))
		quicklist_p_move_forward(head, middle);

	while (quicklist_p_is_overflow(tail))
		quicklist_p_move_backward(tail, middle);
}

/**
 * quicklist_p_add_node_head - Add @raw_node to @quicklist as @quicklist's head.
 */
static void quicklist_add_node_head(struct quicklist *quicklist,
					struct quicklist_node *raw_node)
{
	struct quicklist_partition *head = quicklist->head;
	struct quicklist_node *head_guard = head->guard;
	quicklist_p_add_node(head, head_guard, head_guard->next, raw_node, 1);
	quicklist_fix_compress(quicklist);
	quicklist->count += raw_node->count;
}

/**
 * quicklist_p_add_node_tail - Add @raw_node to @quicklist as @quicklist's tail.
 */
static void quicklist_add_node_tail(struct quicklist *quicklist,
					struct quicklist_node *raw_node)
{
	struct quicklist_partition *tail = quicklist->head->prev;
	struct quicklist_node *tail_guard = tail->guard;
	quicklist_p_add_node(tail, tail_guard->prev, tail_guard, raw_node, 1);
	quicklist_fix_compress(quicklist);
	quicklist->count += raw_node->count;
}

/**
 * quicklist_push_head - Push a new element with value @value and
 * size @sz as @quicklist's head.
 */
void quicklist_push_head(struct quicklist *quicklist, void *value, size_t sz)
{
	struct quicklist_fill *fill = quicklist->fill;

	struct quicklist_partition *first_p;
	struct quicklist_node *first;
	quicklist_first_node(quicklist, &first_p, &first);
	if (first && quicklist_n_try_add_carry_head(fill, first, 0, value, sz)) {
		quicklist->count++;
		return;
	}
	
	struct quicklist_node *new_raw = quicklist_n_new_raw(fill, value, sz);
	quicklist_add_node_head(quicklist, new_raw);
}

/**
 * quicklist_push_tail - Push a new element with value @value and
 * size @sz as @quicklist's tail.
 */
void quicklist_push_tail(struct quicklist *quicklist, void *value, size_t sz)
{
	struct quicklist_fill *fill = quicklist->fill;

	struct quicklist_partition *last_p;
	struct quicklist_node *last;
	quicklist_last_node(quicklist, &last_p, &last);
	if (last && quicklist_n_try_add_carry_tail(fill, last, 0, value, sz)) {
		quicklist->count++;
		return;
	}
	struct quicklist_node *new_raw = quicklist_n_new_raw(fill, value, sz);
	quicklist_add_node_tail(quicklist, new_raw);
}

/**
 * __quicklist_fix_compress - Keep @quicklist fit compress strategy, and
 * monitor @node's partition.
 * @p: partation that @node belongs to before fix.
 * 
 * Return: @node's partition after fix.
 * 
 * Note: if @node is raw, @node will kept raw, otherwise keeps @node following
 * partition compress strategy.
 */
static struct quicklist_partition *__quicklist_fix_compress(
					struct quicklist *quicklist,
					struct quicklist_partition *p,
					struct quicklist_node *node)
{
	struct quicklist_partition *head = quicklist->head;
	struct quicklist_partition *middle = head->next;
	struct quicklist_partition *tail = middle->next;

	while (!quicklist_p_is_full(head) && !quicklist_p_is_empty(middle))
		p = __quicklist_p_move_backward(middle, head, p, node);

	while (!quicklist_p_is_full(head) && quicklist_p_is_overflow(tail))
		p = __quicklist_p_move_backward(tail, head, p, node);

	while (!quicklist_p_is_full(tail) && !quicklist_p_is_empty(middle))
		p = __quicklist_p_move_forward(middle, tail, p, node);

	while (!quicklist_p_is_full(tail) && quicklist_p_is_overflow(head))
		p = __quicklist_p_move_forward(head, tail, p, node);

	while (quicklist_p_is_overflow(head))
		p = __quicklist_p_move_forward(head, middle, p, node);

	while (quicklist_p_is_overflow(tail))
		p = __quicklist_p_move_backward(tail, middle, p, node);
	
	return p;
}

/**
 * __quicklist_merge - Merge node @prev and @next.
 * @prev_p: partition @prev belongs to
 * @next_p: partition @next belongs to
 * @forward: 1 for merged @prev to @next as @next's head, @prev will be deleted;
 * 0 for merged @next to @prev as @prev's tail, @next will be deleted.
 * @keep_raw: 1 for keep the remain node raw, 0 for following partition
 * compress strategy.
 */
static void __quicklist_merge(struct quicklist *quicklist,
	struct quicklist_partition *prev_p, struct quicklist_node *prev,
	struct quicklist_partition *next_p, struct quicklist_node *next,
	int forward, int keep_raw)
{
	struct quicklist_partition *remain_p, *removed_p;
	struct quicklist_node *remain, *removed;
	if (forward) {
		remain_p = next_p;
		remain = next;
		removed_p = prev_p;
		removed = prev;
	} else {
		remain_p = prev_p;
		remain = prev;
		removed_p = next_p;
		removed = next;
	}

	quicklist_n_decompress(prev);
	quicklist_n_decompress(next);

	remain->carry = lpMerge((unsigned char **)&prev->carry,
				(unsigned char **)&next->carry);
	removed->carry = NULL;
	
	remain->raw_sz = lpBytes(remain->carry);
	remain->count += removed->count;

	if (!keep_raw && quicklist_p_is_middle(remain_p))
		quicklist_n_compress_raw(remain);

	quicklist_bm_replace(quicklist, removed, remain);
	__quicklist_p_del_node(removed_p, removed);
}

/**
 * __quicklist_try_merge - Try to merge @prev and @next.
 * @prev_p: partition @prev belongs to
 * @next_p: partition @next belongs to
 * @forward: 1 for try to merged @prev to @next as @next's head, @prev will
 * be deleted after merge; 0 for try to merged @next to @prev as @prev's
 * tail, @next will be freed after merge.
 * @keep_raw: 1 for keep the remain node raw, 0 for following partition
 * compress strategy.
 * 
 * Note: we will not try to decompress @prev nor @next if merge is not performed.
 */
static int __quicklist_try_merge(struct quicklist *quicklist,
	struct quicklist_partition *prev_p, struct quicklist_node *prev,
	struct quicklist_partition *next_p, struct quicklist_node *next,
	int forward, int keep_raw)
{
	if (!quicklist_n_allow_merge(quicklist->fill, prev, next))
		return 0;

	__quicklist_merge(quicklist, prev_p, prev, next_p, next, forward, keep_raw);
	return 1;
}

/**
 * __quicklist_try_merge_prev_raw - Try to merge *@node with its previous node.
 * @p: *@p is the partition @node belongs to, and *@p will change to the remain
 * node's partition after merge.
 * @node: *@node will change to the remain node after merge.
 * 
 * Note: caller should make sure *@node is a raw node, and *@node is guarantee
 * a raw node after the operation.
 */
static void __quicklist_try_merge_prev_raw(struct quicklist *quicklist,
						struct quicklist_partition **p,
				    		struct quicklist_node **node)
{
	struct quicklist_partition *prev_p;
	struct quicklist_node *prev;
	quicklist_prev(*p, *node, &prev_p, &prev);
	if (!prev)
		return;

	if (quicklist_p_is_middle(prev_p)) {
		__quicklist_try_merge(quicklist, prev_p, prev, *p, *node, 1, 1);
	} else if (__quicklist_try_merge(quicklist, prev_p, prev, *p, *node, 0, 1)){
		*p = prev_p;
		*node = prev;
	}
}

/**
 * __quicklist_try_merge_prev - Try to merge @node with its previous node.
 * @p: partition @node belongs to
 * 
 * Note: if compress performed, the remain node will following partition
 * compress strategy. Otherwise, nothing happens.
 */
static void __quicklist_try_merge_prev(struct quicklist *quicklist,
					struct quicklist_partition *p,
				    	struct quicklist_node *node)
{
	struct quicklist_partition *prev_p;
	struct quicklist_node *prev;
	quicklist_prev(p, node, &prev_p, &prev);
	if (!prev)
		return;

	int forward = quicklist_p_is_middle(prev_p);
	__quicklist_try_merge(quicklist, prev_p, prev, p, node, forward, 0);
}

/**
 * __quicklist_try_merge_next_raw - Try to merge *@node with its next node.
 * @p: *@p is the partition @node belongs to, and *@p will change to the remain
 * node's partition after merge.
 * @node: *@node will change to the remain node.
 * 
 * Note: caller should make sure *@node is a raw node, and *@node is guarantee
 * a raw node after the operation.
 */
static void __quicklist_try_merge_next_raw(struct quicklist *quicklist,
						struct quicklist_partition **p,
				    		struct quicklist_node **node)
{
	struct quicklist_partition *next_p;
	struct quicklist_node *next;
	quicklist_next(*p, *node, &next_p, &next);
	if (!next)
		return;

	if (quicklist_p_is_middle(next_p)) {
		__quicklist_try_merge(quicklist, *p, *node, next_p, next, 0, 1);
	} else if (__quicklist_try_merge(quicklist, *p, *node, next_p, next, 1, 1)){
		*p = next_p;
		*node = next;
	}
}

/**
 * __quicklist_try_merge_next - Try to merge @node with its next node.
 * @p: partition @node belongs to
 * 
 * Note: if compress performed, the remain node will following partition
 * compress strategy. Otherwise, nothing happens.
 */
static void __quicklist_try_merge_next(struct quicklist *quicklist,
					struct quicklist_partition *p,
				    	struct quicklist_node *node)
{
	struct quicklist_partition *next_p;
	struct quicklist_node *next;
	quicklist_next(p, node, &next_p, &next);
	if (!next)
		return;

	int forward = !quicklist_p_is_middle(next_p);
	__quicklist_try_merge(quicklist, p, node, next_p, next, forward, 0);
}

/**
 * __quicklist_fix_merge_1 - Fix merge after delete some elements from @node.
 * @p: partition @node belongs to
 * 
 * Note: caller should make sure @node is a raw node, and @node will following
 * partition compress strategy after operation.
 */
static void __quicklist_fix_merge_1(struct quicklist *quicklist,
					struct quicklist_partition *p,
				    	struct quicklist_node *node)
{
	__quicklist_try_merge_prev_raw(quicklist, &p, &node);
	__quicklist_try_merge_next_raw(quicklist, &p, &node);
	if (quicklist_p_is_middle(p))
		quicklist_n_compress_raw(node);
}

/**
 * __quicklist_fix_merge_2 - Fix merge after delete some elements
 * from @prev and @next.
 * @prev_p: partition @prev belongs to.
 * @next_p: partition @next belongs to.
 * 
 * Note: caller should make sure @prev and @next are raw node, @prev and @next
 * will following partition compress strategy after operation.
 */
static void __quicklist_fix_merge_2(struct quicklist *quicklist,
					struct quicklist_partition *prev_p,
					struct quicklist_node *prev,
					struct quicklist_partition *next_p,
					struct quicklist_node *next)
{
	__quicklist_try_merge_prev_raw(quicklist, &prev_p, &prev);
	__quicklist_try_merge_next_raw(quicklist, &next_p, &next);
	int forward = quicklist_p_is_middle(prev_p);
	if (__quicklist_try_merge(quicklist, prev_p, prev, next_p, next, forward, 0))
		return;
	if (quicklist_p_is_middle(prev_p))
		quicklist_n_compress_raw(prev);
	if (quicklist_p_is_middle(next_p))
		quicklist_n_compress_raw(next);
}

/**
 * quicklist_del_forward - Delete exactly @n elements from @quicklist start
 * from @from and working towards to tail.
 * 
 * Note: caller should make sure @from and @n is valid.
 */
static void quicklist_del_forward(struct quicklist *quicklist, long from, long n)
{
	long deleted = n;
	
	struct quicklist_partition *p;
	struct quicklist_node *node;
	quicklist_first_node(quicklist, &p, &node);
	
	while (from >= node->count) {
		from -= node->count;
		quicklist_next(p, node, &p, &node);
	}

	struct quicklist_partition *first_p = NULL;
	struct quicklist_node *first = NULL;
	if (from != 0) {
		first_p = p;
		first = node;
		n -= quicklist_n_del_element_forward(node, 0, from, n);
		quicklist_next(p, node, &p, &node);
	}

	struct quicklist_partition *last_p = NULL;
	struct quicklist_node *last = NULL;
	while (n > 0) {
		if (n < node->count) {
			last_p = p;
			last = node;
			quicklist_n_del_element(node, 0, 0, n);
			break;
		}

		struct quicklist_partition *next_p;
		struct quicklist_node *next;
		quicklist_next(p, node, &next_p, &next);

		n -= node->count;
		quicklist_bm_move_next(quicklist, node);
		__quicklist_p_del_node(p, node);

		p = next_p;
		node = next;
	}

	if (first && last)
		__quicklist_fix_merge_2(quicklist, first_p, first, last_p, last);
	else if (first)
		__quicklist_fix_merge_1(quicklist, first_p, first);
	else if (last)
		__quicklist_fix_merge_1(quicklist, last_p, last);
	else if (node)
		__quicklist_try_merge_prev(quicklist, p, node);

	quicklist_fix_compress(quicklist);

	quicklist->count -= deleted;
}

/**
 * quicklist_del_backward - Delete exactly @n elements from @quicklist start
 * from @from and working towards to head.
 * 
 * Note: caller should make sure @from and @n is valid.
 */
static void quicklist_del_backward(struct quicklist *quicklist, long from, long n)
{
	long deleted = n;

	struct quicklist_partition *p;
	struct quicklist_node *node;
	quicklist_last_node(quicklist, &p, &node);

	long i = quicklist->count - node->count;
	while (i > from) {
		quicklist_prev(p, node, &p, &node);
		i -= node->count;
	}

	struct quicklist_partition *last_p = NULL;
	struct quicklist_node *last = NULL;
	if (from-i != node->count-1) {
		last_p = p;
		last = node;
		n -= quicklist_n_del_element_backward(node, 0, from-i, n);
		quicklist_prev(p, node, &p, &node);
	}

	struct quicklist_partition *first_p = NULL;
	struct quicklist_node *first = NULL;
	while (n > 0) {
		if (n < node->count) {
			first_p = p;
			first = node;
			quicklist_n_del_element(node, 0,  node->count-n, n);
			break;
		}

		struct quicklist_partition *prev_p;
		struct quicklist_node *prev;
		quicklist_prev(p, node, &prev_p, &prev);
		
		n -= node->count;
		quicklist_bm_move_next(quicklist, node);
		__quicklist_p_del_node(p, node);
		
		p = prev_p;
		node = prev;
	}

	if (first && last)
		__quicklist_fix_merge_2(quicklist, first_p, first, last_p, last);
	else if (first)
		__quicklist_fix_merge_1(quicklist, first_p, first);
	else if (last)
		__quicklist_fix_merge_1(quicklist, last_p, last);
	else if (node)
		__quicklist_try_merge_next(quicklist, p, node);

	quicklist_fix_compress(quicklist);

	quicklist->count -= deleted;
}

/** 
 * quicklist_del - Delete at most @n elements from @quicklist start from @from.
 * 
 * Return: number of elements is deleted.
 * 
 * Note: negative @from is acceptable, see quicklist_transform_index().
 * Note: caller should make sure @n is valid.
 */
long quicklist_del(struct quicklist *quicklist, long from, long n)
{
	assert(n > 0);

	from = quicklist_transform_index(quicklist->count, from);
	if (from < 0)
		return 0;

	long deletable = quicklist->count - from;

	if (n >= deletable)
		n = deletable;

	if (from <= deletable - n)
		quicklist_del_forward(quicklist, from, n);
	else
		quicklist_del_backward(quicklist, from + n - 1, n);
	
	return n;
}

/**
 * quicklist_append_plain - Add a plain node with value @value and size @sz
 * as @quicklist's tail.
 * 
 * Note: this function takes over ownership of @value from the caller.
 */
void quicklist_append_plain(struct quicklist *quicklist, void *value, size_t sz)
{
	struct quicklist_node *node = zmalloc(sizeof(*node));
	node->carry = value;
	node->raw_sz = sz;
	node->container = QUICKLIST_NODE_CONTAINER_PLAIN;
	node->count = 1;
	node->raw = 1;
	quicklist_add_node_tail(quicklist, node);
}

/**
 * quicklist_append_listpack - Add a packed node with listpack @lp
 * as @quicklist's tail.
 * 
 * Note: this function takes over ownership of @lp from the caller.
 */
void quicklist_append_listpack(struct quicklist *quicklist, unsigned char *lp)
{
	struct quicklist_node *node = zmalloc(sizeof(*node));
	node->carry = lp;
	node->raw_sz = lpBytes(lp);
	node->container = QUICKLIST_NODE_CONTAINER_PACKED;
	node->count = lpLength(lp);
	node->raw = 1;
	quicklist_add_node_tail(quicklist, node);
}

/**
 * quicklist_iter_get_element - Get the element that @iter point to.
 * @elem: the element is stored to *@elem
 * 
 * Note: caller should make sure @iter is normal.
 */
void quicklist_iter_get_element(struct quicklist_iter *iter,
				struct quicklist_element *elem)
{
	assert(iter->status == QUICKLIST_ITER_STATUS_NORMAL);
	
	struct quicklist_node *node = iter->raw_node;
	if (node->container == QUICKLIST_NODE_CONTAINER_PLAIN) {
		elem->value = node->carry;
		elem->sz = node->raw_sz;
	} else {
		unsigned int sz = 0;
		elem->value = lpGetValue(iter->lp_element, &sz, &elem->longval);
		elem->sz = sz;
	}
}

/**
 * quicklist_iter_set - Set @iter point to the @ith element of @node.
 * @p: partition @node belongs to
 * 
 * Note: this function does nothing to the element that iter previous point to.
 * Note: caller should make sure @i is valid.
 */
static void quicklist_iter_set(struct quicklist_iter *iter,
				struct quicklist_partition *p,
				struct quicklist_node *node, long i)
{
	iter->p = p;
	quicklist_n_decompress(node);
	iter->raw_node = node;
	iter->offset = i;
	if (node->container == QUICKLIST_NODE_CONTAINER_PACKED)
		iter->lp_element = lpSeek(node->carry, i);
}

/**
 * quicklist_iter_set_node_first - Set @iter point to the first element of @node.
 * @p: partition @node belongs to
 * 
 * Note: this function does nothing to the element that iter previous point to.
 */
static void quicklist_iter_set_node_first(struct quicklist_iter *iter,
		struct quicklist_partition *p, struct quicklist_node *node)
{
	quicklist_iter_set(iter, p, node, 0);
}

/**
 * quicklist_iter_set_node_last - Set @iter point to the last element of @node.
 * @p: partition @node belongs to
 * 
 * Note: this function does nothing to the element that iter previous point to.
 */
static void quicklist_iter_set_node_last(struct quicklist_iter *iter,
		struct quicklist_partition *p, struct quicklist_node *node)
{
	quicklist_iter_set(iter, p, node, node->count-1);
}

/**
 * quicklist_iter_new_from_head - Create a new quicklist_iter for @quicklist,
 * search target element from head towards to tail.
 * @i: index of the element that the iterator point to
 * @forward: 1 for iterate forward, 0 for iterate backward
 * 
 * Note: caller should make sure @ith element is exist.
 */
static struct quicklist_iter *quicklist_iter_new_from_head(
			struct quicklist *quicklist, long i, int forward)
{
	struct quicklist_iter *iter = zmalloc(sizeof(*iter));
	iter->status = QUICKLIST_ITER_STATUS_NORMAL;
	iter->forward = forward;
	iter->quicklist = quicklist;

	struct quicklist_partition *p;
	struct quicklist_node *node;
	quicklist_first_node(iter->quicklist, &p, &node);

	while (i >= node->count) {
		i -= node->count;
		quicklist_next(p, node, &p, &node);
	}

	quicklist_iter_set(iter, p, node, i);
	return iter;
}

/**
 * quicklist_iter_new_from_tail - Create a new quicklist_iter for @quicklist,
 * search target element from tail towards to head.
 * @i: index of the element that the iterator point to
 * @forward: 1 for iterate forward, 0 for iterate backward
 * 
 * Note: caller should make sure @ith element is exist.
 */
static struct quicklist_iter *quicklist_iter_new_from_tail(
			struct quicklist *quicklist, long i, int forward)
{
	struct quicklist_iter *iter = zmalloc(sizeof(*iter));
	iter->status = QUICKLIST_ITER_STATUS_NORMAL;
	iter->forward = forward;
	iter->quicklist = quicklist;

	struct quicklist_partition *p;
	struct quicklist_node *node;
	quicklist_last_node(quicklist, &p, &node);

	long j = quicklist->count - node->count;
	while (j > i) {
		quicklist_prev(p, node, &p, &node);
		j -= node->count;
	}

	quicklist_iter_set(iter, p, node, i-j);
	return iter;
}

/**
 * quicklist_iter_new - Create a new quicklist_iter for @quicklist.
 * @i: index of the element that the iterator point to
 * @forward: 1 for iterate forward, 0 for iterate backward
 * 
 * Return: created quicklist_iter, NULL if @ith element is exist.
 * 
 * Note: free function is quicklist_iter_free().
 * Note: negative @i is acceptable, see quicklist_transform_index().
 */
struct quicklist_iter *quicklist_iter_new(struct quicklist *quicklist,
						long i, int forward)
{
	i = quicklist_transform_index(quicklist->count, i);
	if (i < 0)
		return NULL;
	
	if (i < quicklist->count/2)
		return quicklist_iter_new_from_head(quicklist, i, forward);
	
	return quicklist_iter_new_from_tail(quicklist, i, forward);
}

/**
 * quicklist_iter_new_ahead - Create a new ahead quicklist_iter for @quicklist.
 * @i: index of the element that the iterator point to
 * @forward: 1 for iterate forward, 0 for iterate backward
 * 
 * Return: created quicklist_iter, NULL if @ith element is exist.
 * 
 * Note: free function is quicklist_iter_free().
 * Note: negative @i is acceptable, see quicklist_transform_index().
 * Note: returned quicklist_iter is a ahead iterator, is not valid until
 * quicklist_iter_next() is called. See QUICKLIST_ITER_STATUS_AHEAD.
 */
struct quicklist_iter *quicklist_iter_new_ahead(struct quicklist *quicklist,
						long i, int forward)
{
	/* skip quicklist_transform_index() */
	struct quicklist_iter *iter = quicklist_iter_new(quicklist, i, forward);
	if (iter != NULL)
		iter->status = QUICKLIST_ITER_STATUS_AHEAD;
	return iter;
}

/**
 * quicklist_iter_free - Deallocates the space related to @iter.
 */
void quicklist_iter_free(struct quicklist_iter *iter)
{
	if (iter->status != QUICKLIST_ITER_STATUS_COMPLETE && quicklist_p_is_middle(iter->p))
		quicklist_n_compress_raw(iter->raw_node);
	
	zfree(iter);
}

/**
 * quicklist_iter_debug_print - Print @iter's information for debug.
 */
void quicklist_iter_debug_print(struct quicklist_iter *iter)
{
	printf("{iter}\n");
	printf("{status: %d}\n", iter->status);
	printf("{forward: %d}\n", iter->forward);
	printf("{p: %d}\n", iter->p->which);
	printf("{offset: %d}\n", iter->offset);
}

/**
 * quicklist_iter_move_to - Move @iter to @node and point to the
 * first or last element base on @iter->forward.
 * @p: partition @node belongs to
 */
static void quicklist_iter_move_to(struct quicklist_iter *iter,
		struct quicklist_partition *p, struct quicklist_node *node)
{
	if (quicklist_p_is_middle(iter->p))
		quicklist_n_compress_raw(iter->raw_node);

	if (iter->forward)
		quicklist_iter_set_node_first(iter, p, node);
	else
		quicklist_iter_set_node_last(iter, p, node);
}

/**
 * quicklist_iter_pack_next_forward - Move @iter to next element of
 * node @iter->raw_node working towards to tail.
 * 
 * Note: caller should make sure next element is exist.
 */
static void quicklist_iter_pack_next_forward(struct quicklist_iter *iter)
{
	iter->offset++;
	iter->lp_element = lpNext(iter->raw_node->carry, iter->lp_element);
}

/**
 * quicklist_iter_pack_next_backward - Move @iter to next element of
 * node @iter->raw_node working towards to head.
 * 
 * Note: caller should make sure next element is exist.
 */
static void quicklist_iter_pack_next_backward(struct quicklist_iter *iter)
{
	iter->offset--;
	iter->lp_element = lpPrev(iter->raw_node->carry, iter->lp_element);
}

/**
 * quicklist_iter_complete - Complete @iter and disable it for further use.
 */
static void quicklist_iter_complete(struct quicklist_iter *iter)
{
	if (quicklist_p_is_middle(iter->p))
		quicklist_n_compress_raw(iter->raw_node);
	iter->status = QUICKLIST_ITER_STATUS_COMPLETE;
}

/**
 * __quicklist_iter_next_forward - Move @iter to next element working
 * towards to tail.
 * 
 * Return: 1 if next element exists, 0 if not.
 * 
 * Note: @iter is not available for further use if 0 is returned.
 */
static int __quicklist_iter_next_forward(struct quicklist_iter *iter)
{
	if (iter->offset+1 < iter->raw_node->count) {
		quicklist_iter_pack_next_forward(iter);
		return 1;
	}
	
	struct quicklist_partition *next_p;
	struct quicklist_node *next_node;
	quicklist_next(iter->p, iter->raw_node, &next_p, &next_node);
	if (next_node == NULL) {
		quicklist_iter_complete(iter);
		return 0;
	}
	quicklist_iter_move_to(iter, next_p, next_node);
	return 1;
}

/**
 * __quicklist_iter_next_backward - Move @iter to next element working
 * towards to head.
 * 
 * Return: 1 if next element exists, 0 if not.
 * 
 * Note: @iter is not available for further use if 0 is returned.
 */
static int __quicklist_iter_next_backward(struct quicklist_iter *iter)
{
	if (iter->offset > 0) {
		quicklist_iter_pack_next_backward(iter);
		return 1;
	}
	
	struct quicklist_partition *prev_p;
	struct quicklist_node *prev_node;
	quicklist_prev(iter->p, iter->raw_node, &prev_p, &prev_node);
	if (prev_node == NULL) {
		quicklist_iter_complete(iter);
		return 0;
	}
	quicklist_iter_move_to(iter, prev_p, prev_node);
	return 1;
}

/**
 * quicklist_iter_next - Move @iter to next element base on @iter->forward.
 * 
 * Return: 1 if next element exist, 0 if not.
 * 
 * Note: @iter is not available for further use if 0 is returned.
 */
int quicklist_iter_next(struct quicklist_iter *iter)
{
	switch (iter->status) {
	case QUICKLIST_ITER_STATUS_COMPLETE:
		return 0;
	case QUICKLIST_ITER_STATUS_AHEAD:
		iter->status = QUICKLIST_ITER_STATUS_NORMAL;
		return 1;
	}

	if (iter->forward)
		return __quicklist_iter_next_forward(iter);
	else
		return __quicklist_iter_next_backward(iter);
}

/**
 * quicklist_iter_del_single - Delete @iter->raw_node and move @iter to
 * next element base on @iter->forward.
 * 
 * Note: if next element is not exist, @iter's status will change to
 * QUICKLIST_ITER_STATUS_COMPLETE; otherwise, @iter's status will change to
 * QUICKLIST_ITER_STATUS_AHEAD.
 */
static void quicklist_iter_del_single(struct quicklist_iter *iter)
{
	struct quicklist *quicklist = iter->quicklist;

	struct quicklist_partition *prev_p, *next_p;
	struct quicklist_node *prev, *next;
	quicklist_prev(iter->p, iter->raw_node, &prev_p, &prev);
	quicklist_next(iter->p, iter->raw_node, &next_p, &next);

	quicklist_bm_move_next(quicklist, iter->raw_node);
	__quicklist_p_del_node(iter->p, iter->raw_node);
	iter->quicklist->count--;

	if (!prev && !next)
		goto iter_complete;

	if (!prev) {
		if (!iter->forward)
			goto iter_complete;

		next_p = __quicklist_fix_compress(quicklist, next_p, next);
		quicklist_iter_set_node_first(iter, next_p, next);
		goto iter_ahead;
	}

	if (!next) {
		if (iter->forward)
			goto iter_complete;

		prev_p = __quicklist_fix_compress(quicklist, prev_p, prev);
		quicklist_iter_set_node_last(iter, prev_p, prev);
		goto iter_ahead;
	}

	long i = prev->count;
	if (__quicklist_try_merge(quicklist, prev_p, prev, next_p, next, 0, 1)) {
		prev_p = __quicklist_fix_compress(quicklist, prev_p, prev);
		if (iter->forward)
			quicklist_iter_set(iter, prev_p, prev, i);
		else
			quicklist_iter_set(iter, prev_p, prev, i-1);
		goto iter_ahead;
	}

	if (iter->forward) {
		next_p = __quicklist_fix_compress(quicklist, next_p, next);
		quicklist_iter_set_node_first(iter, next_p, next);
		goto iter_ahead;
	}

	prev_p = __quicklist_fix_compress(quicklist, prev_p, prev);
	quicklist_iter_set_node_last(iter, prev_p, prev);
	goto iter_ahead;

iter_complete:
	iter->status = QUICKLIST_ITER_STATUS_COMPLETE;
	return;

iter_ahead:
	iter->status = QUICKLIST_ITER_STATUS_AHEAD;
}

/**
 * quicklist_iter_del_multi - Delete the element that @iter point to and
 * move @iter to next element base on @iter->forward.
 * 
 * Note: if next element is not exist, @iter's status will change to
 * QUICKLIST_ITER_STATUS_COMPLETE; otherwise, @iter's status will change to
 * QUICKLIST_ITER_STATUS_AHEAD.
 */
static void quicklist_iter_del_multi(struct quicklist_iter *iter)
{
	struct quicklist *quicklist = iter->quicklist;
	struct quicklist_partition *p = iter->p;
	struct quicklist_node *node = iter->raw_node;

	node->carry = lpDelete(node->carry, iter->lp_element, NULL);
	node->raw_sz = lpBytes(node->carry);
	node->count--;
	quicklist->count--;

	struct quicklist_partition *prev_p, *next_p;
	struct quicklist_node *prev, *next;
	quicklist_prev(p, node, &prev_p, &prev);
	quicklist_next(p, node, &next_p, &next);

	long i = iter->offset;
	if (prev) {
		long j = i + prev->count;
		if (__quicklist_try_merge(quicklist, prev_p, prev, p, node, 1, 1))
			i = j;
	}
	if (next)
		__quicklist_try_merge(quicklist, p, node, next_p, next, 0, 1);

	p = __quicklist_fix_compress(quicklist, p, node);
	int compress = quicklist_p_is_middle(p);
	quicklist_prev(p, node, &prev_p, &prev);
	quicklist_next(p, node, &next_p, &next);

	if (iter->forward) {
		if (i == node->count) {
			if (!next)
				goto iter_complete;

			if (compress)
				quicklist_n_compress_raw(node);
			quicklist_iter_set_node_first(iter, next_p, next);
			goto iter_ahead;
		} else {
			quicklist_iter_set(iter, p, node, i);
			goto iter_ahead;
		}
	} else {
		if (i == 0) {
			if (!prev)
				goto iter_complete;

			if (compress)
				quicklist_n_compress_raw(node);
			quicklist_iter_set_node_last(iter, prev_p, prev);
			goto iter_ahead;
		} else {
			quicklist_iter_set(iter, p, node, i-1);
			goto iter_ahead;
		}
	}

iter_complete:
	iter->status = QUICKLIST_ITER_STATUS_COMPLETE;
	return;

iter_ahead:
	iter->status = QUICKLIST_ITER_STATUS_AHEAD;
}

/**
 * quicklist_iter_del - Delete the element that @iter point to and
 * move @iter to next element base on @iter->forward.
 * 
 * Note: caller should make sure iter is normal.
 * Note: if next element is not exist, @iter's status will change to
 * QUICKLIST_ITER_STATUS_COMPLETE; otherwise, @iter's status will change to
 * QUICKLIST_ITER_STATUS_AHEAD.
 */
void quicklist_iter_del(struct quicklist_iter *iter)
{
	assert(iter->status == QUICKLIST_ITER_STATUS_NORMAL);

	if (iter->raw_node->count == 1)
		quicklist_iter_del_single(iter);
	else
		quicklist_iter_del_multi(iter);
}

/**
 * quicklist_iter_add_carry - Add a new element with value @value and
 * size @sz after or before the element that @iter point to.
 * @after: 1 for add the new element right after the element that @iter point to, 
 * 0 for add the new element right before the element that @iter point to.
 * 
 * Note: caller should make sure @iter->raw_node can hold the new element.
 * Note: @iter will stay in place after the addition.
 */
static void quicklist_iter_add_carry(struct quicklist_iter *iter,
					void *value, size_t sz, int after)
{
	struct quicklist_node *node = iter->raw_node;
	node->carry = lpInsertString(node->carry, value, sz,iter->lp_element,
					after, &iter->lp_element);
	node->raw_sz = lpBytes(node->carry);
	node->count++;
	if (after)
		quicklist_iter_set(iter, iter->p, node, iter->offset);
	else
		quicklist_iter_set(iter, iter->p, node, iter->offset+1);

	iter->quicklist->count++;
}

/**
 * quicklist_iter_add_after_node - Add a new element with value @value and
 * size @sz right after node @iter->raw_node.
 * 
 * Note: caller should make sure @iter->raw_node can't hold the new element.
 * Note: @iter will stay in place after the addition.
 */
static void quicklist_iter_add_after_node(struct quicklist_iter *iter,
						void *value, size_t sz)
{

	struct quicklist *quicklist = iter->quicklist;
	struct quicklist_fill *fill = quicklist->fill;
	struct quicklist_partition *p = iter->p;
	struct quicklist_node *node = iter->raw_node; 

	struct quicklist_partition *next_p;
	struct quicklist_node *next;
	quicklist_next(p, node, &next_p, &next);
	if (next) {
		int compress = quicklist_p_is_middle(next_p);
		if (quicklist_n_try_add_carry_head(fill, next, compress, value, sz))
			goto added;
	}

	struct quicklist_node *new_raw = quicklist_n_new_raw(fill, value, sz);
	quicklist_p_add_node(p, node, node->next, new_raw, 0);
	iter->p = __quicklist_fix_compress(quicklist, p, node);

added:
	iter->quicklist->count++;
}

/**
 * quicklist_iter_add_before_node - Add a new element with value @value and
 * size @sz right before node @iter->raw_node.
 * 
 * Note: caller should make sure @iter->raw_node can't hold the new element.
 * Note: @iter will stay in place after the addition.
 */
static void quicklist_iter_add_before_node(struct quicklist_iter *iter,
						void *value, size_t sz)
{
	struct quicklist *quicklist = iter->quicklist;
	struct quicklist_fill *fill = quicklist->fill;
	struct quicklist_partition *p = iter->p;
	struct quicklist_node *node = iter->raw_node; 

	struct quicklist_partition *prev_p;
	struct quicklist_node *prev;
	quicklist_prev(p, node, &prev_p, &prev);
	if (prev) {
		int compress = quicklist_p_is_middle(prev_p);
		if (quicklist_n_try_add_carry_tail(fill, prev, compress, value, sz))
			goto added;
	}

	struct quicklist_node *new = quicklist_n_new_raw(fill, value, sz);
	quicklist_p_add_node(p, node->prev, node, new, 0);
	iter->p = __quicklist_fix_compress(quicklist, p, node);

added:
	iter->quicklist->count++;
}

/**
 * quicklist_iter_split_add_after - Split @iter->raw_node after the element
 * @iter point to, and add a new element with value @value and size @sz between.
 * 
 * Note: caller should make sure @iter->raw_node can't hold the new element.
 * Note: caller should make sure @iter->raw_node can split into two parts.
 * Note: @iter will stay in place after the addition.
 */
static void quicklist_iter_split_add_after(struct quicklist_iter *iter,
						void *value, size_t sz)
{
	struct quicklist *quicklist = iter->quicklist;
	struct quicklist_fill *fill = quicklist->fill;
	struct quicklist_partition *p = iter->p;
	struct quicklist_node *node = iter->raw_node;

	struct quicklist_partition *prev_p, *next_p;
	struct quicklist_node *prev, *next;
	quicklist_prev(p, node, &prev_p, &prev);
	quicklist_next(p, node, &next_p, &next);

	struct quicklist_partition *split_p = p;
	struct quicklist_node *split = quicklist_n_split_raw(node, iter->offset+1);
	quicklist_p_add_node(p, node, node->next, split, 1);

	if(prev) {
		int forward = quicklist_p_is_middle(prev_p);
		if (__quicklist_try_merge(quicklist, prev_p, prev, p, node, forward, 1)) {
			if (!forward) {
				p = prev_p;
				node = prev;
			}
		}
	}

	if (next) {
		int forward = !quicklist_p_is_middle(next_p);
		if (__quicklist_try_merge(quicklist, split_p, split, next_p, next, forward, 1)) {
			if (forward) {
				split_p = next_p;
				split = next;
			}
		}
	}

	int split_compress = quicklist_p_is_middle(split_p);
	if (quicklist_n_try_add_carry_head(fill, split, split_compress, value, sz)) {
		p = __quicklist_fix_compress(quicklist, p, node);
		quicklist_iter_set_node_last(iter, p, node);
		goto added;
	}

	if (split_compress)
		quicklist_n_compress_raw(split);

	if (quicklist_n_try_add_carry_tail(fill, node, 0, value, sz)) {
		p = __quicklist_fix_compress(quicklist, p, node);
		quicklist_iter_set(iter, p, node, node->count-2);
		goto added;
	}

	struct quicklist_node *new = quicklist_n_new_raw(fill, value, sz);
	quicklist_p_add_node(p, node, node->next, new, 0);
	p = __quicklist_fix_compress(quicklist, p, node);
	quicklist_iter_set_node_last(iter, p, node);

added:
	iter->quicklist->count++;
}

/**
 * quicklist_iter_split_add_before - Split @iter->raw_node before the element
 * @iter point to, and add a new element with value @value and size @sz between.
 * 
 * Note: caller should make sure @iter->raw_node can't hold the new element.
 * Note: caller should make sure @iter->raw_node can split into two parts.
 * Note: @iter will stay in place after the addition.
 */
static void quicklist_iter_split_add_before(struct quicklist_iter *iter,
						void *value, size_t sz)
{
	struct quicklist *quicklist = iter->quicklist;
	struct quicklist_fill *fill = quicklist->fill;
	struct quicklist_partition *p = iter->p;
	struct quicklist_node *node = iter->raw_node;

	struct quicklist_partition *prev_p, *next_p;
	struct quicklist_node *prev, *next;
	quicklist_prev(p, node, &prev_p, &prev);
	quicklist_next(p, node, &next_p, &next);

	struct quicklist_partition *split_p = p;
	struct quicklist_node *split = quicklist_n_split_raw(node, iter->offset);
	quicklist_p_add_node(p, node, node->next, split, 1);

	if(prev) {
		int forward = quicklist_p_is_middle(prev_p);
		if (__quicklist_try_merge(quicklist, prev_p, prev, p, node, forward, 1)) {
			if (!forward) {
				p = prev_p;
				node = prev;
			}
		}
	}

	if (next) {
		int forward = !quicklist_p_is_middle(next_p);
		if (__quicklist_try_merge(quicklist, split_p, split, next_p, next, forward, 1)) {
			if (forward) {
				split_p = next_p;
				split = next;
			}
		}
	}

	int compress = quicklist_p_is_middle(p);
	if (quicklist_n_try_add_carry_tail(fill, node, compress, value, sz)) {
		split_p = __quicklist_fix_compress(quicklist, split_p, split);
		quicklist_iter_set_node_first(iter, split_p, split);
		goto added;
	}

	if (compress)
		quicklist_n_compress_raw(node);

	if (quicklist_n_try_add_carry_head(fill, split, 0, value, sz)) {
		split_p = __quicklist_fix_compress(quicklist, split_p, split);
		quicklist_iter_set(iter, split_p, split, 1);
		goto added;
	}

	struct quicklist_node *new = quicklist_n_new_raw(fill, value, sz);
	quicklist_p_add_node(p, node, node->next, new, 0);
	split_p = __quicklist_fix_compress(quicklist, split_p, split);
	quicklist_iter_set_node_first(iter, split_p, split);

added:
	iter->quicklist->count++;
}

/**
 * quicklist_iter_add_after - Add a new element with value @value and size @sz
 * right after the element that @iter point to.
 * 
 * Note: @iter will stay in place after the addition.
 * Note: caller should make sure @iter is normal.
 */
void quicklist_iter_add_after(struct quicklist_iter *iter, void *value, size_t sz)
{
	assert(iter->status == QUICKLIST_ITER_STATUS_NORMAL);

	struct quicklist_fill *fill = iter->quicklist->fill;
	struct quicklist_node *node = iter->raw_node;

	if (quicklist_n_allow_add_carry(fill, node, sz))
		quicklist_iter_add_carry(iter, value, sz, 1);
	else if (iter->offset == node->count-1)
		quicklist_iter_add_after_node(iter, value, sz);
	else
		quicklist_iter_split_add_after(iter, value, sz);
}

/**
 * quicklist_iter_add_before - Add a new element with value @value and size @sz
 * right before the element that @iter point to.
 * 
 * Note: @iter will stay in place after the addition.
 * Note: caller should make sure @iter is normal.
 */
void quicklist_iter_add_before(struct quicklist_iter *iter, void *value, size_t sz)
{
	assert(iter->status == QUICKLIST_ITER_STATUS_NORMAL);

	struct quicklist_fill *fill = iter->quicklist->fill;
	struct quicklist_node *node = iter->raw_node;
	
	if (quicklist_n_allow_add_carry(fill, node, sz))
		quicklist_iter_add_carry(iter, value, sz, 0);
	else if (iter->offset == 0)
		quicklist_iter_add_before_node(iter, value, sz);
	else
		quicklist_iter_split_add_before(iter, value, sz);
}

/**
 * quicklist_iter_replace - Replace the element that @iter point to with
 * value @value and size @sz. 
 * 
 * Note: @iter will stay in place after replacement.
 * Note: caller should make sure @iter is normal.
 */
void quicklist_iter_replace(struct quicklist_iter *iter, void *value, size_t sz)
{
	assert(iter->status == QUICKLIST_ITER_STATUS_NORMAL);

	if (iter->forward)
		quicklist_iter_add_after(iter, value, sz);
	else
		quicklist_iter_add_before(iter, value, sz);
	
	quicklist_iter_del(iter);
	quicklist_iter_next(iter);
}

/** 
 * quicklist_replace - Replace @quicklist's @ith element with value @value and
 * size @sz.
 * 
 * Return: 1 if @i is valid, 0 if not.
 * 
 * Note: negative @i is acceptable, see quicklist_transform_index().
 */
int quicklist_replace(struct quicklist *quicklist, long i, void *value, size_t sz)
{
	/* skip quicklist_transform_index() */
	struct quicklist_iter *iter = quicklist_iter_new(quicklist, i, 1);
	if (iter == NULL)
		return 0;
	quicklist_iter_replace(iter, value, sz);
	quicklist_iter_free(iter);
	return 1;
}

/** 
 * quicklist_iter_equal - Test the element that @iter point to is equal with
 * value @value and size @sz or not.
 * 
 * Return: 1 if equal, 0 if not.
 */
int quicklist_iter_equal(struct quicklist_iter *iter, void *value, size_t sz)
{
	
	if (iter->status != QUICKLIST_ITER_STATUS_NORMAL)
		return 0;

	struct quicklist_node *node = iter->raw_node;
	if (node->container == QUICKLIST_NODE_CONTAINER_PACKED)
		return lpCompare(iter->lp_element, value, sz);
	return (node->raw_sz == sz) && (memcmp(node->carry, value, sz) == 0);
}

/* The rest of this file is test cases and test helpers. */
#ifdef REDIS_TEST
#include <stdint.h>
#include <sys/time.h>
#include "testhelp.h"
#include <stdlib.h>

#define yell(str, ...) printf("ERROR! " str "\n\n", __VA_ARGS__)

#define ERROR                                                                  \
    do {                                                                       \
        printf("\tERROR!\n");                                                  \
        err++;                                                                 \
    } while (0)

#define ERR(x, ...)                                                            \
    do {                                                                       \
        printf("%s:%s:%d:\t", __FILE__, __func__, __LINE__);                   \
        printf("ERROR! " x "\n", __VA_ARGS__);                                 \
        err++;                                                                 \
    } while (0)

#define TEST(name) printf("test — %s\n", name);
#define TEST_DESC(name, ...) printf("test — " name "\n", __VA_ARGS__);

#define QL_TEST_VERBOSE 0

#define UNUSED(x) (void)(x)
static void ql_info(struct quicklist *ql) {
#if QL_TEST_VERBOSE
	printf("Container length: %lu\n", quicklist_node_count(ql));
	printf("Container size: %lu\n", ql->count);
	printf("\n");
#else
	UNUSED(ql);
#endif
}

/* Return the UNIX time in microseconds */
static long long ustime(void) {
	struct timeval tv;
	long long ust;

	gettimeofday(&tv, NULL);
	ust = ((long long)tv.tv_sec) * 1000000;
	ust += tv.tv_usec;
	return ust;
}

/* Return the UNIX time in milliseconds */
static long long mstime(void) { return ustime() / 1000; }

/* Iterate over an entire quicklist.
 * Print the list if 'print' == 1.
 *
 * Returns physical count of elements found by iterating over the list. */
static int _itrprintr(struct quicklist *ql, int print, int forward) {
	struct quicklist_iter *iter;
	if (forward)
		iter = quicklist_iter_new_ahead(ql, 0, 1);
	else
		iter = quicklist_iter_new_ahead(ql, -1, 0);
	if (iter == NULL)
		return 0;
	int i = 0;
	int p = 0;
	struct quicklist_node *prev = NULL;
	while (quicklist_iter_next(iter)) {
		if (iter->raw_node != prev) {
			/* Count the number of list nodes too */
			p++;
			prev = iter->raw_node;
		}
		if (print) {
			struct quicklist_element elem;
			quicklist_iter_get_element(iter, &elem);
			int size = (elem.sz > (1<<20)) ? 1<<20 : elem.sz;
			printf("[%3d (%2d)]: [%.*s] (%lld)\n", i, p, size,
				(char *)elem.value, elem.longval);
		}
		i++;
	}
	quicklist_iter_free(iter);
	return i;
}

static int itrprintr(struct quicklist *ql, int print) {
	return _itrprintr(ql, print, 1);
}

static int itrprintr_rev(struct quicklist *ql, int print) {
	return _itrprintr(ql, print, 0);
}

#define ql_verify(a, b, c, d, e)                                               \
    do {                                                                       \
        err += _ql_verify((a), (b), (c), (d), (e));                            \
    } while (0)

static int ql_p_verify_not_compress(struct quicklist_partition *p, long *i)
{
	int error_count = 0;
	long node_count = 0;
	struct quicklist_node *guard = p->guard;
	struct quicklist_node *node = guard->next;
	while (node != guard) {
		if (!node->raw) {
			error_count++;
			yell("Incorrect compression: node: %ld should be raw", *i);
		}

		node = node->next;
		node_count++;
	}
	if (node_count != p->length) {
		error_count++;
		yell("Incorrect compression: partition: %d length: %ld actual: %ld",
			p->which, p->length, node_count);
	}
	if (p->length > p->capacity) {
		error_count++;
		yell("Incorrect compression: partition: %d overflow length: %ld capacity: %ld",
			p->which, p->length, p->capacity);
	}
	(*i) += node_count;
	return error_count;
}

static int ql_p_verify_compress(struct quicklist_partition *p, long *i)
{
	int error_count = 0;
	long node_count = 0;

	if (quicklist_p_is_overflow(p)) {
		error_count++;
		yell("Incorrect compression: partition: %d is overflow", p->which);
	}

	struct quicklist_node *guard = p->guard;
	struct quicklist_node *node = guard->next;
	while (node != guard) {
		if (node->raw) {
			quicklist_n_compress_raw(node);
			if (!node->raw) {
				error_count++;
				yell("Incorrect compression: node: %ld should be compressed", *i);
			}
		}

		node = node->next;
		node_count++;
	}
	if (node_count != p->length) {
		error_count++;
		yell("Incorrect compression: partition: %d length: %ld actual: %ld",
			p->which, p->length, node_count);
	}
	if (p->length > p->capacity) {
		error_count++;
		yell("Incorrect compression: partition: %d overflow length: %ld capacity: %ld",
			p->which, p->length, p->capacity);
	}
	(*i) += node_count;
	return error_count;
}

static int ql_verify_compress(struct quicklist *quicklist)
{
	int error_count = 0;
	long i = 0;
	struct quicklist_partition *head = quicklist->head;
	struct quicklist_partition *middle = head->next;
	struct quicklist_partition *tail = middle->next;
	error_count += ql_p_verify_not_compress(head, &i);
	error_count += ql_p_verify_compress(middle, &i);
	error_count += ql_p_verify_not_compress(tail, &i);

	if (middle->length == 0) {
		if (quicklist_p_is_overflow(head)) {
			error_count++;
			yell("Incorrect compression: partition: head length: %ld is overflow",
				head->length);
		}
		if (quicklist_p_is_overflow(tail)) {
			error_count++;
			yell("Incorrect compression: partition: tail length: %ld is overflow",
				head->length);
		}
	} else {
		if (!quicklist_p_is_full(head)) {
			error_count++;
			yell("Incorrect compression: partition: head length: %ld is not full",
				head->length);
		}
		if (!quicklist_p_is_full(tail)) {
			error_count++;
			yell("Incorrect compression: partition: tail length: %ld is not full",
				head->length);
		}
	}
	return error_count;
}

static int ql_verify_merge(struct quicklist *quicklist)
{
	if (quicklist->count == 0)
		return 0;

	int error_count = 0;
	struct quicklist_fill *fill = quicklist->fill;
	struct quicklist_partition *p;
	struct quicklist_node *node;
	quicklist_first_node(quicklist, &p, &node);
	long i = 0;
	while (1) {
		struct quicklist_partition *next_p;
		struct quicklist_node *next;
		quicklist_next(p, node, &next_p, &next);
		if (!next)
			return error_count;
		if (quicklist_n_allow_merge(fill, node, next)) {
			error_count++;
			yell("Incorrect merge: node: %ld is allowed to merge to next", i);
		}
		p = next_p;
		node = next;
		i++;
	}
}

static int ql_verify_pack_limit(struct quicklist *quicklist)
{
	int error_count = 0;
	struct quicklist_fill *fill = quicklist->fill;
	struct quicklist_partition *p;
	struct quicklist_node *node;
	quicklist_first_node(quicklist, &p, &node);
	long i = 0;
	while (node) {
		if (node->container == QUICKLIST_NODE_CONTAINER_PLAIN) {
			if (!quicklist_is_large_element(fill, node->raw_sz)) {
				error_count++;
				yell("Incorrect limit: node: %ld is not a large element", i);
			}
		} else {
			if (node->count > fill->pack_max_count) {
				error_count++;
				yell("Incorrect limit: node: %ld count: %d limit: %d",
					i, node->count, fill->pack_max_count);
			}
			if (node->raw_sz > fill->pack_max_size) {
				error_count++;
				yell("Incorrect limit: node: %ld sz: %zu limit: %d",
					i, node->raw_sz, fill->pack_max_size);
			}
		}
		i++;
		quicklist_next(p, node, &p, &node);
	}
	return error_count;
}

static int ql_n_actual_count(struct quicklist_node *node)
{
	if (node->container == QUICKLIST_NODE_CONTAINER_PLAIN)
		return 1;
	return lpLength(node->carry);
}

/* Verify list metadata matches physical list contents. */
static int _ql_verify(struct quicklist *ql, long len, long count,
                      int head_count, int tail_count) {
	int errors = 0;

	ql_info(ql);
	if (len != quicklist_node_count(ql)) {
		yell("quicklist length wrong: expected %ld, got %ld", len, quicklist_node_count(ql));
		errors++;
	}

	if (count != ql->count) {
		yell("quicklist count wrong: expected %ld, got %ld", count, ql->count);
		errors++;
	}

	int loopr = itrprintr(ql, 0);
	if (loopr != (int)ql->count) {
		yell("quicklist cached count not match actual count: expected %ld, got "
			"%d", ql->count, loopr);
		errors++;
	}

	int rloopr = itrprintr_rev(ql, 0);
	if (loopr != rloopr) {
		yell("quicklist has different forward count than reverse count!  "
			"Forward count is %d, reverse count is %d.", loopr, rloopr);
		errors++;
	}

	if (quicklist_node_count(ql) == 0 && !errors) {
		return errors;
	}

	struct quicklist_partition *head_p;
	struct quicklist_node *head;
	quicklist_first_node(ql, &head_p, &head);
	if (head) {
		int cache_count = head->count;
		int actual_count = ql_n_actual_count(head);
		if (head_count != cache_count || head_count != actual_count) {
			yell("quicklist head count wrong: expected %d, "
				"got cached %d vs. actual %d",
				head_count, cache_count, actual_count);
			errors++;
		}
	}

	struct quicklist_partition *tail_p;
	struct quicklist_node *tail;
	quicklist_last_node(ql, &tail_p, &tail);
	if (tail) {
		int cache_count = tail->count;
		int actual_count = ql_n_actual_count(tail);
		if (tail_count != cache_count || tail_count != actual_count) {
			yell("quicklist tail count wrong: expected %d, "
				"got cached %d vs. actual %d",
				tail_count, cache_count, actual_count);
			errors++;
		}
	}

	errors += ql_verify_compress(ql);
	errors += ql_verify_merge(ql);
	errors += ql_verify_pack_limit(ql);
	return errors;
}

/* Release iterator and verify compress correctly. */
static void ql_release_iterator(struct quicklist_iter *iter) {
	struct quicklist *ql = NULL;
	if (iter) ql = iter->quicklist;
	quicklist_iter_free(iter);
	if (ql) assert(!ql_verify_compress(ql));
}

/* Generate new string concatenating integer i against string 'prefix' */
static char *genstr(char *prefix, int i) {
	static char result[64] = {0};
	snprintf(result, sizeof(result), "%s%d", prefix, i);
	return result;
}

static void randstring(unsigned char *target, size_t sz) {
	size_t p = 0;
	int minval, maxval;
	switch(rand() % 3) {
	case 0:
		minval = 'a';
		maxval = 'z';
		break;
	case 1:
		minval = '0';
		maxval = '9';
		break;
	case 2:
		minval = 'A';
		maxval = 'Z';
		break;
	default:
		assert(NULL);
	}

	while(p < sz)
		target[p++] = minval+rand()%(maxval-minval+1);
}

/** quicklist_get_element - Get @ith element of @quicklist.
 * @elem: The found element is stored in *@elem.
 * 
 * Return: 1 if element is found, 0 if not.
 */
static int quicklist_get_element(struct quicklist *quicklist, long i,
					struct quicklist_element *elem)
{
	struct quicklist_iter *iter = quicklist_iter_new(quicklist, i, 0);
	if (iter == NULL)
		return 0;

	quicklist_iter_get_element(iter, elem);
	quicklist_iter_free(iter);
	return 1;
}

/* main test, but callable from other files */
int quicklistTest(int argc, char *argv[], int flags) {
	UNUSED(argc);
	UNUSED(argv);

	int accurate = (flags & REDIS_TEST_ACCURATE);
	unsigned int err = 0;
	int optimize_start =
		-(int)(sizeof(QUICKLIST_OPTIMIZED_LEVEL) / sizeof(*QUICKLIST_OPTIMIZED_LEVEL));

	printf("Starting optimization offset at: %d\n", optimize_start);

	int options[] = {0, 1, 2, 3, 4, 5, 6, 10};
	int fills[] = {-5, -4, -3, -2, -1, 0,
			1, 2, 32, 66, 128, 999};
	size_t option_count = sizeof(options) / sizeof(*options);
	int fill_count = (int)(sizeof(fills) / sizeof(*fills));
	long long runtime[option_count];

	for (int _i = 0; _i < (int)option_count; _i++) {
	printf("Testing Compression option %d\n", options[_i]);
	long long start = mstime();
	struct quicklist_iter *iter;

	TEST("create list") {
		struct quicklist *ql = quicklist_new(-2, options[_i]);
		ql_verify(ql, 0, 0, 0, 0);
		quicklist_free(ql);
	}

	TEST("add to tail of empty list") {
		struct quicklist *ql = quicklist_new(-2, options[_i]);
		quicklist_push_tail(ql, "hello", 6);
		/* 1 for head and 1 for tail because 1 node = head = tail */
		ql_verify(ql, 1, 1, 1, 1);
		quicklist_free(ql);
	}

        TEST("add to head of empty list") {
		struct quicklist *ql = quicklist_new(-2, options[_i]);
		quicklist_push_head(ql, "hello", 6);
		/* 1 for head and 1 for tail because 1 node = head = tail */
		ql_verify(ql, 1, 1, 1, 1);
		quicklist_free(ql);
        }

        TEST_DESC("add to tail 5x at compress %d", options[_i]) {
            for (int f = 0; f < fill_count; f++) {
                struct quicklist *ql = quicklist_new(fills[f], options[_i]);
                for (int i = 0; i < 5; i++)
                    quicklist_push_tail(ql, genstr("hello", i), 32);
                if (ql->count != 5)
                    ERROR;
                if (fills[f] == 32)
                    ql_verify(ql, 1, 5, 5, 5);
                quicklist_free(ql);
            }
        }

        TEST_DESC("add to head 5x at compress %d", options[_i]) {
            for (int f = 0; f < fill_count; f++) {
                struct quicklist *ql = quicklist_new(fills[f], options[_i]);
                for (int i = 0; i < 5; i++)
                    quicklist_push_head(ql, genstr("hello", i), 32);
                if (ql->count != 5)
                    ERROR;
                if (fills[f] == 32)
                    ql_verify(ql, 1, 5, 5, 5);
                quicklist_free(ql);
            }
        }

        TEST_DESC("add to tail 500x at compress %d", options[_i]) {
            for (int f = 0; f < fill_count; f++) {
                struct quicklist *ql = quicklist_new(fills[f], options[_i]);
                for (int i = 0; i < 500; i++)
                    quicklist_push_tail(ql, genstr("hello", i), 64);
                if (ql->count != 500)
                    ERROR;
                if (fills[f] == 32)
                    ql_verify(ql, 16, 500, 32, 20);
                quicklist_free(ql);
            }
        }

        TEST_DESC("add to head 500x at compress %d", options[_i]) {
            for (int f = 0; f < fill_count; f++) {
                struct quicklist *ql = quicklist_new(fills[f], options[_i]);
                for (int i = 0; i < 500; i++)
                    quicklist_push_head(ql, genstr("hello", i), 32);
                if (ql->count != 500)
                    ERROR;
                if (fills[f] == 32)
                    ql_verify(ql, 16, 500, 20, 32);
                quicklist_free(ql);
            }
        }

        TEST("Comprassion Plain node") {
            char buf[256];
            struct quicklist *ql = quicklist_new(0, 1);
            for (int i = 0; i < 500; i++) {
                /* Set to 256 to allow the node to be triggered to compress,
                 * if it is less than 48(nocompress), the test will be successful. */
                snprintf(buf, sizeof(buf), "hello%d", i);
                quicklist_push_head(ql, buf, 256);
            }

            struct quicklist_iter *iter = quicklist_iter_new_ahead(ql, -1, 0);
            struct quicklist_element elem;
            int i = 0;
            while (quicklist_iter_next(iter)) {
                snprintf(buf, sizeof(buf), "hello%d", i);
		quicklist_iter_get_element(iter, &elem);
                if (strcmp((char *)elem.value, buf))
                    ERR("value [%s] didn't match [%s] at position %d",
                        elem.value, buf, i);
                i++;
            }
            ql_release_iterator(iter);
            quicklist_free(ql);
        }

        TEST("NEXT plain node")
        {
            struct quicklist *ql = quicklist_new(0, options[_i]);
            char *strings[] = {"hello1", "hello2", "h3", "h4", "hello5"};

            for (int i = 0; i < 5; ++i)
                quicklist_push_head(ql, strings[i], strlen(strings[i]));

            struct quicklist_element elem;
            struct quicklist_iter *iter = quicklist_iter_new_ahead(ql, -1, 0);
            int j = 0;

            while(quicklist_iter_next(iter) != 0) {
		quicklist_iter_get_element(iter, &elem);
                assert(strncmp(strings[j], (char *)elem.value, strlen(strings[j])) == 0);
                j++;
            }
            ql_release_iterator(iter);
            quicklist_free(ql);
        }

        TEST("pop 1 string from 1") {
            struct quicklist *ql = quicklist_new(-2, options[_i]);
            char *populate = genstr("hello", 331);
            quicklist_push_head(ql, populate, 32);
            ql_info(ql);
	    struct quicklist_iter *iter = quicklist_iter_new(ql, 0, 1);
	    assert(iter);
	    struct quicklist_element elem;
	    quicklist_iter_get_element(iter, &elem);
	    assert(elem.value);
	    assert(elem.sz == 32);
	    if (strcmp(populate, (char *)elem.value)) {
                int size = elem.sz;
                ERR("Pop'd value (%.*s) didn't equal original value (%s)", size,
                    elem.value, populate);
            }
	    quicklist_iter_del(iter);
            ql_verify(ql, 0, 0, 0, 0);
            ql_release_iterator(iter);
            quicklist_free(ql);
        }

        TEST("pop head 1 number from 1") {
            struct quicklist *ql = quicklist_new(-2, options[_i]);
            quicklist_push_head(ql, "55513", 5);
	    struct quicklist_iter *iter = quicklist_iter_new(ql, 0, 1);
	    assert(iter);
	    struct quicklist_element elem;
	    quicklist_iter_get_element(iter, &elem);
	    assert(elem.value == NULL);
	    assert(elem.longval == 55513);
	    quicklist_iter_del(iter);
            ql_verify(ql, 0, 0, 0, 0);
            ql_release_iterator(iter);
            quicklist_free(ql);
        }

        TEST("pop head 500 from 500") {
            struct quicklist *ql = quicklist_new(-2, options[_i]);
            for (int i = 0; i < 500; i++)
                quicklist_push_head(ql, genstr("hello", i), 32);
            ql_info(ql);
            for (int i = 0; i < 500; i++) {
		struct quicklist_iter *iter = quicklist_iter_new(ql, 0, 1);
		assert(iter);
	        struct quicklist_element elem;
	        quicklist_iter_get_element(iter, &elem);
	        assert(elem.value);
	        assert(elem.sz == 32);
		if (strcmp(genstr("hello", 499 - i), (char *)elem.value)) {
                    int size = elem.sz;
                    ERR("Pop'd value (%.*s) didn't equal original value (%s)",
                        size, elem.value, genstr("hello", 499 - i));
                }
	        quicklist_iter_del(iter);
                ql_release_iterator(iter);
            }
            ql_verify(ql, 0, 0, 0, 0);
            quicklist_free(ql);
        }

        TEST("pop head 5000 from 500") {
            struct quicklist *ql = quicklist_new(-2, options[_i]);
            for (int i = 0; i < 500; i++)
                quicklist_push_head(ql, genstr("hello", i), 32);
            for (int i = 0; i < 5000; i++) {
		struct quicklist_iter *iter = quicklist_iter_new(ql, 0, 1);
                if (i < 500) {
                    assert(iter);
		    struct quicklist_element elem;
	            quicklist_iter_get_element(iter, &elem);
                    assert(elem.value);
                    assert(elem.sz == 32);
                    if (strcmp(genstr("hello", 499 - i), (char *)elem.value)) {
                        int size = elem.sz;
                        ERR("Pop'd value (%.*s) didn't equal original value "
                            "(%s)",
                            size, elem.value, genstr("hello", 499 - i));
                    }
	            quicklist_iter_del(iter);
                    ql_release_iterator(iter);
                } else {
                    assert(iter == NULL);
                }
            }
            ql_verify(ql, 0, 0, 0, 0);
            quicklist_free(ql);
        }

        TEST("iterate forward over 500 list") {
            struct quicklist *ql = quicklist_new(32, options[_i]);
            for (int i = 0; i < 500; i++)
                quicklist_push_head(ql, genstr("hello", i), 32);
            struct quicklist_iter *iter = quicklist_iter_new_ahead(ql, 0, 1);
            struct quicklist_element elem;
            int i = 499, count = 0;
            while (quicklist_iter_next(iter)) {
		quicklist_iter_get_element(iter, &elem);
                char *h = genstr("hello", i);
                if (strcmp((char *)elem.value, h))
                    ERR("value [%s] didn't match [%s] at position %d",
                        elem.value, h, i);
                i--;
                count++;
            }
            if (count != 500)
                ERR("Didn't iterate over exactly 500 elements (%d)", i);
            ql_verify(ql, 16, 500, 20, 32);
            ql_release_iterator(iter);
            quicklist_free(ql);
        }

        TEST("iterate reverse over 500 list") {
            struct quicklist *ql = quicklist_new(32, options[_i]);
            for (int i = 0; i < 500; i++)
                quicklist_push_head(ql, genstr("hello", i), 32);
            struct quicklist_iter *iter = quicklist_iter_new_ahead(ql, -1, 0);
            struct quicklist_element elem;
            int i = 0;
            while (quicklist_iter_next(iter)) {
                char *h = genstr("hello", i);
		quicklist_iter_get_element(iter, &elem);
                if (strcmp((char *)elem.value, h))
                    ERR("value [%s] didn't match [%s] at position %d",
                        elem.value, h, i);
                i++;
            }
            if (i != 500)
                ERR("Didn't iterate over exactly 500 elements (%d)", i);
            ql_verify(ql, 16, 500, 20, 32);
            ql_release_iterator(iter);
            quicklist_free(ql);
        }

        TEST("insert after 1 element") {
            struct quicklist *ql = quicklist_new(-2, options[_i]);
            quicklist_push_head(ql, "hello", 6);
	    iter = quicklist_iter_new(ql, 0, 0);
	    quicklist_iter_add_after(iter, "abc", 4);
            ql_release_iterator(iter);
            ql_verify(ql, 1, 2, 2, 2);

            /* verify results */
            struct quicklist_element elem;
	    quicklist_get_element(ql, 0, &elem);
	    int sz = elem.sz;
            if (strncmp((char *)elem.value, "hello", 5)) {
                ERR("Value 0 didn't match, instead got: %.*s", sz, elem.value);
            }

	    quicklist_get_element(ql, 1, &elem);
	    sz = elem.sz;
            if (strncmp((char *)elem.value, "abc", 3)) {
                ERR("Value 1 didn't match, instead got: %.*s", sz, elem.value);
            }
            quicklist_free(ql);
        }

        TEST("insert before 1 element") {
            struct quicklist *ql = quicklist_new(-2, options[_i]);
            quicklist_push_head(ql, "hello", 6);
            iter = quicklist_iter_new(ql, 0, 0);
            quicklist_iter_add_before(iter, "abc", 4);
            ql_release_iterator(iter);
            ql_verify(ql, 1, 2, 2, 2);

            /* verify results */
            struct quicklist_element elem;
	    quicklist_get_element(ql, 0, &elem);
	    int sz = elem.sz;
            if (strncmp((char *)elem.value, "abc", 3)) {
                ERR("Value 0 didn't match, instead got: %.*s", sz, elem.value);
            }

	    quicklist_get_element(ql, 1, &elem);
	    sz = elem.sz;
            if (strncmp((char *)elem.value, "hello", 5)) {
                ERR("Value 1 didn't match, instead got: %.*s", sz, elem.value);
            }
            quicklist_free(ql);
        }

        TEST("insert head while head node is full") {
		struct quicklist *ql = quicklist_new(4, options[_i]);
		for (int i = 0; i < 10; i++)
			quicklist_push_tail(ql, genstr("hello", i), 6);
		iter = quicklist_iter_new(ql, -10, 0);
		char buf[4096] = {0};
		quicklist_iter_add_before(iter, buf, 4096);
		ql_release_iterator(iter);
		ql_verify(ql, 4, 11, 1, 2);
		quicklist_free(ql);
        }

        TEST("insert tail while tail node is full") {
		struct quicklist *ql = quicklist_new(4, options[_i]);
		for (int i = 0; i < 10; i++)
			quicklist_push_head(ql, genstr("hello", i), 6);
		iter = quicklist_iter_new(ql, -1, 0);
		char buf[4096] = {0};
		quicklist_iter_add_after(iter, buf, 4096);
		ql_release_iterator(iter);
		ql_verify(ql, 4, 11, 2, 1);
		quicklist_free(ql);
        }

        TEST_DESC("insert once in elements while iterating at compress %d",
                  options[_i]) {
            for (int f = 0; f < fill_count; f++) {
                struct quicklist *ql = quicklist_new(fills[f], options[_i]);
                quicklist_push_tail(ql, "abc", 3);
                quicklist_push_tail(ql, "def", 3);
                quicklist_push_tail(ql, "bob", 3);
                quicklist_push_tail(ql, "foo", 3);
                quicklist_push_tail(ql, "zoo", 3);

                itrprintr(ql, 0);
                /* insert "bar" before "bob" while iterating over list. */
                struct quicklist_iter *iter = quicklist_iter_new_ahead(ql, 0, 1);
                struct quicklist_element elem;
                while (quicklist_iter_next(iter)) {
			quicklist_iter_get_element(iter, &elem);
                    if (!strncmp((char *)elem.value, "bob", 3)) {
                        /* Insert as fill = 1 so it spills into new node. */
                        quicklist_iter_add_before(iter, "bar", 3);
                        break; /* didn't we fix insert-while-iterating? */
                    }
                }
                ql_release_iterator(iter);
                itrprintr(ql, 0);

                /* verify results */
		quicklist_get_element(ql, 0, &elem);
		int sz = elem.sz;
                if (strncmp((char *)elem.value, "abc", 3))
                    ERR("Value 0 didn't match, instead got: %.*s", sz,
                        elem.value);

		quicklist_get_element(ql, 1, &elem);
		sz = elem.sz;
                if (strncmp((char *)elem.value, "def", 3))
                    ERR("Value 1 didn't match, instead got: %.*s", sz,
                        elem.value);

		quicklist_get_element(ql, 2, &elem);
		sz = elem.sz;
                if (strncmp((char *)elem.value, "bar", 3))
                    ERR("Value 2 didn't match, instead got: %.*s", sz,
                        elem.value);

		quicklist_get_element(ql, 3, &elem);
		sz = elem.sz;
                if (strncmp((char *)elem.value, "bob", 3))
                    ERR("Value 3 didn't match, instead got: %.*s", sz,
                        elem.value);

                quicklist_get_element(ql, 4, &elem);
		sz = elem.sz;
                if (strncmp((char *)elem.value, "foo", 3))
                    ERR("Value 4 didn't match, instead got: %.*s", sz,
                        elem.value);

                quicklist_get_element(ql, 5, &elem);
		sz = elem.sz;
                if (strncmp((char *)elem.value, "zoo", 3))
                    ERR("Value 5 didn't match, instead got: %.*s", sz,
                        elem.value);
                quicklist_free(ql);
            }
        }

        TEST_DESC("insert [before] 250 new in middle of 500 elements at compress %d",
                  options[_i]) {
            for (int f = 0; f < fill_count; f++) {
                struct quicklist *ql = quicklist_new(fills[f], options[_i]);
                for (int i = 0; i < 500; i++)
                    quicklist_push_tail(ql, genstr("hello", i), 32);
                for (int i = 0; i < 250; i++) {
                    iter = quicklist_iter_new(ql, 250, 0);
                    quicklist_iter_add_before(iter, genstr("abc", i), 32);
                    ql_release_iterator(iter);
                }
                if (fills[f] == 32)
                    ql_verify(ql, 25, 750, 32, 20);
                quicklist_free(ql);
            }
        }

        TEST_DESC("insert [after] 250 new in middle of 500 elements at compress %d",
                  options[_i]) {
            for (int f = 0; f < fill_count; f++) {
                struct quicklist *ql = quicklist_new(fills[f], options[_i]);
                for (int i = 0; i < 500; i++)
                    quicklist_push_head(ql, genstr("hello", i), 32);
                for (int i = 0; i < 250; i++) {
                    iter = quicklist_iter_new(ql, 250, 0);
                    quicklist_iter_add_after(iter, genstr("abc", i), 32);
                    ql_release_iterator(iter);
                }

                if (ql->count != 750)
                    ERR("List size not 750, but rather %ld", ql->count);

                if (fills[f] == 32)
                    ql_verify(ql, 26, 750, 20, 32);
                quicklist_free(ql);
            }
        }

        TEST("duplicate empty list") {
            struct quicklist *ql = quicklist_new(-2, options[_i]);
            ql_verify(ql, 0, 0, 0, 0);
            struct quicklist *copy = quicklist_dup(ql);
            ql_verify(copy, 0, 0, 0, 0);
            quicklist_free(ql);
            quicklist_free(copy);
        }

        TEST("duplicate list of 1 element") {
            struct quicklist *ql = quicklist_new(-2, options[_i]);
            quicklist_push_head(ql, genstr("hello", 3), 32);
            ql_verify(ql, 1, 1, 1, 1);
            struct quicklist *copy = quicklist_dup(ql);
            ql_verify(copy, 1, 1, 1, 1);
            quicklist_free(ql);
            quicklist_free(copy);
        }

        TEST("duplicate list of 500") {
            struct quicklist *ql = quicklist_new(32, options[_i]);
            for (int i = 0; i < 500; i++)
                quicklist_push_head(ql, genstr("hello", i), 32);
            ql_verify(ql, 16, 500, 20, 32);

            struct quicklist *copy = quicklist_dup(ql);
            ql_verify(copy, 16, 500, 20, 32);
            quicklist_free(ql);
            quicklist_free(copy);
        }

        for (int f = 0; f < fill_count; f++) {
            TEST_DESC("index 1,200 from 500 list at fill %d at compress %d", f,
                      options[_i]) {
                struct quicklist *ql = quicklist_new(fills[f], options[_i]);
                for (int i = 0; i < 500; i++)
                    quicklist_push_tail(ql, genstr("hello", i + 1), 32);
                struct quicklist_element elem;
                quicklist_get_element(ql, 1, &elem);
                if (strcmp((char *)elem.value, "hello2") != 0)
                    ERR("Value: %s", elem.value);

                quicklist_get_element(ql, 200, &elem);
                if (strcmp((char *)elem.value, "hello201") != 0)
                    ERR("Value: %s", elem.value);
                quicklist_free(ql);
            }

            TEST_DESC("index -1,-2 from 500 list at fill %d at compress %d",
                      fills[f], options[_i]) {
                struct quicklist *ql = quicklist_new(fills[f], options[_i]);
                for (int i = 0; i < 500; i++)
                    quicklist_push_tail(ql, genstr("hello", i + 1), 32);
                struct quicklist_element elem;
                quicklist_get_element(ql, -1, &elem);
                if (strcmp((char *)elem.value, "hello500") != 0)
                    ERR("Value: %s", elem.value);

                quicklist_get_element(ql, -2, &elem);
                if (strcmp((char *)elem.value, "hello499") != 0)
                    ERR("Value: %s", elem.value);
                quicklist_free(ql);
            }

            TEST_DESC("index -100 from 500 list at fill %d at compress %d",
                      fills[f], options[_i]) {
                struct quicklist *ql = quicklist_new(fills[f], options[_i]);
                for (int i = 0; i < 500; i++)
                    quicklist_push_tail(ql, genstr("hello", i + 1), 32);
                struct quicklist_element elem;
                quicklist_get_element(ql, -100, &elem);
                if (strcmp((char *)elem.value, "hello401") != 0)
                    ERR("Value: %s", elem.value);
                quicklist_free(ql);
            }

            TEST_DESC("index too big +1 from 50 list at fill %d at compress %d",
                      fills[f], options[_i]) {
                struct quicklist *ql = quicklist_new(fills[f], options[_i]);
                for (int i = 0; i < 50; i++)
                    quicklist_push_tail(ql, genstr("hello", i + 1), 32);
                struct quicklist_element elem;
		iter = quicklist_iter_new(ql, 50, 0);
                if (iter) {
		    quicklist_iter_get_element(iter, &elem);
		    int sz = elem.sz;
		    ERR("Index found at 50 with 50 list: %.*s", sz,
                        elem.value);
		}   
                quicklist_free(ql);
            }
        }

        TEST("delete range empty list") {
            struct quicklist *ql = quicklist_new(-2, options[_i]);
            quicklist_del(ql, 5, 20);
            ql_verify(ql, 0, 0, 0, 0);
            quicklist_free(ql);
        }

        TEST("delete range of entire node in list of one node") {
            struct quicklist *ql = quicklist_new(-2, options[_i]);
            for (int i = 0; i < 32; i++)
                quicklist_push_head(ql, genstr("hello", i), 32);
            ql_verify(ql, 1, 32, 32, 32);
            quicklist_del(ql, 0, 32);
            ql_verify(ql, 0, 0, 0, 0);
            quicklist_free(ql);
        }

        TEST("delete range of entire node with overflow counts") {
           struct quicklist *ql = quicklist_new(-2, options[_i]);
            for (int i = 0; i < 32; i++)
                quicklist_push_head(ql, genstr("hello", i), 32);
            ql_verify(ql, 1, 32, 32, 32);
            quicklist_del(ql, 0, 128);
            ql_verify(ql, 0, 0, 0, 0);
            quicklist_free(ql);
        }

        TEST("delete middle 100 of 500 list") {
            struct quicklist *ql = quicklist_new(32, options[_i]);
            for (int i = 0; i < 500; i++)
                quicklist_push_tail(ql, genstr("hello", i + 1), 32);
            ql_verify(ql, 16, 500, 32, 20);
            quicklist_del(ql, 200, 100);
            ql_verify(ql, 13, 400, 32, 20);
            quicklist_free(ql);
        }

        TEST("delete less than fill but across nodes") {
            struct quicklist *ql = quicklist_new(32, options[_i]);
            for (int i = 0; i < 500; i++)
                quicklist_push_tail(ql, genstr("hello", i + 1), 32);
            ql_verify(ql, 16, 500, 32, 20);
            quicklist_del(ql, 60, 10);
            ql_verify(ql, 16, 490, 32, 20);
            quicklist_free(ql);
        }

        TEST("delete negative 1 from 500 list") {
            struct quicklist *ql = quicklist_new(32, options[_i]);
            for (int i = 0; i < 500; i++)
                quicklist_push_tail(ql, genstr("hello", i + 1), 32);
            ql_verify(ql, 16, 500, 32, 20);
            quicklist_del(ql, -1, 1);
            ql_verify(ql, 16, 499, 32, 19);
            quicklist_free(ql);
        }

        TEST("delete negative 1 from 500 list with overflow counts") {
            struct quicklist *ql = quicklist_new(32, options[_i]);
            for (int i = 0; i < 500; i++)
                quicklist_push_tail(ql, genstr("hello", i + 1), 32);
            ql_verify(ql, 16, 500, 32, 20);
            quicklist_del(ql, -1, 128);
            ql_verify(ql, 16, 499, 32, 19);
            quicklist_free(ql);
        }

        TEST("delete negative 100 from 500 list") {
            struct quicklist *ql = quicklist_new(32, options[_i]);
            for (int i = 0; i < 500; i++)
                quicklist_push_tail(ql, genstr("hello", i + 1), 32);
            quicklist_del(ql, -100, 100);
            ql_verify(ql, 13, 400, 32, 16);
            quicklist_free(ql);
        }

        TEST("delete -10 count 5 from 50 list") {
            struct quicklist *ql = quicklist_new(32, options[_i]);
            for (int i = 0; i < 50; i++)
                quicklist_push_tail(ql, genstr("hello", i + 1), 32);
            ql_verify(ql, 2, 50, 32, 18);
            quicklist_del(ql, -10, 5);
            ql_verify(ql, 2, 45, 32, 13);
            quicklist_free(ql);
        }

        TEST("numbers only list read") {
            struct quicklist *ql = quicklist_new(-2, options[_i]);
            quicklist_push_tail(ql, "1111", 4);
            quicklist_push_tail(ql, "2222", 4);
            quicklist_push_tail(ql, "3333", 4);
            quicklist_push_tail(ql, "4444", 4);
            ql_verify(ql, 1, 4, 4, 4);
            struct quicklist_element elem;
            quicklist_get_element(ql, 0, &elem);
            if (elem.longval != 1111)
                ERR("Not 1111, %lld", elem.longval);

            quicklist_get_element(ql, 1, &elem);
            if (elem.longval != 2222)
                ERR("Not 2222, %lld", elem.longval);

            quicklist_get_element(ql, 2, &elem);
            if (elem.longval != 3333)
                ERR("Not 3333, %lld", elem.longval);

            quicklist_get_element(ql, 3, &elem);
            if (elem.longval != 4444)
                ERR("Not 4444, %lld", elem.longval);

            if (quicklist_get_element(ql, 4, &elem))
                ERR("Index past elements: %lld", elem.longval);
            
            quicklist_get_element(ql, -1, &elem);
            if (elem.longval != 4444)
                ERR("Not 4444 (reverse), %lld", elem.longval);

            quicklist_get_element(ql, -2, &elem);
            if (elem.longval != 3333)
                ERR("Not 3333 (reverse), %lld", elem.longval);

            quicklist_get_element(ql, -3, &elem);
            if (elem.longval != 2222)
                ERR("Not 2222 (reverse), %lld", elem.longval);
            
            quicklist_get_element(ql, -4, &elem);
            if (elem.longval != 1111)
                ERR("Not 1111 (reverse), %lld", elem.longval);
            
            if (quicklist_get_element(ql, -5, &elem))
                ERR("Index past elements (reverse), %lld", elem.longval);
            quicklist_free(ql);
        }

        TEST("numbers larger list read") {
            struct quicklist *ql = quicklist_new(32, options[_i]);
            char num[32];
            long long nums[5000];
            for (int i = 0; i < 5000; i++) {
                nums[i] = -5157318210846258176 + i;
                int sz = ll2string(num, sizeof(num), nums[i]);
                quicklist_push_tail(ql, num, sz);
            }
            quicklist_push_tail(ql, "xxxxxxxxxxxxxxxxxxxx", 20);
            struct quicklist_element elem;
            for (int i = 0; i < 5000; i++) {
                quicklist_get_element(ql, i, &elem);
                if (elem.longval != nums[i])
                    ERR("[%d] Not longval %lld but rather %lld", i, nums[i],
                        elem.longval);
                elem.longval = 0xdeadbeef;
            }
            quicklist_get_element(ql, 5000, &elem);
            if (strncmp((char *)elem.value, "xxxxxxxxxxxxxxxxxxxx", 20))
                ERR("String val not match: %s", elem.value);
            ql_verify(ql, 157, 5001, 32, 9);
            quicklist_free(ql);
        }

        TEST("numbers larger list read B") {
            struct quicklist *ql = quicklist_new(-2, options[_i]);
            quicklist_push_tail(ql, "99", 2);
            quicklist_push_tail(ql, "98", 2);
            quicklist_push_tail(ql, "xxxxxxxxxxxxxxxxxxxx", 20);
            quicklist_push_tail(ql, "96", 2);
            quicklist_push_tail(ql, "95", 2);
            quicklist_replace(ql, 1, "foo", 3);
            quicklist_replace(ql, -1, "bar", 3);
            quicklist_free(ql);
        }

        TEST_DESC("lrem test at compress %d", options[_i]) {
            for (int f = 0; f < fill_count; f++) {
                struct quicklist *ql = quicklist_new(fills[f], options[_i]);
                char *words[] = {"abc", "foo", "bar",  "foobar", "foobared",
                                 "zap", "bar", "test", "foo"};
                char *result[] = {"abc", "foo",  "foobar", "foobared",
                                  "zap", "test", "foo"};
                char *resultB[] = {"abc",      "foo", "foobar",
                                   "foobared", "zap", "test"};
                for (int i = 0; i < 9; i++)
                    quicklist_push_tail(ql, words[i], strlen(words[i]));

                /* lrem 0 bar */
                struct quicklist_iter *iter = quicklist_iter_new_ahead(ql, 0, 1);
                struct quicklist_element elem;
                int i = 0;
                while (quicklist_iter_next(iter)) {
                    if (quicklist_iter_equal(iter, (unsigned char *)"bar", 3)) {
                        quicklist_iter_del(iter);
                    }
                    i++;
                }
                ql_release_iterator(iter);

                /* check result of lrem 0 bar */
                iter = quicklist_iter_new_ahead(ql, 0, 1);
                i = 0;
                while (quicklist_iter_next(iter)) {
		    quicklist_iter_get_element(iter, &elem);
		    int sz = elem.sz;
                    /* Result must be: abc, foo, foobar, foobared, zap, test,
                     * foo */
                    if (strncmp((char *)elem.value, result[i], sz)) {
                        ERR("No match at position %d, got %.*s instead of %s",
                            i, sz, elem.value, result[i]);
                    }
                    i++;
                }
                ql_release_iterator(iter);

                quicklist_push_tail(ql, "foo", 3);

                /* lrem -2 foo */
                iter = quicklist_iter_new_ahead(ql, -1, 0);
                i = 0;
                int del = 2;
                while (quicklist_iter_next(iter)) {
                    if (quicklist_iter_equal(iter, (unsigned char *)"foo", 3)) {
                        quicklist_iter_del(iter);
                        del--;
                    }
                    if (!del)
                        break;
                    i++;
                }
                ql_release_iterator(iter);

                /* check result of lrem -2 foo */
                /* (we're ignoring the '2' part and still deleting all foo
                 * because
                 * we only have two foo) */
                iter = quicklist_iter_new_ahead(ql, -1, 0);
                i = 0;
                size_t resB = sizeof(resultB) / sizeof(*resultB);
                while (quicklist_iter_next(iter)) {
                    /* Result must be: abc, foo, foobar, foobared, zap, test,
                     * foo */
		    quicklist_iter_get_element(iter, &elem);
		    int sz = elem.sz;
                    if (strncmp((char *)elem.value, resultB[resB - 1 - i], sz)) {
                        ERR("No match at position %d, got %.*s instead of %s",
                            i, sz, elem.value, resultB[resB - 1 - i]);
                    }
                    i++;
                }

                ql_release_iterator(iter);
                quicklist_free(ql);
            }
        }

        TEST_DESC("iterate reverse + delete at compress %d", options[_i]) {
            for (int f = 0; f < fill_count; f++) {
                struct quicklist *ql = quicklist_new(fills[f], options[_i]);
                quicklist_push_tail(ql, "abc", 3);
                quicklist_push_tail(ql, "def", 3);
                quicklist_push_tail(ql, "hij", 3);
                quicklist_push_tail(ql, "jkl", 3);
                quicklist_push_tail(ql, "oop", 3);

                struct quicklist_iter *iter = quicklist_iter_new_ahead(ql, -1, 0);
                int i = 0;
                while (quicklist_iter_next(iter)) {
                    if (quicklist_iter_equal(iter, (unsigned char *)"hij", 3)) {
                        quicklist_iter_del(iter);
                    }
                    i++;
                }
                ql_release_iterator(iter);

                if (i != 5)
                    ERR("Didn't iterate 5 times, iterated %d times.", i);

                /* Check results after deletion of "hij" */
                iter = quicklist_iter_new_ahead(ql, 0, 1);
                i = 0;
                char *vals[] = {"abc", "def", "jkl", "oop"};
                while (quicklist_iter_next(iter)) {
                    if (!quicklist_iter_equal(iter, (unsigned char *)vals[i], 3)) {
                        ERR("Value at %d didn't match %s\n", i, vals[i]);
                    }
                    i++;
                }
                ql_release_iterator(iter);
                quicklist_free(ql);
            }
        }

        TEST_DESC("iterator at index test at compress %d", options[_i]) {
            for (int f = 0; f < fill_count; f++) {
                struct quicklist *ql = quicklist_new(fills[f], options[_i]);
                char num[32];
                long long nums[5000];
                for (int i = 0; i < 760; i++) {
                    nums[i] = -5157318210846258176 + i;
                    int sz = ll2string(num, sizeof(num), nums[i]);
                    quicklist_push_tail(ql, num, sz);
                }

                struct quicklist_element elem;
                struct quicklist_iter *iter = quicklist_iter_new_ahead(ql, 437, 1);
                int i = 437;
                while (quicklist_iter_next(iter)) {
		    quicklist_iter_get_element(iter, &elem);
                    if (!elem.value && elem.longval != nums[i])
                        ERR("Expected %lld, but got %lld", nums[i], elem.longval);
                    i++;
                }
                ql_release_iterator(iter);
                quicklist_free(ql);
            }
        }

        TEST_DESC("ltrim test A at compress %d", options[_i]) {
            for (int f = 0; f < fill_count; f++) {
                struct quicklist *ql = quicklist_new(fills[f], options[_i]);
                char num[32];
                long long nums[5000];
                for (int i = 0; i < 32; i++) {
                    nums[i] = -5157318210846258176 + i;
                    int sz = ll2string(num, sizeof(num), nums[i]);
                    quicklist_push_tail(ql, num, sz);
                }
                if (fills[f] == 32)
                    ql_verify(ql, 1, 32, 32, 32);
                /* ltrim 25 53 (keep [25,32] inclusive = 7 remaining) */
                quicklist_del(ql, 0, 25);
                struct quicklist_element elem;
                for (int i = 0; i < 7; i++) {
		    quicklist_get_element(ql, i, &elem);
                    if (!elem.value && elem.longval != nums[25 + i])
                        ERR("Deleted invalid range!  Expected %lld but got "
                            "%lld",
                            elem.longval, nums[25 + i]);
                }
                if (fills[f] == 32)
                    ql_verify(ql, 1, 7, 7, 7);
                quicklist_free(ql);
            }
        }

        TEST_DESC("ltrim test B at compress %d", options[_i]) {
            for (int f = 0; f < fill_count; f++) {
                /* Force-disable compression because our 33 sequential
                 * integers don't compress and the check always fails. */
                struct quicklist *ql = quicklist_new(fills[f], 0);
                char num[32];
                long long nums[5000];
                for (int i = 0; i < 33; i++) {
                    nums[i] = i;
                    int sz = ll2string(num, sizeof(num), nums[i]);
                    quicklist_push_tail(ql, num, sz);
                }
                if (fills[f] == 32)
                    ql_verify(ql, 2, 33, 32, 1);
                /* ltrim 5 16 (keep [5,16] inclusive = 12 remaining) */
                quicklist_del(ql, 0, 5);
                quicklist_del(ql, -16, 16);
                if (fills[f] == 32)
                    ql_verify(ql, 1, 12, 12, 12);
                struct quicklist_element elem;

		quicklist_get_element(ql, 0, &elem);
                if (!elem.value && elem.longval != 5)
                    ERR("A: longval not 5, but %lld", elem.longval);

                quicklist_get_element(ql, -1, &elem);
                if (!elem.value && elem.longval != 16)
                    ERR("B! got instead: %lld", elem.longval);
                quicklist_push_tail(ql, "bobobob", 7);

                quicklist_get_element(ql, -1, &elem);
		int sz = elem.sz;
                if (strncmp((char *)elem.value, "bobobob", 7))
                    ERR("Tail doesn't match bobobob, it's %.*s instead",
                        sz, elem.value);

                for (int i = 0; i < 12; i++) {
                    quicklist_get_element(ql, i, &elem);
                    if (!elem.value && elem.longval != nums[5 + i])
                        ERR("Deleted invalid range!  Expected %lld but got "
                            "%lld",
                            elem.longval, nums[5 + i]);
                }
                quicklist_free(ql);
            }
        }

        TEST_DESC("ltrim test C at compress %d", options[_i]) {
            for (int f = 0; f < fill_count; f++) {
                struct quicklist *ql = quicklist_new(fills[f], options[_i]);
                char num[32];
                long long nums[5000];
                for (int i = 0; i < 33; i++) {
                    nums[i] = -5157318210846258176 + i;
                    int sz = ll2string(num, sizeof(num), nums[i]);
                    quicklist_push_tail(ql, num, sz);
                }
                if (fills[f] == 32)
                    ql_verify(ql, 2, 33, 32, 1);
                /* ltrim 3 3 (keep [3,3] inclusive = 1 remaining) */
                quicklist_del(ql, 0, 3);
                quicklist_del(ql, -29, 4000); /* make sure not loop forever */
                if (fills[f] == 32)
                    ql_verify(ql, 1, 1, 1, 1);
                struct quicklist_element elem;
                quicklist_get_element(ql, 0, &elem);
                if (elem.longval != -5157318210846258173)
                    ERROR;
                quicklist_free(ql);
            }
        }

        TEST_DESC("ltrim test D at compress %d", options[_i]) {
            for (int f = 0; f < fill_count; f++) {
                struct quicklist *ql = quicklist_new(fills[f], options[_i]);
                char num[32];
                long long nums[5000];
                for (int i = 0; i < 33; i++) {
                    nums[i] = -5157318210846258176 + i;
                    int sz = ll2string(num, sizeof(num), nums[i]);
                    quicklist_push_tail(ql, num, sz);
                }
                if (fills[f] == 32)
                    ql_verify(ql, 2, 33, 32, 1);
                quicklist_del(ql, -12, 3);
                if (ql->count != 30)
                    ERR("Didn't delete exactly three elements!  Count is: %lu",
                        ql->count);
                quicklist_free(ql);
            }
        }

        long long stop = mstime();
        runtime[_i] = stop - start;
    }

    /* Run a longer test of compression depth outside of primary test loop. */
    int list_sizes[] = {250, 251, 500, 999, 1000};
    long long start = mstime();
    int list_count = accurate ? (int)(sizeof(list_sizes) / sizeof(*list_sizes)) : 1;
    for (int list = 0; list < list_count; list++) {
        TEST_DESC("verify specific compression of interior nodes with %d list ",
                  list_sizes[list]) {
            for (int f = 0; f < fill_count; f++) {
                for (int depth = 1; depth < 40; depth++) {
                    /* skip over many redundant test cases */
                    struct quicklist *ql = quicklist_new(fills[f], depth);
                    for (int i = 0; i < list_sizes[list]; i++) {
                        quicklist_push_tail(ql, genstr("hello TAIL", i + 1), 64);
                        quicklist_push_head(ql, genstr("hello HEAD", i + 1), 64);
                    }

                    for (int step = 0; step < 2; step++) {
                        /* test remove node */
                        if (step == 1) {
                            for (int i = 0; i < list_sizes[list] / 2; i++) {
                                assert(quicklist_del(ql, 0, 1));
                                assert(quicklist_del(ql, -1, 1));
                            }
                        }

			err += ql_verify_compress(ql);
                    }

                    quicklist_free(ql);
                }
            }
        }
    }
    long long stop = mstime();

    printf("\n");
    for (size_t i = 0; i < option_count; i++)
        printf("Test Loop %02d: %0.2f seconds.\n", options[i],
               (float)runtime[i] / 1000);
    printf("Compressions: %0.2f seconds.\n", (float)(stop - start) / 1000);
    printf("\n");

    TEST("bookmark get updated to next item") {
        struct quicklist *ql = quicklist_new(1, 0);
        quicklist_push_tail(ql, "1", 1);
        quicklist_push_tail(ql, "2", 1);
        quicklist_push_tail(ql, "3", 1);
        quicklist_push_tail(ql, "4", 1);
        quicklist_push_tail(ql, "5", 1);
        assert(quicklist_node_count(ql) == 5);

	struct quicklist_partition *head_p, *tail_p;
	struct quicklist_node *head, *tail;
	quicklist_first_node(ql, &head_p, &head);
	quicklist_last_node(ql, &tail_p, &tail);

	struct quicklist_partition *head_next_p, *tail_prev_p;
	struct quicklist_node *head_next, *tail_prev;
	quicklist_next(head_p, head, &head_next_p, &head_next);
	quicklist_prev(tail_p, tail, &tail_prev_p, &tail_prev);
	assert(head_next);
	assert(tail_prev);

        /* add two bookmarks, one pointing to the node before the last. */
        assert(quicklist_bm_create(&ql, "_dummy", head_next));
        assert(quicklist_bm_create(&ql, "_test", tail_prev));
        /* test that the bookmark returns the right node, delete it and see that the bookmark points to the last node */
        assert(quicklist_bm_find(ql, "_test") == tail_prev);
        assert(quicklist_del(ql, -2, 1));
	quicklist_last_node(ql, &tail_p, &tail);
        assert(quicklist_bm_find(ql, "_test") == tail);
        /* delete the last node, and see that the bookmark was deleted. */
        assert(quicklist_del(ql, -1, 1));
        assert(quicklist_bm_find(ql, "_test") == NULL);
        /* test that other bookmarks aren't affected */
	quicklist_first_node(ql, &head_p, &head);
	quicklist_next(head_p, head, &head_next_p, &head_next);
        assert(quicklist_bm_find(ql, "_dummy") == head_next);
        assert(quicklist_bm_find(ql, "_missing") == NULL);
        assert(quicklist_node_count(ql) == 3);
        quicklist_bm_clear(ql); /* for coverage */
        assert(quicklist_bm_find(ql, "_dummy") == NULL);
        quicklist_free(ql);
    }

    TEST("bookmark limit") {
        int i;
        struct quicklist *ql = quicklist_new(1, 0);
        quicklist_push_head(ql, "1", 1);
	struct quicklist_partition *head_p;
	struct quicklist_node *head;
	quicklist_first_node(ql, &head_p, &head);
        for (i=0; i<QUICKLIST_MAX_BM; i++)
            assert(quicklist_bm_create(&ql, genstr("",i), head));
        /* when all bookmarks are used, creation fails */
        assert(!quicklist_bm_create(&ql, "_test", head));
        /* delete one and see that we can now create another */
        assert(quicklist_bm_delete(ql, "0"));
        assert(quicklist_bm_create(&ql, "_test", head));
        /* delete one and see that the rest survive */
        assert(quicklist_bm_delete(ql, "_test"));
	quicklist_first_node(ql, &head_p, &head);
        for (i=1; i<QUICKLIST_MAX_BM; i++)
            assert(quicklist_bm_find(ql, genstr("",i)) == head);
        /* make sure the deleted ones are indeed gone */
        assert(!quicklist_bm_find(ql, "0"));
        assert(!quicklist_bm_find(ql, "_test"));
        quicklist_free(ql);
    }

    if (flags & REDIS_TEST_LARGE_MEMORY) {
        TEST("compress and decompress quicklist listpack node") {
		struct quicklist_node *node = zmalloc(sizeof(*node));
		node->carry = lpNew(0);
		node->raw_sz = lpBytes(node->carry);
		node->container = QUICKLIST_NODE_CONTAINER_PACKED;
		node->count = 0;
		node->raw = 1;
            
		/* Create a rand string */
		size_t sz = (1 << 25); /* 32MB per one entry */
		unsigned char *s = zmalloc(sz);
		randstring(s, sz);

		/* Keep filling the node, until it reaches 1GB */
		for (int i = 0; i < 32; i++) {
			node->carry = lpAppend(node->carry, s, sz);
			node->raw_sz = lpBytes(node->carry);

			long long start = mstime();
			quicklist_n_compress_raw(node);
			assert(quicklist_n_decompress(node));
			printf("Compress and decompress: %zu MB in %.2f seconds.\n",
			node->raw_sz/1024/1024, (float)(mstime() - start) / 1000);
		}

		zfree(s);
		quicklist_n_free(node);
        }

#if ULONG_MAX >= 0xffffffffffffffff
        TEST("compress and decomress quicklist plain node large than UINT32_MAX") {
		size_t sz = (1ull << 32);
		unsigned char *s = zmalloc(sz);
		randstring(s, sz);
		memcpy(s, "helloworld", 10);
		memcpy(s + sz - 10, "1234567890", 10);

		struct quicklist_node *node = zmalloc(sizeof(*node));
		node->carry = zmalloc(sz);
		memcpy(node->carry, s, sz);
		node->raw_sz = sz;
		node->container = QUICKLIST_NODE_CONTAINER_PLAIN;
		node->count = 1;
		node->raw = 1;

		long long start = mstime();
		quicklist_n_compress_raw(node);
		assert(quicklist_n_decompress(node));
		printf("Compress and decompress: %zu MB in %.2f seconds.\n",
			node->raw_sz/1024/1024, (float)(mstime() - start) / 1000);

		assert(memcmp(node->carry, "helloworld", 10) == 0);
		assert(memcmp((char *)node->carry + sz - 10, "1234567890", 10) == 0);
		zfree(s);
		quicklist_n_free(node);
        }
#endif
    }

    if (!err)
        printf("ALL TESTS PASSED!\n");
    else
        ERR("Sorry, not all tests passed!  In fact, %d tests failed.", err);

    return err;
}
#endif
