#include "test/jemalloc_test.h"

#define	TEST_STRUCT(p, t)						\
struct p##_test_s {							\
	t	accum0;							\
	t	x;							\
	t	s;							\
};									\
typedef struct p##_test_s p##_test_t;

#define	TEST_BODY(p, t, tc, ta, FMT) do {				\
	const p##_test_t tests[] = {					\
		{(t)-1, (t)-1, (t)-2},					\
		{(t)-1, (t) 0, (t)-2},					\
		{(t)-1, (t) 1, (t)-2},					\
									\
		{(t) 0, (t)-1, (t)-2},					\
		{(t) 0, (t) 0, (t)-2},					\
		{(t) 0, (t) 1, (t)-2},					\
									\
		{(t) 1, (t)-1, (t)-2},					\
		{(t) 1, (t) 0, (t)-2},					\
		{(t) 1, (t) 1, (t)-2},					\
									\
		{(t)0, (t)-(1 << 22), (t)-2},				\
		{(t)0, (t)(1 << 22), (t)-2},				\
		{(t)(1 << 22), (t)-(1 << 22), (t)-2},			\
		{(t)(1 << 22), (t)(1 << 22), (t)-2}			\
	};								\
	unsigned i;							\
									\
	for (i = 0; i < sizeof(tests)/sizeof(p##_test_t); i++) {	\
		bool err;						\
		t accum = tests[i].accum0;				\
		assert_##ta##_eq(atomic_read_##p(&accum),		\
		    tests[i].accum0,					\
		    "Erroneous read, i=%u", i);				\
									\
		assert_##ta##_eq(atomic_add_##p(&accum, tests[i].x),	\
		    (t)((tc)tests[i].accum0 + (tc)tests[i].x),		\
		    "i=%u, accum=%"FMT", x=%"FMT,			\
		    i, tests[i].accum0, tests[i].x);			\
		assert_##ta##_eq(atomic_read_##p(&accum), accum,	\
		    "Erroneous add, i=%u", i);				\
									\
		accum = tests[i].accum0;				\
		assert_##ta##_eq(atomic_sub_##p(&accum, tests[i].x),	\
		    (t)((tc)tests[i].accum0 - (tc)tests[i].x),		\
		    "i=%u, accum=%"FMT", x=%"FMT,			\
		    i, tests[i].accum0, tests[i].x);			\
		assert_##ta##_eq(atomic_read_##p(&accum), accum,	\
		    "Erroneous sub, i=%u", i);				\
									\
		accum = tests[i].accum0;				\
		err = atomic_cas_##p(&accum, tests[i].x, tests[i].s);	\
		assert_b_eq(err, tests[i].accum0 != tests[i].x,		\
		    "Erroneous cas success/failure result");		\
		assert_##ta##_eq(accum, err ? tests[i].accum0 :		\
		    tests[i].s, "Erroneous cas effect, i=%u", i);	\
									\
		accum = tests[i].accum0;				\
		atomic_write_##p(&accum, tests[i].s);			\
		assert_##ta##_eq(accum, tests[i].s,			\
		    "Erroneous write, i=%u", i);			\
	}								\
} while (0)

TEST_STRUCT(uint64, uint64_t)
TEST_BEGIN(test_atomic_uint64)
{

#if !(LG_SIZEOF_PTR == 3 || LG_SIZEOF_INT == 3)
	test_skip("64-bit atomic operations not supported");
#else
	TEST_BODY(uint64, uint64_t, uint64_t, u64, FMTx64);
#endif
}
TEST_END

TEST_STRUCT(uint32, uint32_t)
TEST_BEGIN(test_atomic_uint32)
{

	TEST_BODY(uint32, uint32_t, uint32_t, u32, "#"FMTx32);
}
TEST_END

TEST_STRUCT(p, void *)
TEST_BEGIN(test_atomic_p)
{

	TEST_BODY(p, void *, uintptr_t, ptr, "p");
}
TEST_END

TEST_STRUCT(z, size_t)
TEST_BEGIN(test_atomic_z)
{

	TEST_BODY(z, size_t, size_t, zu, "#zx");
}
TEST_END

TEST_STRUCT(u, unsigned)
TEST_BEGIN(test_atomic_u)
{

	TEST_BODY(u, unsigned, unsigned, u, "#x");
}
TEST_END

int
main(void)
{

	return (test(
	    test_atomic_uint64,
	    test_atomic_uint32,
	    test_atomic_p,
	    test_atomic_z,
	    test_atomic_u));
}
