#include "test/jemalloc_test.h"
#include "test/bench.h"

#define MIBLEN 8
static size_t mib[MIBLEN];
static size_t miblen = MIBLEN;

#define TINY_BATCH 10
#define TINY_BATCH_ITER (10 * 1000 * 1000)
#define HUGE_BATCH (1000 * 1000)
#define HUGE_BATCH_ITER 100
#define LEN (100 * 1000 * 1000)
static void *batch_ptrs[LEN];
static size_t batch_ptrs_next = 0;
static void *item_ptrs[LEN];
static size_t item_ptrs_next = 0;

#define SIZE 7

typedef struct batch_alloc_packet_s batch_alloc_packet_t;
struct batch_alloc_packet_s {
	void **ptrs;
	size_t num;
	size_t size;
	int flags;
};

static void
batch_alloc_wrapper(size_t batch) {
	batch_alloc_packet_t batch_alloc_packet =
	    {batch_ptrs + batch_ptrs_next, batch, SIZE, 0};
	size_t filled;
	size_t len = sizeof(size_t);
	assert_d_eq(mallctlbymib(mib, miblen, &filled, &len,
	    &batch_alloc_packet, sizeof(batch_alloc_packet)), 0, "");
	assert_zu_eq(filled, batch, "");
}

static void
item_alloc_wrapper(size_t batch) {
	for (size_t i = item_ptrs_next, end = i + batch; i < end; ++i) {
		item_ptrs[i] = malloc(SIZE);
	}
}

static void
release_and_clear(void **ptrs, size_t len) {
	for (size_t i = 0; i < len; ++i) {
		void *p = ptrs[i];
		assert_ptr_not_null(p, "allocation failed");
		sdallocx(p, SIZE, 0);
		ptrs[i] = NULL;
	}
}

static void
batch_alloc_without_free(size_t batch) {
	batch_alloc_wrapper(batch);
	batch_ptrs_next += batch;
}

static void
item_alloc_without_free(size_t batch) {
	item_alloc_wrapper(batch);
	item_ptrs_next += batch;
}

static void
batch_alloc_with_free(size_t batch) {
	batch_alloc_wrapper(batch);
	release_and_clear(batch_ptrs + batch_ptrs_next, batch);
	batch_ptrs_next += batch;
}

static void
item_alloc_with_free(size_t batch) {
	item_alloc_wrapper(batch);
	release_and_clear(item_ptrs + item_ptrs_next, batch);
	item_ptrs_next += batch;
}

static void
compare_without_free(size_t batch, size_t iter,
    void (*batch_alloc_without_free_func)(void),
    void (*item_alloc_without_free_func)(void)) {
	assert(batch_ptrs_next == 0);
	assert(item_ptrs_next == 0);
	assert(batch * iter <= LEN);
	for (size_t i = 0; i < iter; ++i) {
		batch_alloc_without_free_func();
		item_alloc_without_free_func();
	}
	release_and_clear(batch_ptrs, batch_ptrs_next);
	batch_ptrs_next = 0;
	release_and_clear(item_ptrs, item_ptrs_next);
	item_ptrs_next = 0;
	compare_funcs(0, iter,
	    "batch allocation", batch_alloc_without_free_func,
	    "item allocation", item_alloc_without_free_func);
	release_and_clear(batch_ptrs, batch_ptrs_next);
	batch_ptrs_next = 0;
	release_and_clear(item_ptrs, item_ptrs_next);
	item_ptrs_next = 0;
}

static void
compare_with_free(size_t batch, size_t iter,
    void (*batch_alloc_with_free_func)(void),
    void (*item_alloc_with_free_func)(void)) {
	assert(batch_ptrs_next == 0);
	assert(item_ptrs_next == 0);
	assert(batch * iter <= LEN);
	for (size_t i = 0; i < iter; ++i) {
		batch_alloc_with_free_func();
		item_alloc_with_free_func();
	}
	batch_ptrs_next = 0;
	item_ptrs_next = 0;
	compare_funcs(0, iter,
	    "batch allocation", batch_alloc_with_free_func,
	    "item allocation", item_alloc_with_free_func);
	batch_ptrs_next = 0;
	item_ptrs_next = 0;
}

static void
batch_alloc_without_free_tiny() {
	batch_alloc_without_free(TINY_BATCH);
}

static void
item_alloc_without_free_tiny() {
	item_alloc_without_free(TINY_BATCH);
}

TEST_BEGIN(test_tiny_batch_without_free) {
	compare_without_free(TINY_BATCH, TINY_BATCH_ITER,
	    batch_alloc_without_free_tiny, item_alloc_without_free_tiny);
}
TEST_END

static void
batch_alloc_with_free_tiny() {
	batch_alloc_with_free(TINY_BATCH);
}

static void
item_alloc_with_free_tiny() {
	item_alloc_with_free(TINY_BATCH);
}

TEST_BEGIN(test_tiny_batch_with_free) {
	compare_with_free(TINY_BATCH, TINY_BATCH_ITER,
	    batch_alloc_with_free_tiny, item_alloc_with_free_tiny);
}
TEST_END

static void
batch_alloc_without_free_huge() {
	batch_alloc_without_free(HUGE_BATCH);
}

static void
item_alloc_without_free_huge() {
	item_alloc_without_free(HUGE_BATCH);
}

TEST_BEGIN(test_huge_batch_without_free) {
	compare_without_free(HUGE_BATCH, HUGE_BATCH_ITER,
	    batch_alloc_without_free_huge, item_alloc_without_free_huge);
}
TEST_END

static void
batch_alloc_with_free_huge() {
	batch_alloc_with_free(HUGE_BATCH);
}

static void
item_alloc_with_free_huge() {
	item_alloc_with_free(HUGE_BATCH);
}

TEST_BEGIN(test_huge_batch_with_free) {
	compare_with_free(HUGE_BATCH, HUGE_BATCH_ITER,
	    batch_alloc_with_free_huge, item_alloc_with_free_huge);
}
TEST_END

int main(void) {
	assert_d_eq(mallctlnametomib("experimental.batch_alloc", mib, &miblen),
	    0, "");
	return test_no_reentrancy(
	    test_tiny_batch_without_free,
	    test_tiny_batch_with_free,
	    test_huge_batch_without_free,
	    test_huge_batch_with_free);
}
