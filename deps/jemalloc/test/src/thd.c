#include "test/jemalloc_test.h"

#ifdef _WIN32
void
thd_create(thd_t *thd, void *(*proc)(void *), void *arg) {
	LPTHREAD_START_ROUTINE routine = (LPTHREAD_START_ROUTINE)proc;
	*thd = CreateThread(NULL, 0, routine, arg, 0, NULL);
	if (*thd == NULL) {
		test_fail("Error in CreateThread()\n");
	}
}

void
thd_join(thd_t thd, void **ret) {
	if (WaitForSingleObject(thd, INFINITE) == WAIT_OBJECT_0 && ret) {
		DWORD exit_code;
		GetExitCodeThread(thd, (LPDWORD) &exit_code);
		*ret = (void *)(uintptr_t)exit_code;
	}
}

#else
void
thd_create(thd_t *thd, void *(*proc)(void *), void *arg) {
	if (pthread_create(thd, NULL, proc, arg) != 0) {
		test_fail("Error in pthread_create()\n");
	}
}

void
thd_join(thd_t thd, void **ret) {
	pthread_join(thd, ret);
}
#endif
