#include "test/jemalloc_test.h"

static unsigned		test_count = 0;
static test_status_t	test_counts[test_status_count] = {0, 0, 0};
static test_status_t	test_status = test_status_pass;
static const char *	test_name = "";

JEMALLOC_FORMAT_PRINTF(1, 2)
void
test_skip(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	malloc_vcprintf(NULL, NULL, format, ap);
	va_end(ap);
	malloc_printf("\n");
	test_status = test_status_skip;
}

JEMALLOC_FORMAT_PRINTF(1, 2)
void
test_fail(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	malloc_vcprintf(NULL, NULL, format, ap);
	va_end(ap);
	malloc_printf("\n");
	test_status = test_status_fail;
}

static const char *
test_status_string(test_status_t test_status)
{

	switch (test_status) {
	case test_status_pass: return "pass";
	case test_status_skip: return "skip";
	case test_status_fail: return "fail";
	default: not_reached();
	}
}

void
p_test_init(const char *name)
{

	test_count++;
	test_status = test_status_pass;
	test_name = name;
}

void
p_test_fini(void)
{

	test_counts[test_status]++;
	malloc_printf("%s: %s\n", test_name, test_status_string(test_status));
}

static test_status_t
p_test_impl(bool do_malloc_init, test_t *t, va_list ap)
{
	test_status_t ret;

	if (do_malloc_init) {
		/*
		 * Make sure initialization occurs prior to running tests.
		 * Tests are special because they may use internal facilities
		 * prior to triggering initialization as a side effect of
		 * calling into the public API.
		 */
		if (nallocx(1, 0) == 0) {
			malloc_printf("Initialization error");
			return (test_status_fail);
		}
	}

	ret = test_status_pass;
	for (; t != NULL; t = va_arg(ap, test_t *)) {
		t();
		if (test_status > ret)
			ret = test_status;
	}

	malloc_printf("--- %s: %u/%u, %s: %u/%u, %s: %u/%u ---\n",
	    test_status_string(test_status_pass),
	    test_counts[test_status_pass], test_count,
	    test_status_string(test_status_skip),
	    test_counts[test_status_skip], test_count,
	    test_status_string(test_status_fail),
	    test_counts[test_status_fail], test_count);

	return (ret);
}

test_status_t
p_test(test_t *t, ...)
{
	test_status_t ret;
	va_list ap;

	ret = test_status_pass;
	va_start(ap, t);
	ret = p_test_impl(true, t, ap);
	va_end(ap);

	return (ret);
}

test_status_t
p_test_no_malloc_init(test_t *t, ...)
{
	test_status_t ret;
	va_list ap;

	ret = test_status_pass;
	va_start(ap, t);
	ret = p_test_impl(false, t, ap);
	va_end(ap);

	return (ret);
}

void
p_test_fail(const char *prefix, const char *message)
{

	malloc_cprintf(NULL, NULL, "%s%s\n", prefix, message);
	test_status = test_status_fail;
}
