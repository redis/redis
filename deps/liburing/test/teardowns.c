/* SPDX-License-Identifier: MIT */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>

#include "liburing.h"

static void loop(void)
{
	int i, ret = 0;

	for (i = 0; i < 100; i++) {
		struct io_uring ring;
		int fd;

		memset(&ring, 0, sizeof(ring));
		fd = io_uring_queue_init(0xa4, &ring, 0);
		if (fd >= 0) {
			close(fd);
			continue;
		}
		if (fd != -ENOMEM)
			ret++;
	}
	exit(ret);
}

int main(int argc, char *argv[])
{
	int i, ret, status;

	if (argc > 1)
		return 0;

	for (i = 0; i < 12; i++) {
		if (!fork()) {
			loop();
			break;
		}
	}

	ret = 0;
	for (i = 0; i < 12; i++) {
		if (waitpid(-1, &status, 0) < 0) {
			perror("waitpid");
			return 1;
		}
		if (WEXITSTATUS(status))
			ret++;
	}

	return ret;
}
