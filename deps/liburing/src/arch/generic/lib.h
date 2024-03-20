/* SPDX-License-Identifier: MIT */

#ifndef LIBURING_ARCH_GENERIC_LIB_H
#define LIBURING_ARCH_GENERIC_LIB_H

static inline long get_page_size(void)
{
	long page_size;

	page_size = sysconf(_SC_PAGESIZE);
	if (page_size < 0)
		page_size = 4096;

	return page_size;
}

#endif /* #ifndef LIBURING_ARCH_GENERIC_LIB_H */
