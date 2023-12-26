/* quicklist.h - A generic doubly linked quicklist implementation
 *
 * Copyright (c) 2014, Matt Stancliff <matt@genges.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
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

#include <stdint.h> // for UINTPTR_MAX
#include <assert.h>

#ifndef __QUICKLIST_H__
#define __QUICKLIST_H__

#ifndef static_assert
#define static_assert(expr, lit) extern char __static_assert_failure[(expr) ? 1:-1]
#endif

#if UINTPTR_MAX == 0xffffffff
/* 32-bit */
#define QUICKLIST_FILL_BITS 14
#elif UINTPTR_MAX == 0xffffffffffffffff
/* 64-bit */
#define QUICKLIST_FILL_BITS 16
#else
#error unknown arch bits count
#endif

#define QUICKLIST_NODE_CONTAINER_BITS 2
#define QUICKLIST_NODE_RAW_BITS 1
static_assert(QUICKLIST_NODE_CONTAINER_BITS + 
	      QUICKLIST_NODE_RAW_BITS +
	      QUICKLIST_FILL_BITS <= 32,
	      "overflow unsigned int");

#define QUICKLIST_NODE_CONTAINER_PLAIN 1
#define QUICKLIST_NODE_CONTAINER_PACKED 2

/**
 * struct quicklist_node - Circular doubly linked list structure.
 * @carry: the data that the node holds. It can be plain data or listpack data
 * or compressed plain data or compressed listpack data depends on @container
 * and @raw.
 * @raw_sz: the size in bytes of the data that the node holds before compressed.
 * @container: the type of container that holds the data.
 * @count: number of elements in the node.
 * @raw: 1 for raw node, 0 for lzf compressed node.
 */
struct quicklist_node {
	struct quicklist_node *prev;
	struct quicklist_node *next;
	void *carry;
	size_t raw_sz;
	unsigned int container : QUICKLIST_NODE_CONTAINER_BITS;
	unsigned int count : QUICKLIST_FILL_BITS;
	unsigned int raw : QUICKLIST_NODE_RAW_BITS;
};

#define QUICKLIST_P_HEAD 0
#define QUICKLIST_P_MIDDLE 1
#define QUICKLIST_P_TAIL 2

/**
 * struct quicklist_partition - Circular doubly linked list structure.
 * @which: which partition of the quicklist
 * @capacity: the maximum number of nodes this partition can hold, it's 
 * related with quicklist's compress strategy.
 * @guard: circular doubly linked quicklist_node. It self is not a valid
 * quicklist_node, it's a guard node.
 * @length: number of nodes in the partition.
 */
struct quicklist_partition {
	int which;
	long capacity;
	struct quicklist_node *guard;
	struct quicklist_partition *prev;
	struct quicklist_partition *next;

	long length;
};


#define QUICKLIST_PACK_SIZE_BITS 16
static_assert(QUICKLIST_FILL_BITS+QUICKLIST_PACK_SIZE_BITS <= 32,
		"overflow unsigned int");

/**
 * struct quicklist_fill - The limitation of a packed node.
 * @pack_max_count: the maximum number of elements that a packed node can hold
 * @pack_max_size: the maximum size in bytes that a packed node can reach
 */
struct quicklist_fill {
	unsigned int pack_max_count : QUICKLIST_FILL_BITS;
	unsigned int pack_max_size : QUICKLIST_PACK_SIZE_BITS;
};


struct quicklist_bookmark {
	struct quicklist_node *node;
    	char *name;
};

/**
 * struct quicklist
 * @head: circular doubly linked quicklist_partition, has three partitions,
 * start with head, next middle, and next tail.
 * @fill: the limitation of a packed node.
 * @count: the number of nodes in the quicklist.
 * @bookmark_count: the number of bookmarks of the quicklist
 * @bookmarks: the bookmarks for quick reach the target node. Note it that if
 * the target node is deleted, it will update to the next node, if it is the
 * last node, the bookmark will be deleted.
 * 
 * Note: there are three strategies the quicklist should keep.
 * 
 * compress strategy - every partition should not overflow, and the middle
 * partition should keep empty until the head and tail partition is full.
 * 
 * partition compress strategy - the node in head and tail partition should keep
 * uncompressed. The node in middle partition should be compressed unless it can
 * not compress small enough.
 * 
 * merge strategy - any adjacent node in quicklist can not be merged.
 */
struct quicklist {
	struct quicklist_partition *head;
	struct quicklist_fill *fill;
	long count;
	int bookmark_count;
	struct quicklist_bookmark bookmarks[];
};

/**
 * quicklist_lzf - A lzf compressed data structure.
 * @sz: the size in bytes of @compressed
 * @compressed: the compressed data
 */
struct quicklist_lzf {
	size_t sz;
	char compressed[];
};

/**
 * QUICKLIST_ITER_STATUS_AHEAD
 * In this status, the iterator is invalid until quicklist_iter_next() is called,
 * then the iterator will point to the target element.
 */
#define QUICKLIST_ITER_STATUS_AHEAD 0

/**
 * QUICKLIST_ITER_STATUS_NORMAL
 * In this status, the iterator is point to the target element.
 */
#define QUICKLIST_ITER_STATUS_NORMAL 1

/**
 * QUICKLIST_ITER_STATUS_COMPLETE
 * In this status, the iterator is finish its iteration, and is not available
 * for further use.
 */
#define QUICKLIST_ITER_STATUS_COMPLETE 2

/**
 * quicklist_iter - The quicklist element iterator structure.
 * @status: see QUICKLIST_ITER_STATUS_AHEAD, QUICKLIST_ITER_STATUS_NORMAL
 * and QUICKLIST_ITER_STATUS_COMPLETE.
 * @forward: 1 for iterate towards to tail, 0 for iterate towards to head.
 * @p: the partition that the iterator is current at
 * @raw_node: the node that the iterator is current at, it should not be compressed.
 * @offset: the index of the element in @node
 * @lp_element: the element address in listpack, only valid if @raw_node is a
 * packed node. It's used to quick iterate in listpack.
 */
struct quicklist_iter {
	int status;
	int forward;
	struct quicklist *quicklist;

	struct quicklist_partition *p;
	struct quicklist_node *raw_node;
	int offset;
	
	unsigned char *lp_element;
};

/**
 * quicklist_element - The quicklist element structure.
 * @value: the string value of the element, NULL if the element is a integer
 * value. Note that it is a reference to the element in quicklist.
 * @sz: the size in bytes of the string value of the element. If the
 * element is a integer value, it is undefined.
 * @longval: the integer value of the element. If the
 * element is a string value, it is undefined.
 */
struct quicklist_element {
	unsigned char *value;
	size_t sz;
	long long longval;
};

struct quicklist *quicklist_new(int fill, int compress);
void quicklist_free(struct quicklist *quicklist);
void quicklist_push_head(struct quicklist *quicklist, void *value, size_t sz);
void quicklist_push_tail(struct quicklist *quicklist, void *value, size_t sz);
void quicklist_append_listpack(struct quicklist *quicklist, unsigned char *zl);
void quicklist_append_plain(struct quicklist *quicklist, void *value, size_t sz);
void quicklist_iter_add_after(struct quicklist_iter *iter, void *value, size_t sz);
void quicklist_iter_add_before(struct quicklist_iter *iter, void *value, size_t sz);
void quicklist_iter_del(struct quicklist_iter *iter);
void quicklist_iter_replace(struct quicklist_iter *iter, void *value, size_t sz);
int quicklist_replace(struct quicklist *quicklist, long i, void *value, size_t sz);
long quicklist_del(struct quicklist *quicklist, long from, long n);
void quicklist_first_node(struct quicklist *quicklist,
		struct quicklist_partition **p, struct quicklist_node **node);
void quicklist_next(struct quicklist_partition *p, struct quicklist_node *node,
	struct quicklist_partition **next_p, struct quicklist_node **next_node);
struct quicklist_node *quicklist_next_for_bookmark(
		struct quicklist *quicklist, struct quicklist_node *node);
struct quicklist_iter *quicklist_iter_new(struct quicklist *quicklist,
						long i, int forward);
struct quicklist_iter *quicklist_iter_new_ahead(struct quicklist *quicklist,
						long i, int forward);
int quicklist_iter_next(struct quicklist_iter *iter);
void quicklist_iter_free(struct quicklist_iter *iter);
struct quicklist *quicklist_dup(struct quicklist *quicklist);
long quicklist_count(struct quicklist *quicklist);
long quicklist_node_count(struct quicklist *quicklist);
int quicklist_iter_equal(struct quicklist_iter *iter, void *value, size_t sz);
void quicklist_debug_print(struct quicklist *quicklist, int full);
void quicklist_iter_debug_print(struct quicklist_iter *iter);
int quicklist_bm_create(struct quicklist **ql_ref, char *name, struct quicklist_node *node);
int quicklist_bm_delete(struct quicklist *quicklist, char *name);
struct quicklist_node *quicklist_bm_find(struct quicklist *quicklist, char *name);
void quicklist_iter_get_element(struct quicklist_iter *iter, struct quicklist_element *elem);
struct quicklist_fill *quicklist_fill_new(int fill);

#ifdef REDIS_TEST
int quicklistTest(int argc, char *argv[], int flags);
#endif

#endif /* __QUICKLIST_H__ */
