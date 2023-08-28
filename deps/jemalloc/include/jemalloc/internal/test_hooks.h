#ifndef JEMALLOC_INTERNAL_TEST_HOOKS_H
#define JEMALLOC_INTERNAL_TEST_HOOKS_H

extern JEMALLOC_EXPORT void (*test_hooks_arena_new_hook)();
extern JEMALLOC_EXPORT void (*test_hooks_libc_hook)();

#if defined(JEMALLOC_JET) || defined(JEMALLOC_UNIT_TEST)
#  define JEMALLOC_TEST_HOOK(fn, hook) ((void)(hook != NULL && (hook(), 0)), fn)

#  define open JEMALLOC_TEST_HOOK(open, test_hooks_libc_hook)
#  define read JEMALLOC_TEST_HOOK(read, test_hooks_libc_hook)
#  define write JEMALLOC_TEST_HOOK(write, test_hooks_libc_hook)
#  define readlink JEMALLOC_TEST_HOOK(readlink, test_hooks_libc_hook)
#  define close JEMALLOC_TEST_HOOK(close, test_hooks_libc_hook)
#  define creat JEMALLOC_TEST_HOOK(creat, test_hooks_libc_hook)
#  define secure_getenv JEMALLOC_TEST_HOOK(secure_getenv, test_hooks_libc_hook)
/* Note that this is undef'd and re-define'd in src/prof.c. */
#  define _Unwind_Backtrace JEMALLOC_TEST_HOOK(_Unwind_Backtrace, test_hooks_libc_hook)
#else
#  define JEMALLOC_TEST_HOOK(fn, hook) fn
#endif


#endif /* JEMALLOC_INTERNAL_TEST_HOOKS_H */
