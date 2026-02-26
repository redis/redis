#define ASSERT_BUFSIZE	256

#define verify_cmp(may_abort, t, a, b, cmp, neg_cmp, pri, ...) do {	\
	const t a_ = (a);						\
	const t b_ = (b);						\
	if (!(a_ cmp b_)) {						\
		char prefix[ASSERT_BUFSIZE];				\
		char message[ASSERT_BUFSIZE];				\
		malloc_snprintf(prefix, sizeof(prefix),			\
		    "%s:%s:%d: Failed assertion: "			\
		    "(%s) " #cmp " (%s) --> "				\
		    "%" pri " " #neg_cmp " %" pri ": ",			\
		    __func__, __FILE__, __LINE__,			\
		    #a, #b, a_, b_);					\
		malloc_snprintf(message, sizeof(message), __VA_ARGS__);	\
		if (may_abort) {					\
			abort();					\
		} else {						\
			p_test_fail(prefix, message);			\
		}							\
	}								\
} while (0)

#define expect_cmp(t, a, b, cmp, neg_cmp, pri, ...) verify_cmp(false,	\
    t, a, b, cmp, neg_cmp, pri, __VA_ARGS__)

#define expect_ptr_eq(a, b, ...)	expect_cmp(void *, a, b, ==,	\
    !=, "p", __VA_ARGS__)
#define expect_ptr_ne(a, b, ...)	expect_cmp(void *, a, b, !=,	\
    ==, "p", __VA_ARGS__)
#define expect_ptr_null(a, ...)		expect_cmp(void *, a, NULL, ==,	\
    !=, "p", __VA_ARGS__)
#define expect_ptr_not_null(a, ...)	expect_cmp(void *, a, NULL, !=,	\
    ==, "p", __VA_ARGS__)

#define expect_c_eq(a, b, ...)	expect_cmp(char, a, b, ==, !=, "c", __VA_ARGS__)
#define expect_c_ne(a, b, ...)	expect_cmp(char, a, b, !=, ==, "c", __VA_ARGS__)
#define expect_c_lt(a, b, ...)	expect_cmp(char, a, b, <, >=, "c", __VA_ARGS__)
#define expect_c_le(a, b, ...)	expect_cmp(char, a, b, <=, >, "c", __VA_ARGS__)
#define expect_c_ge(a, b, ...)	expect_cmp(char, a, b, >=, <, "c", __VA_ARGS__)
#define expect_c_gt(a, b, ...)	expect_cmp(char, a, b, >, <=, "c", __VA_ARGS__)

#define expect_x_eq(a, b, ...)	expect_cmp(int, a, b, ==, !=, "#x", __VA_ARGS__)
#define expect_x_ne(a, b, ...)	expect_cmp(int, a, b, !=, ==, "#x", __VA_ARGS__)
#define expect_x_lt(a, b, ...)	expect_cmp(int, a, b, <, >=, "#x", __VA_ARGS__)
#define expect_x_le(a, b, ...)	expect_cmp(int, a, b, <=, >, "#x", __VA_ARGS__)
#define expect_x_ge(a, b, ...)	expect_cmp(int, a, b, >=, <, "#x", __VA_ARGS__)
#define expect_x_gt(a, b, ...)	expect_cmp(int, a, b, >, <=, "#x", __VA_ARGS__)

#define expect_d_eq(a, b, ...)	expect_cmp(int, a, b, ==, !=, "d", __VA_ARGS__)
#define expect_d_ne(a, b, ...)	expect_cmp(int, a, b, !=, ==, "d", __VA_ARGS__)
#define expect_d_lt(a, b, ...)	expect_cmp(int, a, b, <, >=, "d", __VA_ARGS__)
#define expect_d_le(a, b, ...)	expect_cmp(int, a, b, <=, >, "d", __VA_ARGS__)
#define expect_d_ge(a, b, ...)	expect_cmp(int, a, b, >=, <, "d", __VA_ARGS__)
#define expect_d_gt(a, b, ...)	expect_cmp(int, a, b, >, <=, "d", __VA_ARGS__)

#define expect_u_eq(a, b, ...)	expect_cmp(int, a, b, ==, !=, "u", __VA_ARGS__)
#define expect_u_ne(a, b, ...)	expect_cmp(int, a, b, !=, ==, "u", __VA_ARGS__)
#define expect_u_lt(a, b, ...)	expect_cmp(int, a, b, <, >=, "u", __VA_ARGS__)
#define expect_u_le(a, b, ...)	expect_cmp(int, a, b, <=, >, "u", __VA_ARGS__)
#define expect_u_ge(a, b, ...)	expect_cmp(int, a, b, >=, <, "u", __VA_ARGS__)
#define expect_u_gt(a, b, ...)	expect_cmp(int, a, b, >, <=, "u", __VA_ARGS__)

#define expect_ld_eq(a, b, ...)	expect_cmp(long, a, b, ==,	\
    !=, "ld", __VA_ARGS__)
#define expect_ld_ne(a, b, ...)	expect_cmp(long, a, b, !=,	\
    ==, "ld", __VA_ARGS__)
#define expect_ld_lt(a, b, ...)	expect_cmp(long, a, b, <,	\
    >=, "ld", __VA_ARGS__)
#define expect_ld_le(a, b, ...)	expect_cmp(long, a, b, <=,	\
    >, "ld", __VA_ARGS__)
#define expect_ld_ge(a, b, ...)	expect_cmp(long, a, b, >=,	\
    <, "ld", __VA_ARGS__)
#define expect_ld_gt(a, b, ...)	expect_cmp(long, a, b, >,	\
    <=, "ld", __VA_ARGS__)

#define expect_lu_eq(a, b, ...)	expect_cmp(unsigned long,	\
    a, b, ==, !=, "lu", __VA_ARGS__)
#define expect_lu_ne(a, b, ...)	expect_cmp(unsigned long,	\
    a, b, !=, ==, "lu", __VA_ARGS__)
#define expect_lu_lt(a, b, ...)	expect_cmp(unsigned long,	\
    a, b, <, >=, "lu", __VA_ARGS__)
#define expect_lu_le(a, b, ...)	expect_cmp(unsigned long,	\
    a, b, <=, >, "lu", __VA_ARGS__)
#define expect_lu_ge(a, b, ...)	expect_cmp(unsigned long,	\
    a, b, >=, <, "lu", __VA_ARGS__)
#define expect_lu_gt(a, b, ...)	expect_cmp(unsigned long,	\
    a, b, >, <=, "lu", __VA_ARGS__)

#define expect_qd_eq(a, b, ...)	expect_cmp(long long, a, b, ==,	\
    !=, "qd", __VA_ARGS__)
#define expect_qd_ne(a, b, ...)	expect_cmp(long long, a, b, !=,	\
    ==, "qd", __VA_ARGS__)
#define expect_qd_lt(a, b, ...)	expect_cmp(long long, a, b, <,	\
    >=, "qd", __VA_ARGS__)
#define expect_qd_le(a, b, ...)	expect_cmp(long long, a, b, <=,	\
    >, "qd", __VA_ARGS__)
#define expect_qd_ge(a, b, ...)	expect_cmp(long long, a, b, >=,	\
    <, "qd", __VA_ARGS__)
#define expect_qd_gt(a, b, ...)	expect_cmp(long long, a, b, >,	\
    <=, "qd", __VA_ARGS__)

#define expect_qu_eq(a, b, ...)	expect_cmp(unsigned long long,	\
    a, b, ==, !=, "qu", __VA_ARGS__)
#define expect_qu_ne(a, b, ...)	expect_cmp(unsigned long long,	\
    a, b, !=, ==, "qu", __VA_ARGS__)
#define expect_qu_lt(a, b, ...)	expect_cmp(unsigned long long,	\
    a, b, <, >=, "qu", __VA_ARGS__)
#define expect_qu_le(a, b, ...)	expect_cmp(unsigned long long,	\
    a, b, <=, >, "qu", __VA_ARGS__)
#define expect_qu_ge(a, b, ...)	expect_cmp(unsigned long long,	\
    a, b, >=, <, "qu", __VA_ARGS__)
#define expect_qu_gt(a, b, ...)	expect_cmp(unsigned long long,	\
    a, b, >, <=, "qu", __VA_ARGS__)

#define expect_jd_eq(a, b, ...)	expect_cmp(intmax_t, a, b, ==,	\
    !=, "jd", __VA_ARGS__)
#define expect_jd_ne(a, b, ...)	expect_cmp(intmax_t, a, b, !=,	\
    ==, "jd", __VA_ARGS__)
#define expect_jd_lt(a, b, ...)	expect_cmp(intmax_t, a, b, <,	\
    >=, "jd", __VA_ARGS__)
#define expect_jd_le(a, b, ...)	expect_cmp(intmax_t, a, b, <=,	\
    >, "jd", __VA_ARGS__)
#define expect_jd_ge(a, b, ...)	expect_cmp(intmax_t, a, b, >=,	\
    <, "jd", __VA_ARGS__)
#define expect_jd_gt(a, b, ...)	expect_cmp(intmax_t, a, b, >,	\
    <=, "jd", __VA_ARGS__)

#define expect_ju_eq(a, b, ...)	expect_cmp(uintmax_t, a, b, ==,	\
    !=, "ju", __VA_ARGS__)
#define expect_ju_ne(a, b, ...)	expect_cmp(uintmax_t, a, b, !=,	\
    ==, "ju", __VA_ARGS__)
#define expect_ju_lt(a, b, ...)	expect_cmp(uintmax_t, a, b, <,	\
    >=, "ju", __VA_ARGS__)
#define expect_ju_le(a, b, ...)	expect_cmp(uintmax_t, a, b, <=,	\
    >, "ju", __VA_ARGS__)
#define expect_ju_ge(a, b, ...)	expect_cmp(uintmax_t, a, b, >=,	\
    <, "ju", __VA_ARGS__)
#define expect_ju_gt(a, b, ...)	expect_cmp(uintmax_t, a, b, >,	\
    <=, "ju", __VA_ARGS__)

#define expect_zd_eq(a, b, ...)	expect_cmp(ssize_t, a, b, ==,	\
    !=, "zd", __VA_ARGS__)
#define expect_zd_ne(a, b, ...)	expect_cmp(ssize_t, a, b, !=,	\
    ==, "zd", __VA_ARGS__)
#define expect_zd_lt(a, b, ...)	expect_cmp(ssize_t, a, b, <,	\
    >=, "zd", __VA_ARGS__)
#define expect_zd_le(a, b, ...)	expect_cmp(ssize_t, a, b, <=,	\
    >, "zd", __VA_ARGS__)
#define expect_zd_ge(a, b, ...)	expect_cmp(ssize_t, a, b, >=,	\
    <, "zd", __VA_ARGS__)
#define expect_zd_gt(a, b, ...)	expect_cmp(ssize_t, a, b, >,	\
    <=, "zd", __VA_ARGS__)

#define expect_zu_eq(a, b, ...)	expect_cmp(size_t, a, b, ==,	\
    !=, "zu", __VA_ARGS__)
#define expect_zu_ne(a, b, ...)	expect_cmp(size_t, a, b, !=,	\
    ==, "zu", __VA_ARGS__)
#define expect_zu_lt(a, b, ...)	expect_cmp(size_t, a, b, <,	\
    >=, "zu", __VA_ARGS__)
#define expect_zu_le(a, b, ...)	expect_cmp(size_t, a, b, <=,	\
    >, "zu", __VA_ARGS__)
#define expect_zu_ge(a, b, ...)	expect_cmp(size_t, a, b, >=,	\
    <, "zu", __VA_ARGS__)
#define expect_zu_gt(a, b, ...)	expect_cmp(size_t, a, b, >,	\
    <=, "zu", __VA_ARGS__)

#define expect_d32_eq(a, b, ...)	expect_cmp(int32_t, a, b, ==,	\
    !=, FMTd32, __VA_ARGS__)
#define expect_d32_ne(a, b, ...)	expect_cmp(int32_t, a, b, !=,	\
    ==, FMTd32, __VA_ARGS__)
#define expect_d32_lt(a, b, ...)	expect_cmp(int32_t, a, b, <,	\
    >=, FMTd32, __VA_ARGS__)
#define expect_d32_le(a, b, ...)	expect_cmp(int32_t, a, b, <=,	\
    >, FMTd32, __VA_ARGS__)
#define expect_d32_ge(a, b, ...)	expect_cmp(int32_t, a, b, >=,	\
    <, FMTd32, __VA_ARGS__)
#define expect_d32_gt(a, b, ...)	expect_cmp(int32_t, a, b, >,	\
    <=, FMTd32, __VA_ARGS__)

#define expect_u32_eq(a, b, ...)	expect_cmp(uint32_t, a, b, ==,	\
    !=, FMTu32, __VA_ARGS__)
#define expect_u32_ne(a, b, ...)	expect_cmp(uint32_t, a, b, !=,	\
    ==, FMTu32, __VA_ARGS__)
#define expect_u32_lt(a, b, ...)	expect_cmp(uint32_t, a, b, <,	\
    >=, FMTu32, __VA_ARGS__)
#define expect_u32_le(a, b, ...)	expect_cmp(uint32_t, a, b, <=,	\
    >, FMTu32, __VA_ARGS__)
#define expect_u32_ge(a, b, ...)	expect_cmp(uint32_t, a, b, >=,	\
    <, FMTu32, __VA_ARGS__)
#define expect_u32_gt(a, b, ...)	expect_cmp(uint32_t, a, b, >,	\
    <=, FMTu32, __VA_ARGS__)

#define expect_d64_eq(a, b, ...)	expect_cmp(int64_t, a, b, ==,	\
    !=, FMTd64, __VA_ARGS__)
#define expect_d64_ne(a, b, ...)	expect_cmp(int64_t, a, b, !=,	\
    ==, FMTd64, __VA_ARGS__)
#define expect_d64_lt(a, b, ...)	expect_cmp(int64_t, a, b, <,	\
    >=, FMTd64, __VA_ARGS__)
#define expect_d64_le(a, b, ...)	expect_cmp(int64_t, a, b, <=,	\
    >, FMTd64, __VA_ARGS__)
#define expect_d64_ge(a, b, ...)	expect_cmp(int64_t, a, b, >=,	\
    <, FMTd64, __VA_ARGS__)
#define expect_d64_gt(a, b, ...)	expect_cmp(int64_t, a, b, >,	\
    <=, FMTd64, __VA_ARGS__)

#define expect_u64_eq(a, b, ...)	expect_cmp(uint64_t, a, b, ==,	\
    !=, FMTu64, __VA_ARGS__)
#define expect_u64_ne(a, b, ...)	expect_cmp(uint64_t, a, b, !=,	\
    ==, FMTu64, __VA_ARGS__)
#define expect_u64_lt(a, b, ...)	expect_cmp(uint64_t, a, b, <,	\
    >=, FMTu64, __VA_ARGS__)
#define expect_u64_le(a, b, ...)	expect_cmp(uint64_t, a, b, <=,	\
    >, FMTu64, __VA_ARGS__)
#define expect_u64_ge(a, b, ...)	expect_cmp(uint64_t, a, b, >=,	\
    <, FMTu64, __VA_ARGS__)
#define expect_u64_gt(a, b, ...)	expect_cmp(uint64_t, a, b, >,	\
    <=, FMTu64, __VA_ARGS__)

#define verify_b_eq(may_abort, a, b, ...) do {				\
	bool a_ = (a);							\
	bool b_ = (b);							\
	if (!(a_ == b_)) {						\
		char prefix[ASSERT_BUFSIZE];				\
		char message[ASSERT_BUFSIZE];				\
		malloc_snprintf(prefix, sizeof(prefix),			\
		    "%s:%s:%d: Failed assertion: "			\
		    "(%s) == (%s) --> %s != %s: ",			\
		    __func__, __FILE__, __LINE__,			\
		    #a, #b, a_ ? "true" : "false",			\
		    b_ ? "true" : "false");				\
		malloc_snprintf(message, sizeof(message), __VA_ARGS__);	\
		if (may_abort) {					\
			abort();					\
		} else {						\
			p_test_fail(prefix, message);			\
		}							\
	}								\
} while (0)

#define verify_b_ne(may_abort, a, b, ...) do {				\
	bool a_ = (a);							\
	bool b_ = (b);							\
	if (!(a_ != b_)) {						\
		char prefix[ASSERT_BUFSIZE];				\
		char message[ASSERT_BUFSIZE];				\
		malloc_snprintf(prefix, sizeof(prefix),			\
		    "%s:%s:%d: Failed assertion: "			\
		    "(%s) != (%s) --> %s == %s: ",			\
		    __func__, __FILE__, __LINE__,			\
		    #a, #b, a_ ? "true" : "false",			\
		    b_ ? "true" : "false");				\
		malloc_snprintf(message, sizeof(message), __VA_ARGS__);	\
		if (may_abort) {					\
			abort();					\
		} else {						\
			p_test_fail(prefix, message);			\
		}							\
	}								\
} while (0)

#define expect_b_eq(a, b, ...)	verify_b_eq(false, a, b, __VA_ARGS__)
#define expect_b_ne(a, b, ...)	verify_b_ne(false, a, b, __VA_ARGS__)

#define expect_true(a, ...)	expect_b_eq(a, true, __VA_ARGS__)
#define expect_false(a, ...)	expect_b_eq(a, false, __VA_ARGS__)

#define verify_str_eq(may_abort, a, b, ...) do {			\
	if (strcmp((a), (b))) {						\
		char prefix[ASSERT_BUFSIZE];				\
		char message[ASSERT_BUFSIZE];				\
		malloc_snprintf(prefix, sizeof(prefix),			\
		    "%s:%s:%d: Failed assertion: "			\
		    "(%s) same as (%s) --> "				\
		    "\"%s\" differs from \"%s\": ",			\
		    __func__, __FILE__, __LINE__, #a, #b, a, b);	\
		malloc_snprintf(message, sizeof(message), __VA_ARGS__);	\
		if (may_abort) {					\
			abort();					\
		} else {						\
			p_test_fail(prefix, message);			\
		}							\
	}								\
} while (0)

#define verify_str_ne(may_abort, a, b, ...) do {			\
	if (!strcmp((a), (b))) {					\
		char prefix[ASSERT_BUFSIZE];				\
		char message[ASSERT_BUFSIZE];				\
		malloc_snprintf(prefix, sizeof(prefix),			\
		    "%s:%s:%d: Failed assertion: "			\
		    "(%s) differs from (%s) --> "			\
		    "\"%s\" same as \"%s\": ",				\
		    __func__, __FILE__, __LINE__, #a, #b, a, b);	\
		malloc_snprintf(message, sizeof(message), __VA_ARGS__);	\
		if (may_abort) {					\
			abort();					\
		} else {						\
			p_test_fail(prefix, message);			\
		}							\
	}								\
} while (0)

#define expect_str_eq(a, b, ...) verify_str_eq(false, a, b, __VA_ARGS__)
#define expect_str_ne(a, b, ...) verify_str_ne(false, a, b, __VA_ARGS__)

#define verify_not_reached(may_abort, ...) do {				\
	char prefix[ASSERT_BUFSIZE];					\
	char message[ASSERT_BUFSIZE];					\
	malloc_snprintf(prefix, sizeof(prefix),				\
	    "%s:%s:%d: Unreachable code reached: ",			\
	    __func__, __FILE__, __LINE__);				\
	malloc_snprintf(message, sizeof(message), __VA_ARGS__);		\
	if (may_abort) {						\
		abort();						\
	} else {							\
		p_test_fail(prefix, message);				\
	}								\
} while (0)

#define expect_not_reached(...) verify_not_reached(false, __VA_ARGS__)

#define assert_cmp(t, a, b, cmp, neg_cmp, pri, ...) verify_cmp(true,	\
    t, a, b, cmp, neg_cmp, pri, __VA_ARGS__)

#define assert_ptr_eq(a, b, ...)	assert_cmp(void *, a, b, ==,	\
    !=, "p", __VA_ARGS__)
#define assert_ptr_ne(a, b, ...)	assert_cmp(void *, a, b, !=,	\
    ==, "p", __VA_ARGS__)
#define assert_ptr_null(a, ...)		assert_cmp(void *, a, NULL, ==,	\
    !=, "p", __VA_ARGS__)
#define assert_ptr_not_null(a, ...)	assert_cmp(void *, a, NULL, !=,	\
    ==, "p", __VA_ARGS__)

#define assert_c_eq(a, b, ...)	assert_cmp(char, a, b, ==, !=, "c", __VA_ARGS__)
#define assert_c_ne(a, b, ...)	assert_cmp(char, a, b, !=, ==, "c", __VA_ARGS__)
#define assert_c_lt(a, b, ...)	assert_cmp(char, a, b, <, >=, "c", __VA_ARGS__)
#define assert_c_le(a, b, ...)	assert_cmp(char, a, b, <=, >, "c", __VA_ARGS__)
#define assert_c_ge(a, b, ...)	assert_cmp(char, a, b, >=, <, "c", __VA_ARGS__)
#define assert_c_gt(a, b, ...)	assert_cmp(char, a, b, >, <=, "c", __VA_ARGS__)

#define assert_x_eq(a, b, ...)	assert_cmp(int, a, b, ==, !=, "#x", __VA_ARGS__)
#define assert_x_ne(a, b, ...)	assert_cmp(int, a, b, !=, ==, "#x", __VA_ARGS__)
#define assert_x_lt(a, b, ...)	assert_cmp(int, a, b, <, >=, "#x", __VA_ARGS__)
#define assert_x_le(a, b, ...)	assert_cmp(int, a, b, <=, >, "#x", __VA_ARGS__)
#define assert_x_ge(a, b, ...)	assert_cmp(int, a, b, >=, <, "#x", __VA_ARGS__)
#define assert_x_gt(a, b, ...)	assert_cmp(int, a, b, >, <=, "#x", __VA_ARGS__)

#define assert_d_eq(a, b, ...)	assert_cmp(int, a, b, ==, !=, "d", __VA_ARGS__)
#define assert_d_ne(a, b, ...)	assert_cmp(int, a, b, !=, ==, "d", __VA_ARGS__)
#define assert_d_lt(a, b, ...)	assert_cmp(int, a, b, <, >=, "d", __VA_ARGS__)
#define assert_d_le(a, b, ...)	assert_cmp(int, a, b, <=, >, "d", __VA_ARGS__)
#define assert_d_ge(a, b, ...)	assert_cmp(int, a, b, >=, <, "d", __VA_ARGS__)
#define assert_d_gt(a, b, ...)	assert_cmp(int, a, b, >, <=, "d", __VA_ARGS__)

#define assert_u_eq(a, b, ...)	assert_cmp(int, a, b, ==, !=, "u", __VA_ARGS__)
#define assert_u_ne(a, b, ...)	assert_cmp(int, a, b, !=, ==, "u", __VA_ARGS__)
#define assert_u_lt(a, b, ...)	assert_cmp(int, a, b, <, >=, "u", __VA_ARGS__)
#define assert_u_le(a, b, ...)	assert_cmp(int, a, b, <=, >, "u", __VA_ARGS__)
#define assert_u_ge(a, b, ...)	assert_cmp(int, a, b, >=, <, "u", __VA_ARGS__)
#define assert_u_gt(a, b, ...)	assert_cmp(int, a, b, >, <=, "u", __VA_ARGS__)

#define assert_ld_eq(a, b, ...)	assert_cmp(long, a, b, ==,	\
    !=, "ld", __VA_ARGS__)
#define assert_ld_ne(a, b, ...)	assert_cmp(long, a, b, !=,	\
    ==, "ld", __VA_ARGS__)
#define assert_ld_lt(a, b, ...)	assert_cmp(long, a, b, <,	\
    >=, "ld", __VA_ARGS__)
#define assert_ld_le(a, b, ...)	assert_cmp(long, a, b, <=,	\
    >, "ld", __VA_ARGS__)
#define assert_ld_ge(a, b, ...)	assert_cmp(long, a, b, >=,	\
    <, "ld", __VA_ARGS__)
#define assert_ld_gt(a, b, ...)	assert_cmp(long, a, b, >,	\
    <=, "ld", __VA_ARGS__)

#define assert_lu_eq(a, b, ...)	assert_cmp(unsigned long,	\
    a, b, ==, !=, "lu", __VA_ARGS__)
#define assert_lu_ne(a, b, ...)	assert_cmp(unsigned long,	\
    a, b, !=, ==, "lu", __VA_ARGS__)
#define assert_lu_lt(a, b, ...)	assert_cmp(unsigned long,	\
    a, b, <, >=, "lu", __VA_ARGS__)
#define assert_lu_le(a, b, ...)	assert_cmp(unsigned long,	\
    a, b, <=, >, "lu", __VA_ARGS__)
#define assert_lu_ge(a, b, ...)	assert_cmp(unsigned long,	\
    a, b, >=, <, "lu", __VA_ARGS__)
#define assert_lu_gt(a, b, ...)	assert_cmp(unsigned long,	\
    a, b, >, <=, "lu", __VA_ARGS__)

#define assert_qd_eq(a, b, ...)	assert_cmp(long long, a, b, ==,	\
    !=, "qd", __VA_ARGS__)
#define assert_qd_ne(a, b, ...)	assert_cmp(long long, a, b, !=,	\
    ==, "qd", __VA_ARGS__)
#define assert_qd_lt(a, b, ...)	assert_cmp(long long, a, b, <,	\
    >=, "qd", __VA_ARGS__)
#define assert_qd_le(a, b, ...)	assert_cmp(long long, a, b, <=,	\
    >, "qd", __VA_ARGS__)
#define assert_qd_ge(a, b, ...)	assert_cmp(long long, a, b, >=,	\
    <, "qd", __VA_ARGS__)
#define assert_qd_gt(a, b, ...)	assert_cmp(long long, a, b, >,	\
    <=, "qd", __VA_ARGS__)

#define assert_qu_eq(a, b, ...)	assert_cmp(unsigned long long,	\
    a, b, ==, !=, "qu", __VA_ARGS__)
#define assert_qu_ne(a, b, ...)	assert_cmp(unsigned long long,	\
    a, b, !=, ==, "qu", __VA_ARGS__)
#define assert_qu_lt(a, b, ...)	assert_cmp(unsigned long long,	\
    a, b, <, >=, "qu", __VA_ARGS__)
#define assert_qu_le(a, b, ...)	assert_cmp(unsigned long long,	\
    a, b, <=, >, "qu", __VA_ARGS__)
#define assert_qu_ge(a, b, ...)	assert_cmp(unsigned long long,	\
    a, b, >=, <, "qu", __VA_ARGS__)
#define assert_qu_gt(a, b, ...)	assert_cmp(unsigned long long,	\
    a, b, >, <=, "qu", __VA_ARGS__)

#define assert_jd_eq(a, b, ...)	assert_cmp(intmax_t, a, b, ==,	\
    !=, "jd", __VA_ARGS__)
#define assert_jd_ne(a, b, ...)	assert_cmp(intmax_t, a, b, !=,	\
    ==, "jd", __VA_ARGS__)
#define assert_jd_lt(a, b, ...)	assert_cmp(intmax_t, a, b, <,	\
    >=, "jd", __VA_ARGS__)
#define assert_jd_le(a, b, ...)	assert_cmp(intmax_t, a, b, <=,	\
    >, "jd", __VA_ARGS__)
#define assert_jd_ge(a, b, ...)	assert_cmp(intmax_t, a, b, >=,	\
    <, "jd", __VA_ARGS__)
#define assert_jd_gt(a, b, ...)	assert_cmp(intmax_t, a, b, >,	\
    <=, "jd", __VA_ARGS__)

#define assert_ju_eq(a, b, ...)	assert_cmp(uintmax_t, a, b, ==,	\
    !=, "ju", __VA_ARGS__)
#define assert_ju_ne(a, b, ...)	assert_cmp(uintmax_t, a, b, !=,	\
    ==, "ju", __VA_ARGS__)
#define assert_ju_lt(a, b, ...)	assert_cmp(uintmax_t, a, b, <,	\
    >=, "ju", __VA_ARGS__)
#define assert_ju_le(a, b, ...)	assert_cmp(uintmax_t, a, b, <=,	\
    >, "ju", __VA_ARGS__)
#define assert_ju_ge(a, b, ...)	assert_cmp(uintmax_t, a, b, >=,	\
    <, "ju", __VA_ARGS__)
#define assert_ju_gt(a, b, ...)	assert_cmp(uintmax_t, a, b, >,	\
    <=, "ju", __VA_ARGS__)

#define assert_zd_eq(a, b, ...)	assert_cmp(ssize_t, a, b, ==,	\
    !=, "zd", __VA_ARGS__)
#define assert_zd_ne(a, b, ...)	assert_cmp(ssize_t, a, b, !=,	\
    ==, "zd", __VA_ARGS__)
#define assert_zd_lt(a, b, ...)	assert_cmp(ssize_t, a, b, <,	\
    >=, "zd", __VA_ARGS__)
#define assert_zd_le(a, b, ...)	assert_cmp(ssize_t, a, b, <=,	\
    >, "zd", __VA_ARGS__)
#define assert_zd_ge(a, b, ...)	assert_cmp(ssize_t, a, b, >=,	\
    <, "zd", __VA_ARGS__)
#define assert_zd_gt(a, b, ...)	assert_cmp(ssize_t, a, b, >,	\
    <=, "zd", __VA_ARGS__)

#define assert_zu_eq(a, b, ...)	assert_cmp(size_t, a, b, ==,	\
    !=, "zu", __VA_ARGS__)
#define assert_zu_ne(a, b, ...)	assert_cmp(size_t, a, b, !=,	\
    ==, "zu", __VA_ARGS__)
#define assert_zu_lt(a, b, ...)	assert_cmp(size_t, a, b, <,	\
    >=, "zu", __VA_ARGS__)
#define assert_zu_le(a, b, ...)	assert_cmp(size_t, a, b, <=,	\
    >, "zu", __VA_ARGS__)
#define assert_zu_ge(a, b, ...)	assert_cmp(size_t, a, b, >=,	\
    <, "zu", __VA_ARGS__)
#define assert_zu_gt(a, b, ...)	assert_cmp(size_t, a, b, >,	\
    <=, "zu", __VA_ARGS__)

#define assert_d32_eq(a, b, ...)	assert_cmp(int32_t, a, b, ==,	\
    !=, FMTd32, __VA_ARGS__)
#define assert_d32_ne(a, b, ...)	assert_cmp(int32_t, a, b, !=,	\
    ==, FMTd32, __VA_ARGS__)
#define assert_d32_lt(a, b, ...)	assert_cmp(int32_t, a, b, <,	\
    >=, FMTd32, __VA_ARGS__)
#define assert_d32_le(a, b, ...)	assert_cmp(int32_t, a, b, <=,	\
    >, FMTd32, __VA_ARGS__)
#define assert_d32_ge(a, b, ...)	assert_cmp(int32_t, a, b, >=,	\
    <, FMTd32, __VA_ARGS__)
#define assert_d32_gt(a, b, ...)	assert_cmp(int32_t, a, b, >,	\
    <=, FMTd32, __VA_ARGS__)

#define assert_u32_eq(a, b, ...)	assert_cmp(uint32_t, a, b, ==,	\
    !=, FMTu32, __VA_ARGS__)
#define assert_u32_ne(a, b, ...)	assert_cmp(uint32_t, a, b, !=,	\
    ==, FMTu32, __VA_ARGS__)
#define assert_u32_lt(a, b, ...)	assert_cmp(uint32_t, a, b, <,	\
    >=, FMTu32, __VA_ARGS__)
#define assert_u32_le(a, b, ...)	assert_cmp(uint32_t, a, b, <=,	\
    >, FMTu32, __VA_ARGS__)
#define assert_u32_ge(a, b, ...)	assert_cmp(uint32_t, a, b, >=,	\
    <, FMTu32, __VA_ARGS__)
#define assert_u32_gt(a, b, ...)	assert_cmp(uint32_t, a, b, >,	\
    <=, FMTu32, __VA_ARGS__)

#define assert_d64_eq(a, b, ...)	assert_cmp(int64_t, a, b, ==,	\
    !=, FMTd64, __VA_ARGS__)
#define assert_d64_ne(a, b, ...)	assert_cmp(int64_t, a, b, !=,	\
    ==, FMTd64, __VA_ARGS__)
#define assert_d64_lt(a, b, ...)	assert_cmp(int64_t, a, b, <,	\
    >=, FMTd64, __VA_ARGS__)
#define assert_d64_le(a, b, ...)	assert_cmp(int64_t, a, b, <=,	\
    >, FMTd64, __VA_ARGS__)
#define assert_d64_ge(a, b, ...)	assert_cmp(int64_t, a, b, >=,	\
    <, FMTd64, __VA_ARGS__)
#define assert_d64_gt(a, b, ...)	assert_cmp(int64_t, a, b, >,	\
    <=, FMTd64, __VA_ARGS__)

#define assert_u64_eq(a, b, ...)	assert_cmp(uint64_t, a, b, ==,	\
    !=, FMTu64, __VA_ARGS__)
#define assert_u64_ne(a, b, ...)	assert_cmp(uint64_t, a, b, !=,	\
    ==, FMTu64, __VA_ARGS__)
#define assert_u64_lt(a, b, ...)	assert_cmp(uint64_t, a, b, <,	\
    >=, FMTu64, __VA_ARGS__)
#define assert_u64_le(a, b, ...)	assert_cmp(uint64_t, a, b, <=,	\
    >, FMTu64, __VA_ARGS__)
#define assert_u64_ge(a, b, ...)	assert_cmp(uint64_t, a, b, >=,	\
    <, FMTu64, __VA_ARGS__)
#define assert_u64_gt(a, b, ...)	assert_cmp(uint64_t, a, b, >,	\
    <=, FMTu64, __VA_ARGS__)

#define assert_b_eq(a, b, ...)	verify_b_eq(true, a, b, __VA_ARGS__)
#define assert_b_ne(a, b, ...)	verify_b_ne(true, a, b, __VA_ARGS__)

#define assert_true(a, ...)	assert_b_eq(a, true, __VA_ARGS__)
#define assert_false(a, ...)	assert_b_eq(a, false, __VA_ARGS__)

#define assert_str_eq(a, b, ...) verify_str_eq(true, a, b, __VA_ARGS__)
#define assert_str_ne(a, b, ...) verify_str_ne(true, a, b, __VA_ARGS__)

#define assert_not_reached(...) verify_not_reached(true, __VA_ARGS__)

/*
 * If this enum changes, corresponding changes in test/test.sh.in are also
 * necessary.
 */
typedef enum {
	test_status_pass = 0,
	test_status_skip = 1,
	test_status_fail = 2,

	test_status_count = 3
} test_status_t;

typedef void (test_t)(void);

#define TEST_BEGIN(f)							\
static void								\
f(void) {								\
	p_test_init(#f);

#define TEST_END							\
	goto label_test_end;						\
label_test_end:								\
	p_test_fini();							\
}

#define test(...)							\
	p_test(__VA_ARGS__, NULL)

#define test_no_reentrancy(...)							\
	p_test_no_reentrancy(__VA_ARGS__, NULL)

#define test_no_malloc_init(...)					\
	p_test_no_malloc_init(__VA_ARGS__, NULL)

#define test_skip_if(e) do {						\
	if (e) {							\
		test_skip("%s:%s:%d: Test skipped: (%s)",		\
		    __func__, __FILE__, __LINE__, #e);			\
		goto label_test_end;					\
	}								\
} while (0)

bool test_is_reentrant();

void	test_skip(const char *format, ...) JEMALLOC_FORMAT_PRINTF(1, 2);
void	test_fail(const char *format, ...) JEMALLOC_FORMAT_PRINTF(1, 2);

/* For private use by macros. */
test_status_t	p_test(test_t *t, ...);
test_status_t	p_test_no_reentrancy(test_t *t, ...);
test_status_t	p_test_no_malloc_init(test_t *t, ...);
void	p_test_init(const char *name);
void	p_test_fini(void);
void	p_test_fail(const char *prefix, const char *message);
