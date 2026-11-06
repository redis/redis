/*
 * Shared utility for checking if background_thread is enabled, which affects
 * the purging behavior and assumptions in some tests.
 */

static inline bool
is_background_thread_enabled(void) {
	bool enabled;
	size_t sz = sizeof(bool);
	int ret = mallctl("background_thread", (void *)&enabled, &sz, NULL,0);
	if (ret == ENOENT) {
		return false;
	}
	assert_d_eq(ret, 0, "Unexpected mallctl error");

	return enabled;
}
