/* SPDX-License-Identifier: MIT */
#ifndef LIBURING_LIB_H
#define LIBURING_LIB_H

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__x86_64__) || defined(__i386__)
#include "arch/x86/lib.h"
#elif defined(__aarch64__)
#include "arch/aarch64/lib.h"
#elif defined(__riscv) && __riscv_xlen == 64
#include "arch/riscv64/lib.h"
#else
/*
 * We don't have nolibc support for this arch. Must use libc!
 */
#ifdef CONFIG_NOLIBC
#error "This arch doesn't support building liburing without libc"
#endif
/* libc wrappers. */
#include "arch/generic/lib.h"
#endif


#ifndef offsetof
#define offsetof(TYPE, FIELD) ((size_t) &((TYPE *)0)->FIELD)
#endif

#ifndef container_of
#define container_of(PTR, TYPE, FIELD) ({			\
	__typeof__(((TYPE *)0)->FIELD) *__FIELD_PTR = (PTR);	\
	(TYPE *)((char *) __FIELD_PTR - offsetof(TYPE, FIELD));	\
})
#endif

#define __maybe_unused		__attribute__((__unused__))
#define __hot			__attribute__((__hot__))
#define __cold			__attribute__((__cold__))

#ifdef CONFIG_NOLIBC
void *__uring_memset(void *s, int c, size_t n);
void *__uring_malloc(size_t len);
void __uring_free(void *p);

#define malloc(LEN)		__uring_malloc(LEN)
#define free(PTR)		__uring_free(PTR)
#define memset(PTR, C, LEN)	__uring_memset(PTR, C, LEN)
#endif

#endif /* #ifndef LIBURING_LIB_H */
