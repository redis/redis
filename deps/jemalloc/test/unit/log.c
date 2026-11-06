#include "test/jemalloc_test.h"

#include "jemalloc/internal/log.h"

static void
update_log_var_names(const char *names) {
	strncpy(log_var_names, names, sizeof(log_var_names));
}

static void
expect_no_logging(const char *names) {
	log_var_t log_l1 = LOG_VAR_INIT("l1");
	log_var_t log_l2 = LOG_VAR_INIT("l2");
	log_var_t log_l2_a = LOG_VAR_INIT("l2.a");

	update_log_var_names(names);

	int count = 0;

	for (int i = 0; i < 10; i++) {
		log_do_begin(log_l1)
			count++;
		log_do_end(log_l1)

		log_do_begin(log_l2)
			count++;
		log_do_end(log_l2)

		log_do_begin(log_l2_a)
			count++;
		log_do_end(log_l2_a)
	}
	expect_d_eq(count, 0, "Disabled logging not ignored!");
}

TEST_BEGIN(test_log_disabled) {
	test_skip_if(!config_log);
	atomic_store_b(&log_init_done, true, ATOMIC_RELAXED);
	expect_no_logging("");
	expect_no_logging("abc");
	expect_no_logging("a.b.c");
	expect_no_logging("l12");
	expect_no_logging("l123|a456|b789");
	expect_no_logging("|||");
}
TEST_END

TEST_BEGIN(test_log_enabled_direct) {
	test_skip_if(!config_log);
	atomic_store_b(&log_init_done, true, ATOMIC_RELAXED);
	log_var_t log_l1 = LOG_VAR_INIT("l1");
	log_var_t log_l1_a = LOG_VAR_INIT("l1.a");
	log_var_t log_l2 = LOG_VAR_INIT("l2");

	int count;

	count = 0;
	update_log_var_names("l1");
	for (int i = 0; i < 10; i++) {
		log_do_begin(log_l1)
			count++;
		log_do_end(log_l1)
	}
	expect_d_eq(count, 10, "Mis-logged!");

	count = 0;
	update_log_var_names("l1.a");
	for (int i = 0; i < 10; i++) {
		log_do_begin(log_l1_a)
			count++;
		log_do_end(log_l1_a)
	}
	expect_d_eq(count, 10, "Mis-logged!");

	count = 0;
	update_log_var_names("l1.a|abc|l2|def");
	for (int i = 0; i < 10; i++) {
		log_do_begin(log_l1_a)
			count++;
		log_do_end(log_l1_a)

		log_do_begin(log_l2)
			count++;
		log_do_end(log_l2)
	}
	expect_d_eq(count, 20, "Mis-logged!");
}
TEST_END

TEST_BEGIN(test_log_enabled_indirect) {
	test_skip_if(!config_log);
	atomic_store_b(&log_init_done, true, ATOMIC_RELAXED);
	update_log_var_names("l0|l1|abc|l2.b|def");

	/* On. */
	log_var_t log_l1 = LOG_VAR_INIT("l1");
	/* Off. */
	log_var_t log_l1a = LOG_VAR_INIT("l1a");
	/* On. */
	log_var_t log_l1_a = LOG_VAR_INIT("l1.a");
	/* Off. */
	log_var_t log_l2_a = LOG_VAR_INIT("l2.a");
	/* On. */
	log_var_t log_l2_b_a = LOG_VAR_INIT("l2.b.a");
	/* On. */
	log_var_t log_l2_b_b = LOG_VAR_INIT("l2.b.b");

	/* 4 are on total, so should sum to 40. */
	int count = 0;
	for (int i = 0; i < 10; i++) {
		log_do_begin(log_l1)
			count++;
		log_do_end(log_l1)

		log_do_begin(log_l1a)
			count++;
		log_do_end(log_l1a)

		log_do_begin(log_l1_a)
			count++;
		log_do_end(log_l1_a)

		log_do_begin(log_l2_a)
			count++;
		log_do_end(log_l2_a)

		log_do_begin(log_l2_b_a)
			count++;
		log_do_end(log_l2_b_a)

		log_do_begin(log_l2_b_b)
			count++;
		log_do_end(log_l2_b_b)
	}

	expect_d_eq(count, 40, "Mis-logged!");
}
TEST_END

TEST_BEGIN(test_log_enabled_global) {
	test_skip_if(!config_log);
	atomic_store_b(&log_init_done, true, ATOMIC_RELAXED);
	update_log_var_names("abc|.|def");

	log_var_t log_l1 = LOG_VAR_INIT("l1");
	log_var_t log_l2_a_a = LOG_VAR_INIT("l2.a.a");

	int count = 0;
	for (int i = 0; i < 10; i++) {
		log_do_begin(log_l1)
		    count++;
		log_do_end(log_l1)

		log_do_begin(log_l2_a_a)
		    count++;
		log_do_end(log_l2_a_a)
	}
	expect_d_eq(count, 20, "Mis-logged!");
}
TEST_END

TEST_BEGIN(test_logs_if_no_init) {
	test_skip_if(!config_log);
	atomic_store_b(&log_init_done, false, ATOMIC_RELAXED);

	log_var_t l = LOG_VAR_INIT("definitely.not.enabled");

	int count = 0;
	for (int i = 0; i < 10; i++) {
		log_do_begin(l)
			count++;
		log_do_end(l)
	}
	expect_d_eq(count, 0, "Logging shouldn't happen if not initialized.");
}
TEST_END

/*
 * This really just checks to make sure that this usage compiles; we don't have
 * any test code to run.
 */
TEST_BEGIN(test_log_only_format_string) {
	if (false) {
		LOG("log_str", "No arguments follow this format string.");
	}
}
TEST_END

int
main(void) {
	return test(
	    test_log_disabled,
	    test_log_enabled_direct,
	    test_log_enabled_indirect,
	    test_log_enabled_global,
	    test_logs_if_no_init,
	    test_log_only_format_string);
}
