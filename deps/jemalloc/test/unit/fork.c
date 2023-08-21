#include "test/jemalloc_test.h"

#ifndef _WIN32
#include <sys/wait.h>
#endif

#ifndef _WIN32
static void
wait_for_child_exit(int pid) {
	int status;
	while (true) {
		if (waitpid(pid, &status, 0) == -1) {
			test_fail("Unexpected waitpid() failure.");
		}
		if (WIFSIGNALED(status)) {
			test_fail("Unexpected child termination due to "
			    "signal %d", WTERMSIG(status));
			break;
		}
		if (WIFEXITED(status)) {
			if (WEXITSTATUS(status) != 0) {
				test_fail("Unexpected child exit value %d",
				    WEXITSTATUS(status));
			}
			break;
		}
	}
}
#endif

TEST_BEGIN(test_fork) {
#ifndef _WIN32
	void *p;
	pid_t pid;

	/* Set up a manually managed arena for test. */
	unsigned arena_ind;
	size_t sz = sizeof(unsigned);
	expect_d_eq(mallctl("arenas.create", (void *)&arena_ind, &sz, NULL, 0),
	    0, "Unexpected mallctl() failure");

	/* Migrate to the new arena. */
	unsigned old_arena_ind;
	sz = sizeof(old_arena_ind);
	expect_d_eq(mallctl("thread.arena", (void *)&old_arena_ind, &sz,
	    (void *)&arena_ind, sizeof(arena_ind)), 0,
	    "Unexpected mallctl() failure");

	p = malloc(1);
	expect_ptr_not_null(p, "Unexpected malloc() failure");

	pid = fork();

	free(p);

	p = malloc(64);
	expect_ptr_not_null(p, "Unexpected malloc() failure");
	free(p);

	if (pid == -1) {
		/* Error. */
		test_fail("Unexpected fork() failure");
	} else if (pid == 0) {
		/* Child. */
		_exit(0);
	} else {
		wait_for_child_exit(pid);
	}
#else
	test_skip("fork(2) is irrelevant to Windows");
#endif
}
TEST_END

#ifndef _WIN32
static void *
do_fork_thd(void *arg) {
	malloc(1);
	int pid = fork();
	if (pid == -1) {
		/* Error. */
		test_fail("Unexpected fork() failure");
	} else if (pid == 0) {
		/* Child. */
		char *args[] = {"true", NULL};
		execvp(args[0], args);
		test_fail("Exec failed");
	} else {
		/* Parent */
		wait_for_child_exit(pid);
	}
	return NULL;
}
#endif

#ifndef _WIN32
static void
do_test_fork_multithreaded() {
	thd_t child;
	thd_create(&child, do_fork_thd, NULL);
	do_fork_thd(NULL);
	thd_join(child, NULL);
}
#endif

TEST_BEGIN(test_fork_multithreaded) {
#ifndef _WIN32
	/*
	 * We've seen bugs involving hanging on arenas_lock (though the same
	 * class of bugs can happen on any mutex).  The bugs are intermittent
	 * though, so we want to run the test multiple times.  Since we hold the
	 * arenas lock only early in the process lifetime, we can't just run
	 * this test in a loop (since, after all the arenas are initialized, we
	 * won't acquire arenas_lock any further).  We therefore repeat the test
	 * with multiple processes.
	 */
	for (int i = 0; i < 100; i++) {
		int pid = fork();
		if (pid == -1) {
			/* Error. */
			test_fail("Unexpected fork() failure,");
		} else if (pid == 0) {
			/* Child. */
			do_test_fork_multithreaded();
			_exit(0);
		} else {
			wait_for_child_exit(pid);
		}
	}
#else
	test_skip("fork(2) is irrelevant to Windows");
#endif
}
TEST_END

int
main(void) {
	return test_no_reentrancy(
	    test_fork,
	    test_fork_multithreaded);
}
