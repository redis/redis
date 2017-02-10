#include "test/jemalloc_test.h"

#define	TEST_POW2_CEIL(t, suf, pri) do {				\
	unsigned i, pow2;						\
	t x;								\
									\
	assert_##suf##_eq(pow2_ceil_##suf(0), 0, "Unexpected result");	\
									\
	for (i = 0; i < sizeof(t) * 8; i++) {				\
		assert_##suf##_eq(pow2_ceil_##suf(((t)1) << i), ((t)1)	\
		    << i, "Unexpected result");				\
	}								\
									\
	for (i = 2; i < sizeof(t) * 8; i++) {				\
		assert_##suf##_eq(pow2_ceil_##suf((((t)1) << i) - 1),	\
		    ((t)1) << i, "Unexpected result");			\
	}								\
									\
	for (i = 0; i < sizeof(t) * 8 - 1; i++) {			\
		assert_##suf##_eq(pow2_ceil_##suf((((t)1) << i) + 1),	\
		    ((t)1) << (i+1), "Unexpected result");		\
	}								\
									\
	for (pow2 = 1; pow2 < 25; pow2++) {				\
		for (x = (((t)1) << (pow2-1)) + 1; x <= ((t)1) << pow2;	\
		    x++) {						\
			assert_##suf##_eq(pow2_ceil_##suf(x),		\
			    ((t)1) << pow2,				\
			    "Unexpected result, x=%"pri, x);		\
		}							\
	}								\
} while (0)

TEST_BEGIN(test_pow2_ceil_u64)
{

	TEST_POW2_CEIL(uint64_t, u64, FMTu64);
}
TEST_END

TEST_BEGIN(test_pow2_ceil_u32)
{

	TEST_POW2_CEIL(uint32_t, u32, FMTu32);
}
TEST_END

TEST_BEGIN(test_pow2_ceil_zu)
{

	TEST_POW2_CEIL(size_t, zu, "zu");
}
TEST_END

TEST_BEGIN(test_malloc_strtoumax_no_endptr)
{
	int err;

	set_errno(0);
	assert_ju_eq(malloc_strtoumax("0", NULL, 0), 0, "Unexpected result");
	err = get_errno();
	assert_d_eq(err, 0, "Unexpected failure");
}
TEST_END

TEST_BEGIN(test_malloc_strtoumax)
{
	struct test_s {
		const char *input;
		const char *expected_remainder;
		int base;
		int expected_errno;
		const char *expected_errno_name;
		uintmax_t expected_x;
	};
#define	ERR(e)		e, #e
#define	KUMAX(x)	((uintmax_t)x##ULL)
#define	KSMAX(x)	((uintmax_t)(intmax_t)x##LL)
	struct test_s tests[] = {
		{"0",		"0",	-1,	ERR(EINVAL),	UINTMAX_MAX},
		{"0",		"0",	1,	ERR(EINVAL),	UINTMAX_MAX},
		{"0",		"0",	37,	ERR(EINVAL),	UINTMAX_MAX},

		{"",		"",	0,	ERR(EINVAL),	UINTMAX_MAX},
		{"+",		"+",	0,	ERR(EINVAL),	UINTMAX_MAX},
		{"++3",		"++3",	0,	ERR(EINVAL),	UINTMAX_MAX},
		{"-",		"-",	0,	ERR(EINVAL),	UINTMAX_MAX},

		{"42",		"",	0,	ERR(0),		KUMAX(42)},
		{"+42",		"",	0,	ERR(0),		KUMAX(42)},
		{"-42",		"",	0,	ERR(0),		KSMAX(-42)},
		{"042",		"",	0,	ERR(0),		KUMAX(042)},
		{"+042",	"",	0,	ERR(0),		KUMAX(042)},
		{"-042",	"",	0,	ERR(0),		KSMAX(-042)},
		{"0x42",	"",	0,	ERR(0),		KUMAX(0x42)},
		{"+0x42",	"",	0,	ERR(0),		KUMAX(0x42)},
		{"-0x42",	"",	0,	ERR(0),		KSMAX(-0x42)},

		{"0",		"",	0,	ERR(0),		KUMAX(0)},
		{"1",		"",	0,	ERR(0),		KUMAX(1)},

		{"42",		"",	0,	ERR(0),		KUMAX(42)},
		{" 42",		"",	0,	ERR(0),		KUMAX(42)},
		{"42 ",		" ",	0,	ERR(0),		KUMAX(42)},
		{"0x",		"x",	0,	ERR(0),		KUMAX(0)},
		{"42x",		"x",	0,	ERR(0),		KUMAX(42)},

		{"07",		"",	0,	ERR(0),		KUMAX(7)},
		{"010",		"",	0,	ERR(0),		KUMAX(8)},
		{"08",		"8",	0,	ERR(0),		KUMAX(0)},
		{"0_",		"_",	0,	ERR(0),		KUMAX(0)},

		{"0x",		"x",	0,	ERR(0),		KUMAX(0)},
		{"0X",		"X",	0,	ERR(0),		KUMAX(0)},
		{"0xg",		"xg",	0,	ERR(0),		KUMAX(0)},
		{"0XA",		"",	0,	ERR(0),		KUMAX(10)},

		{"010",		"",	10,	ERR(0),		KUMAX(10)},
		{"0x3",		"x3",	10,	ERR(0),		KUMAX(0)},

		{"12",		"2",	2,	ERR(0),		KUMAX(1)},
		{"78",		"8",	8,	ERR(0),		KUMAX(7)},
		{"9a",		"a",	10,	ERR(0),		KUMAX(9)},
		{"9A",		"A",	10,	ERR(0),		KUMAX(9)},
		{"fg",		"g",	16,	ERR(0),		KUMAX(15)},
		{"FG",		"G",	16,	ERR(0),		KUMAX(15)},
		{"0xfg",	"g",	16,	ERR(0),		KUMAX(15)},
		{"0XFG",	"G",	16,	ERR(0),		KUMAX(15)},
		{"z_",		"_",	36,	ERR(0),		KUMAX(35)},
		{"Z_",		"_",	36,	ERR(0),		KUMAX(35)}
	};
#undef ERR
#undef KUMAX
#undef KSMAX
	unsigned i;

	for (i = 0; i < sizeof(tests)/sizeof(struct test_s); i++) {
		struct test_s *test = &tests[i];
		int err;
		uintmax_t result;
		char *remainder;

		set_errno(0);
		result = malloc_strtoumax(test->input, &remainder, test->base);
		err = get_errno();
		assert_d_eq(err, test->expected_errno,
		    "Expected errno %s for \"%s\", base %d",
		    test->expected_errno_name, test->input, test->base);
		assert_str_eq(remainder, test->expected_remainder,
		    "Unexpected remainder for \"%s\", base %d",
		    test->input, test->base);
		if (err == 0) {
			assert_ju_eq(result, test->expected_x,
			    "Unexpected result for \"%s\", base %d",
			    test->input, test->base);
		}
	}
}
TEST_END

TEST_BEGIN(test_malloc_snprintf_truncated)
{
#define	BUFLEN	15
	char buf[BUFLEN];
	size_t result;
	size_t len;
#define	TEST(expected_str_untruncated, ...) do {			\
	result = malloc_snprintf(buf, len, __VA_ARGS__);		\
	assert_d_eq(strncmp(buf, expected_str_untruncated, len-1), 0,	\
	    "Unexpected string inequality (\"%s\" vs \"%s\")",		\
	    buf, expected_str_untruncated);				\
	assert_zu_eq(result, strlen(expected_str_untruncated),		\
	    "Unexpected result");					\
} while (0)

	for (len = 1; len < BUFLEN; len++) {
		TEST("012346789",	"012346789");
		TEST("a0123b",		"a%sb", "0123");
		TEST("a01234567",	"a%s%s", "0123", "4567");
		TEST("a0123  ",		"a%-6s", "0123");
		TEST("a  0123",		"a%6s", "0123");
		TEST("a   012",		"a%6.3s", "0123");
		TEST("a   012",		"a%*.*s", 6, 3, "0123");
		TEST("a 123b",		"a% db", 123);
		TEST("a123b",		"a%-db", 123);
		TEST("a-123b",		"a%-db", -123);
		TEST("a+123b",		"a%+db", 123);
	}
#undef BUFLEN
#undef TEST
}
TEST_END

TEST_BEGIN(test_malloc_snprintf)
{
#define	BUFLEN	128
	char buf[BUFLEN];
	size_t result;
#define	TEST(expected_str, ...) do {					\
	result = malloc_snprintf(buf, sizeof(buf), __VA_ARGS__);	\
	assert_str_eq(buf, expected_str, "Unexpected output");		\
	assert_zu_eq(result, strlen(expected_str), "Unexpected result");\
} while (0)

	TEST("hello", "hello");

	TEST("50%, 100%", "50%%, %d%%", 100);

	TEST("a0123b", "a%sb", "0123");

	TEST("a 0123b", "a%5sb", "0123");
	TEST("a 0123b", "a%*sb", 5, "0123");

	TEST("a0123 b", "a%-5sb", "0123");
	TEST("a0123b", "a%*sb", -1, "0123");
	TEST("a0123 b", "a%*sb", -5, "0123");
	TEST("a0123 b", "a%-*sb", -5, "0123");

	TEST("a012b", "a%.3sb", "0123");
	TEST("a012b", "a%.*sb", 3, "0123");
	TEST("a0123b", "a%.*sb", -3, "0123");

	TEST("a  012b", "a%5.3sb", "0123");
	TEST("a  012b", "a%5.*sb", 3, "0123");
	TEST("a  012b", "a%*.3sb", 5, "0123");
	TEST("a  012b", "a%*.*sb", 5, 3, "0123");
	TEST("a 0123b", "a%*.*sb", 5, -3, "0123");

	TEST("_abcd_", "_%x_", 0xabcd);
	TEST("_0xabcd_", "_%#x_", 0xabcd);
	TEST("_1234_", "_%o_", 01234);
	TEST("_01234_", "_%#o_", 01234);
	TEST("_1234_", "_%u_", 1234);

	TEST("_1234_", "_%d_", 1234);
	TEST("_ 1234_", "_% d_", 1234);
	TEST("_+1234_", "_%+d_", 1234);
	TEST("_-1234_", "_%d_", -1234);
	TEST("_-1234_", "_% d_", -1234);
	TEST("_-1234_", "_%+d_", -1234);

	TEST("_-1234_", "_%d_", -1234);
	TEST("_1234_", "_%d_", 1234);
	TEST("_-1234_", "_%i_", -1234);
	TEST("_1234_", "_%i_", 1234);
	TEST("_01234_", "_%#o_", 01234);
	TEST("_1234_", "_%u_", 1234);
	TEST("_0x1234abc_", "_%#x_", 0x1234abc);
	TEST("_0X1234ABC_", "_%#X_", 0x1234abc);
	TEST("_c_", "_%c_", 'c');
	TEST("_string_", "_%s_", "string");
	TEST("_0x42_", "_%p_", ((void *)0x42));

	TEST("_-1234_", "_%ld_", ((long)-1234));
	TEST("_1234_", "_%ld_", ((long)1234));
	TEST("_-1234_", "_%li_", ((long)-1234));
	TEST("_1234_", "_%li_", ((long)1234));
	TEST("_01234_", "_%#lo_", ((long)01234));
	TEST("_1234_", "_%lu_", ((long)1234));
	TEST("_0x1234abc_", "_%#lx_", ((long)0x1234abc));
	TEST("_0X1234ABC_", "_%#lX_", ((long)0x1234ABC));

	TEST("_-1234_", "_%lld_", ((long long)-1234));
	TEST("_1234_", "_%lld_", ((long long)1234));
	TEST("_-1234_", "_%lli_", ((long long)-1234));
	TEST("_1234_", "_%lli_", ((long long)1234));
	TEST("_01234_", "_%#llo_", ((long long)01234));
	TEST("_1234_", "_%llu_", ((long long)1234));
	TEST("_0x1234abc_", "_%#llx_", ((long long)0x1234abc));
	TEST("_0X1234ABC_", "_%#llX_", ((long long)0x1234ABC));

	TEST("_-1234_", "_%qd_", ((long long)-1234));
	TEST("_1234_", "_%qd_", ((long long)1234));
	TEST("_-1234_", "_%qi_", ((long long)-1234));
	TEST("_1234_", "_%qi_", ((long long)1234));
	TEST("_01234_", "_%#qo_", ((long long)01234));
	TEST("_1234_", "_%qu_", ((long long)1234));
	TEST("_0x1234abc_", "_%#qx_", ((long long)0x1234abc));
	TEST("_0X1234ABC_", "_%#qX_", ((long long)0x1234ABC));

	TEST("_-1234_", "_%jd_", ((intmax_t)-1234));
	TEST("_1234_", "_%jd_", ((intmax_t)1234));
	TEST("_-1234_", "_%ji_", ((intmax_t)-1234));
	TEST("_1234_", "_%ji_", ((intmax_t)1234));
	TEST("_01234_", "_%#jo_", ((intmax_t)01234));
	TEST("_1234_", "_%ju_", ((intmax_t)1234));
	TEST("_0x1234abc_", "_%#jx_", ((intmax_t)0x1234abc));
	TEST("_0X1234ABC_", "_%#jX_", ((intmax_t)0x1234ABC));

	TEST("_1234_", "_%td_", ((ptrdiff_t)1234));
	TEST("_-1234_", "_%td_", ((ptrdiff_t)-1234));
	TEST("_1234_", "_%ti_", ((ptrdiff_t)1234));
	TEST("_-1234_", "_%ti_", ((ptrdiff_t)-1234));

	TEST("_-1234_", "_%zd_", ((ssize_t)-1234));
	TEST("_1234_", "_%zd_", ((ssize_t)1234));
	TEST("_-1234_", "_%zi_", ((ssize_t)-1234));
	TEST("_1234_", "_%zi_", ((ssize_t)1234));
	TEST("_01234_", "_%#zo_", ((ssize_t)01234));
	TEST("_1234_", "_%zu_", ((ssize_t)1234));
	TEST("_0x1234abc_", "_%#zx_", ((ssize_t)0x1234abc));
	TEST("_0X1234ABC_", "_%#zX_", ((ssize_t)0x1234ABC));
#undef BUFLEN
}
TEST_END

int
main(void)
{

	return (test(
	    test_pow2_ceil_u64,
	    test_pow2_ceil_u32,
	    test_pow2_ceil_zu,
	    test_malloc_strtoumax_no_endptr,
	    test_malloc_strtoumax,
	    test_malloc_snprintf_truncated,
	    test_malloc_snprintf));
}
