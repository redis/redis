#include "test/jemalloc_test.h"
#include "test/bench.h"

static void
mallctl_short(void) {
	const char *version;
	size_t sz = sizeof(version);
	int err = mallctl("version", &version, &sz, NULL, 0);
	assert_d_eq(err, 0, "mallctl failure");
}

size_t mib_short[1];

static void
mallctlbymib_short(void) {
	size_t miblen = sizeof(mib_short)/sizeof(mib_short[0]);
	const char *version;
	size_t sz = sizeof(version);
	int err = mallctlbymib(mib_short, miblen, &version, &sz, NULL, 0);
	assert_d_eq(err, 0, "mallctlbymib failure");
}

TEST_BEGIN(test_mallctl_vs_mallctlbymib_short) {
	size_t miblen = sizeof(mib_short)/sizeof(mib_short[0]);

	int err = mallctlnametomib("version", mib_short, &miblen);
	assert_d_eq(err, 0, "mallctlnametomib failure");
	compare_funcs(10*1000*1000, 10*1000*1000, "mallctl_short",
	    mallctl_short, "mallctlbymib_short", mallctlbymib_short);
}
TEST_END

static void
mallctl_long(void) {
	uint64_t nmalloc;
	size_t sz = sizeof(nmalloc);
	int err = mallctl("stats.arenas.0.bins.0.nmalloc", &nmalloc, &sz, NULL,
	    0);
	assert_d_eq(err, 0, "mallctl failure");
}

size_t mib_long[6];

static void
mallctlbymib_long(void) {
	size_t miblen = sizeof(mib_long)/sizeof(mib_long[0]);
	uint64_t nmalloc;
	size_t sz = sizeof(nmalloc);
	int err = mallctlbymib(mib_long, miblen, &nmalloc, &sz, NULL, 0);
	assert_d_eq(err, 0, "mallctlbymib failure");
}

TEST_BEGIN(test_mallctl_vs_mallctlbymib_long) {
	/*
	 * We want to use the longest mallctl we have; that needs stats support
	 * to be allowed.
	 */
	test_skip_if(!config_stats);

	size_t miblen = sizeof(mib_long)/sizeof(mib_long[0]);
	int err = mallctlnametomib("stats.arenas.0.bins.0.nmalloc", mib_long,
	    &miblen);
	assert_d_eq(err, 0, "mallctlnametomib failure");
	compare_funcs(10*1000*1000, 10*1000*1000, "mallctl_long",
	    mallctl_long, "mallctlbymib_long", mallctlbymib_long);
}
TEST_END

int
main(void) {
	return test_no_reentrancy(
	    test_mallctl_vs_mallctlbymib_short,
	    test_mallctl_vs_mallctlbymib_long);
}
