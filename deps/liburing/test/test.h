/* SPDX-License-Identifier: MIT */
/*
 * Description: Test configs for tests.
 */
#ifndef LIBURING_TEST_H
#define LIBURING_TEST_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct io_uring_test_config {
	unsigned int flags;
	const char *description;
} io_uring_test_config;

__attribute__((__unused__))
static io_uring_test_config io_uring_test_configs[] = {
	{ 0, 						"default" },
	{ IORING_SETUP_SQE128, 				"large SQE"},
	{ IORING_SETUP_CQE32, 				"large CQE"},
	{ IORING_SETUP_SQE128 | IORING_SETUP_CQE32, 	"large SQE/CQE" },
};

#define FOR_ALL_TEST_CONFIGS							\
	for (int i = 0; i < sizeof(io_uring_test_configs) / sizeof(io_uring_test_configs[0]); i++)

#define IORING_GET_TEST_CONFIG_FLAGS() (io_uring_test_configs[i].flags)
#define IORING_GET_TEST_CONFIG_DESCRIPTION() (io_uring_test_configs[i].description)


#ifdef __cplusplus
}
#endif

#endif
