/* SPDX-License-Identifier: MIT */
/*
 * Test liburing nolibc functionality.
 *
 * Currently, supported architectures are:
 *   1) x86
 *   2) x86-64
 *   3) aarch64
 *   4) riscv64
 *
 */
#include "helpers.h"

#if !defined(__x86_64__) && !defined(__i386__) && !defined(__aarch64__) && (!defined(__riscv) && __riscv_xlen != 64)


/*
 * This arch doesn't support nolibc.
 */
int main(void)
{
	return T_EXIT_SKIP;
}

#else /* #if !defined(__x86_64__) && !defined(__i386__) && !defined(__aarch64__) && (!defined(__riscv) && __riscv_xlen != 64) */

#ifndef CONFIG_NOLIBC
#define CONFIG_NOLIBC
#endif

#include <stdio.h>
#include <unistd.h>
#include "../src/lib.h"

static int test_get_page_size(void)
{
	long a, b;

	a = sysconf(_SC_PAGESIZE);
	b = get_page_size();
	if (a != b) {
		fprintf(stderr, "get_page_size() fails, %ld != %ld", a, b);
		return -1;
	}
	return 0;
}

int main(int argc, char *argv[])
{
	int ret;

	if (argc > 1)
		return T_EXIT_SKIP;

	ret = test_get_page_size();
	if (ret)
		return T_EXIT_FAIL;

	return T_EXIT_PASS;
}

#endif /* #if !defined(__x86_64__) && !defined(__i386__) && !defined(__aarch64__) && (!defined(__riscv) && __riscv_xlen != 64) */
