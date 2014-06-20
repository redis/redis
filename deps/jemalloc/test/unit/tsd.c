#include "test/jemalloc_test.h"

#define	THREAD_DATA 0x72b65c10

typedef unsigned int data_t;

static bool data_cleanup_executed;

void
data_cleanup(void *arg)
{
	data_t *data = (data_t *)arg;

	assert_x_eq(*data, THREAD_DATA,
	    "Argument passed into cleanup function should match tsd value");
	data_cleanup_executed = true;
}

malloc_tsd_protos(, data, data_t)
malloc_tsd_externs(data, data_t)
#define	DATA_INIT 0x12345678
malloc_tsd_data(, data, data_t, DATA_INIT)
malloc_tsd_funcs(, data, data_t, DATA_INIT, data_cleanup)

static void *
thd_start(void *arg)
{
	data_t d = (data_t)(uintptr_t)arg;
	assert_x_eq(*data_tsd_get(), DATA_INIT,
	    "Initial tsd get should return initialization value");

	data_tsd_set(&d);
	assert_x_eq(*data_tsd_get(), d,
	    "After tsd set, tsd get should return value that was set");

	d = 0;
	assert_x_eq(*data_tsd_get(), (data_t)(uintptr_t)arg,
	    "Resetting local data should have no effect on tsd");

	return (NULL);
}

TEST_BEGIN(test_tsd_main_thread)
{

	thd_start((void *) 0xa5f3e329);
}
TEST_END

TEST_BEGIN(test_tsd_sub_thread)
{
	thd_t thd;

	data_cleanup_executed = false;
	thd_create(&thd, thd_start, (void *)THREAD_DATA);
	thd_join(thd, NULL);
	assert_true(data_cleanup_executed,
	    "Cleanup function should have executed");
}
TEST_END

int
main(void)
{

	data_tsd_boot();

	return (test(
	    test_tsd_main_thread,
	    test_tsd_sub_thread));
}
