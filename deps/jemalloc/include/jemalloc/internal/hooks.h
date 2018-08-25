#ifndef JEMALLOC_INTERNAL_HOOKS_H
#define JEMALLOC_INTERNAL_HOOKS_H

extern JEMALLOC_EXPORT void (*hooks_arena_new_hook)();
extern JEMALLOC_EXPORT void (*hooks_libc_hook)();

#define JEMALLOC_HOOK(fn, hook) ((void)(hook != NULL && (hook(), 0)), fn)

#define open JEMALLOC_HOOK(open, hooks_libc_hook)
#define read JEMALLOC_HOOK(read, hooks_libc_hook)
#define write JEMALLOC_HOOK(write, hooks_libc_hook)
#define readlink JEMALLOC_HOOK(readlink, hooks_libc_hook)
#define close JEMALLOC_HOOK(close, hooks_libc_hook)
#define creat JEMALLOC_HOOK(creat, hooks_libc_hook)
#define secure_getenv JEMALLOC_HOOK(secure_getenv, hooks_libc_hook)
/* Note that this is undef'd and re-define'd in src/prof.c. */
#define _Unwind_Backtrace JEMALLOC_HOOK(_Unwind_Backtrace, hooks_libc_hook)

#endif /* JEMALLOC_INTERNAL_HOOKS_H */
