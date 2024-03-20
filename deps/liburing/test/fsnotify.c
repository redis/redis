/* SPDX-License-Identifier: MIT */
/*
 * Description: test fsnotify access off O_DIRECT read
 */

#include "helpers.h"

#ifdef CONFIG_HAVE_FANOTIFY
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/fanotify.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "liburing.h"

int main(int argc, char *argv[])
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct io_uring ring;
	int fan, ret, fd, err;
	char fname[64], *f;
	struct stat sb;
	void *buf;

	fan = fanotify_init(FAN_CLASS_NOTIF|FAN_CLASS_CONTENT, 0);
	if (fan < 0) {
		if (errno == ENOSYS)
			return T_EXIT_SKIP;
		if (geteuid())
			return T_EXIT_SKIP;
		perror("fanotify_init");
		return T_EXIT_FAIL;
	}

	err = T_EXIT_FAIL;
	if (argc > 1) {
		f = argv[1];
		fd = open(argv[1], O_RDONLY | O_DIRECT);
		if (fd < 0 && errno == EINVAL)
			return T_EXIT_SKIP;
	} else {
		sprintf(fname, ".fsnotify.%d", getpid());
		f = fname;
		t_create_file(fname, 8192);
		fd = open(fname, O_RDONLY | O_DIRECT);
		if (fd < 0 && errno == EINVAL) {
			unlink(fname);
			return T_EXIT_SKIP;
		}
	}
	if (fd < 0) {
		perror("open");
		goto out;
	}

	if (fstat(fd, &sb) < 0) {
		perror("fstat");
		goto out;
	}
	if ((sb.st_mode & S_IFMT) != S_IFREG) {
		err = T_EXIT_SKIP;
		close(fd);
		goto out;
	}

	ret = fanotify_mark(fan, FAN_MARK_ADD, FAN_ACCESS|FAN_MODIFY, fd, NULL);
	if (ret < 0) {
		perror("fanotify_mark");
		goto out;
	}

	if (fork()) {
		int wstat;

		io_uring_queue_init(1, &ring, 0);
		if (posix_memalign(&buf, 4096, 4096))
			goto out;
		sqe = io_uring_get_sqe(&ring);
		io_uring_prep_read(sqe, fd, buf, 4096, 0);
		io_uring_submit(&ring);
		ret = io_uring_wait_cqe(&ring, &cqe);
		if (ret) {
			fprintf(stderr, "wait_ret=%d\n", ret);
			goto out;
		}
		wait(&wstat);
		if (!WEXITSTATUS(wstat))
			err = T_EXIT_PASS;
	} else {
		struct fanotify_event_metadata m;
		int fret;

		fret = read(fan, &m, sizeof(m));
		if (fret < 0)
			perror("fanotify read");
		/* fail if mask isn't right or pid indicates non-task context */
		else if (!(m.mask & 1) || !m.pid)
			exit(1);
		exit(0);
	}

out:
	if (f == fname)
		unlink(fname);
	return err;
}

#else /* #ifdef CONFIG_HAVE_FANOTIFY */

int main(void)
{
	return T_EXIT_SKIP;
}
#endif /* #ifdef CONFIG_HAVE_FANOTIFY */
