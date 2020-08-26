#include "test/jemalloc_test.h"

static bool hook_called = false;

static void
hook() {
	hook_called = true;
}

static int
func_to_hook(int arg1, int arg2) {
	return arg1 + arg2;
}

#define func_to_hook JEMALLOC_HOOK(func_to_hook, hooks_libc_hook)

TEST_BEGIN(unhooked_call) {
	hooks_libc_hook = NULL;
	hook_called = false;
	assert_d_eq(3, func_to_hook(1, 2), "Hooking changed return value.");
	assert_false(hook_called, "Nulling out hook didn't take.");
}
TEST_END

TEST_BEGIN(hooked_call) {
	hooks_libc_hook = &hook;
	hook_called = false;
	assert_d_eq(3, func_to_hook(1, 2), "Hooking changed return value.");
	assert_true(hook_called, "Hook should have executed.");
}
TEST_END

int
main(void) {
	return test(
	    unhooked_call,
	    hooked_call);
}
