/* SPDX-License-Identifier: MIT */
/*
 * Description: check version macros and runtime checks work
 *
 */
#include "liburing.h"
#include "helpers.h"

int main(int argc, char *argv[])
{
	if (!IO_URING_CHECK_VERSION(io_uring_major_version(), io_uring_minor_version()))
		return T_EXIT_FAIL;

	if (io_uring_major_version() != IO_URING_VERSION_MAJOR)
		return T_EXIT_FAIL;

	if (io_uring_minor_version() != IO_URING_VERSION_MINOR)
		return T_EXIT_FAIL;

#if !IO_URING_CHECK_VERSION(IO_URING_VERSION_MAJOR, IO_URING_VERSION_MINOR)
	return T_EXIT_FAIL;
#endif

	return T_EXIT_PASS;
}
