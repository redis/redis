/* SPDX-License-Identifier: MIT */

#include "liburing.h"
#include "liburing/io_uring_version.h"

int io_uring_major_version(void)
{
	return IO_URING_VERSION_MAJOR;
}

int io_uring_minor_version(void)
{
	return IO_URING_VERSION_MINOR;
}

bool io_uring_check_version(int major, int minor)
{
	return major > io_uring_major_version() ||
		(major == io_uring_major_version() &&
		 minor >= io_uring_minor_version());
}
