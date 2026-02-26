#ifndef JEMALLOC_INTERNAL_PROF_HOOK_H
#define JEMALLOC_INTERNAL_PROF_HOOK_H

/*
 * The hooks types of which are declared in this file are experimental and
 * undocumented, thus the typedefs are located in an 'internal' header.
 */

/*
 * A hook to mock out backtrace functionality.  This can be handy, since it's
 * otherwise difficult to guarantee that two allocations are reported as coming
 * from the exact same stack trace in the presence of an optimizing compiler.
 */
typedef void (*prof_backtrace_hook_t)(void **, unsigned *, unsigned);

/*
 * A callback hook that notifies about recently dumped heap profile.
 */
typedef void (*prof_dump_hook_t)(const char *filename);

#endif /* JEMALLOC_INTERNAL_PROF_HOOK_H */
