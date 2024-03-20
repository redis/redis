/* SPDX-License-Identifier: MIT */

#ifndef LIBURING_ARCH_X86_SYSCALL_H
#define LIBURING_ARCH_X86_SYSCALL_H

#if defined(__x86_64__)
/**
 * Note for syscall registers usage (x86-64):
 *   - %rax is the syscall number.
 *   - %rax is also the return value.
 *   - %rdi is the 1st argument.
 *   - %rsi is the 2nd argument.
 *   - %rdx is the 3rd argument.
 *   - %r10 is the 4th argument (**yes it's %r10, not %rcx!**).
 *   - %r8  is the 5th argument.
 *   - %r9  is the 6th argument.
 *
 * `syscall` instruction will clobber %r11 and %rcx.
 *
 * After the syscall returns to userspace:
 *   - %r11 will contain %rflags.
 *   - %rcx will contain the return address.
 *
 * IOW, after the syscall returns to userspace:
 *   %r11 == %rflags and %rcx == %rip.
 */

#define __do_syscall0(NUM) ({			\
	intptr_t rax;				\
						\
	__asm__ volatile(			\
		"syscall"			\
		: "=a"(rax)	/* %rax */	\
		: "a"(NUM)	/* %rax */	\
		: "rcx", "r11", "memory"	\
	);					\
	rax;					\
})

#define __do_syscall1(NUM, ARG1) ({		\
	intptr_t rax;				\
						\
	__asm__ volatile(			\
		"syscall"			\
		: "=a"(rax)	/* %rax */	\
		: "a"((NUM)),	/* %rax */	\
		  "D"((ARG1))	/* %rdi */	\
		: "rcx", "r11", "memory"	\
	);					\
	rax;					\
})

#define __do_syscall2(NUM, ARG1, ARG2) ({	\
	intptr_t rax;				\
						\
	__asm__ volatile(			\
		"syscall"			\
		: "=a"(rax)	/* %rax */	\
		: "a"((NUM)),	/* %rax */	\
		  "D"((ARG1)),	/* %rdi */	\
		  "S"((ARG2))	/* %rsi */	\
		: "rcx", "r11", "memory"	\
	);					\
	rax;					\
})

#define __do_syscall3(NUM, ARG1, ARG2, ARG3) ({	\
	intptr_t rax;				\
						\
	__asm__ volatile(			\
		"syscall"			\
		: "=a"(rax)	/* %rax */	\
		: "a"((NUM)),	/* %rax */	\
		  "D"((ARG1)),	/* %rdi */	\
		  "S"((ARG2)),	/* %rsi */	\
		  "d"((ARG3))	/* %rdx */	\
		: "rcx", "r11", "memory"	\
	);					\
	rax;					\
})

#define __do_syscall4(NUM, ARG1, ARG2, ARG3, ARG4) ({			\
	intptr_t rax;							\
	register __typeof__(ARG4) __r10 __asm__("r10") = (ARG4);	\
									\
	__asm__ volatile(						\
		"syscall"						\
		: "=a"(rax)	/* %rax */				\
		: "a"((NUM)),	/* %rax */				\
		  "D"((ARG1)),	/* %rdi */				\
		  "S"((ARG2)),	/* %rsi */				\
		  "d"((ARG3)),	/* %rdx */				\
		  "r"(__r10)	/* %r10 */				\
		: "rcx", "r11", "memory"				\
	);								\
	rax;								\
})

#define __do_syscall5(NUM, ARG1, ARG2, ARG3, ARG4, ARG5) ({		\
	intptr_t rax;							\
	register __typeof__(ARG4) __r10 __asm__("r10") = (ARG4);	\
	register __typeof__(ARG5) __r8 __asm__("r8") = (ARG5);		\
									\
	__asm__ volatile(						\
		"syscall"						\
		: "=a"(rax)	/* %rax */				\
		: "a"((NUM)),	/* %rax */				\
		  "D"((ARG1)),	/* %rdi */				\
		  "S"((ARG2)),	/* %rsi */				\
		  "d"((ARG3)),	/* %rdx */				\
		  "r"(__r10),	/* %r10 */				\
		  "r"(__r8)	/* %r8 */				\
		: "rcx", "r11", "memory"				\
	);								\
	rax;								\
})

#define __do_syscall6(NUM, ARG1, ARG2, ARG3, ARG4, ARG5, ARG6) ({	\
	intptr_t rax;							\
	register __typeof__(ARG4) __r10 __asm__("r10") = (ARG4);	\
	register __typeof__(ARG5) __r8 __asm__("r8") = (ARG5);		\
	register __typeof__(ARG6) __r9 __asm__("r9") = (ARG6);		\
									\
	__asm__ volatile(						\
		"syscall"						\
		: "=a"(rax)	/* %rax */				\
		: "a"((NUM)),	/* %rax */				\
		  "D"((ARG1)),	/* %rdi */				\
		  "S"((ARG2)),	/* %rsi */				\
		  "d"((ARG3)),	/* %rdx */				\
		  "r"(__r10),	/* %r10 */				\
		  "r"(__r8),	/* %r8 */				\
		  "r"(__r9)	/* %r9 */				\
		: "rcx", "r11", "memory"				\
	);								\
	rax;								\
})

#include "../syscall-defs.h"

#else /* #if defined(__x86_64__) */

#ifdef CONFIG_NOLIBC
/**
 * Note for syscall registers usage (x86, 32-bit):
 *   - %eax is the syscall number.
 *   - %eax is also the return value.
 *   - %ebx is the 1st argument.
 *   - %ecx is the 2nd argument.
 *   - %edx is the 3rd argument.
 *   - %esi is the 4th argument.
 *   - %edi is the 5th argument.
 *   - %ebp is the 6th argument.
 */

#define __do_syscall0(NUM) ({			\
	intptr_t eax;				\
						\
	__asm__ volatile(			\
		"int	$0x80"			\
		: "=a"(eax)	/* %eax */	\
		: "a"(NUM)	/* %eax */	\
		: "memory"			\
	);					\
	eax;					\
})

#define __do_syscall1(NUM, ARG1) ({		\
	intptr_t eax;				\
						\
	__asm__ volatile(			\
		"int	$0x80"			\
		: "=a"(eax)	/* %eax */	\
		: "a"(NUM),	/* %eax */	\
		  "b"((ARG1))	/* %ebx */	\
		: "memory"			\
	);					\
	eax;					\
})

#define __do_syscall2(NUM, ARG1, ARG2) ({	\
	intptr_t eax;				\
						\
	__asm__ volatile(			\
		"int	$0x80"			\
		: "=a" (eax)	/* %eax */	\
		: "a"(NUM),	/* %eax */	\
		  "b"((ARG1)),	/* %ebx */	\
		  "c"((ARG2))	/* %ecx */	\
		: "memory"			\
	);					\
	eax;					\
})

#define __do_syscall3(NUM, ARG1, ARG2, ARG3) ({	\
	intptr_t eax;				\
						\
	__asm__ volatile(			\
		"int	$0x80"			\
		: "=a" (eax)	/* %eax */	\
		: "a"(NUM),	/* %eax */	\
		  "b"((ARG1)),	/* %ebx */	\
		  "c"((ARG2)),	/* %ecx */	\
		  "d"((ARG3))	/* %edx */	\
		: "memory"			\
	);					\
	eax;					\
})

#define __do_syscall4(NUM, ARG1, ARG2, ARG3, ARG4) ({	\
	intptr_t eax;					\
							\
	__asm__ volatile(				\
		"int	$0x80"				\
		: "=a" (eax)	/* %eax */		\
		: "a"(NUM),	/* %eax */		\
		  "b"((ARG1)),	/* %ebx */		\
		  "c"((ARG2)),	/* %ecx */		\
		  "d"((ARG3)),	/* %edx */		\
		  "S"((ARG4))	/* %esi */		\
		: "memory"				\
	);						\
	eax;						\
})

#define __do_syscall5(NUM, ARG1, ARG2, ARG3, ARG4, ARG5) ({	\
	intptr_t eax;						\
								\
	__asm__ volatile(					\
		"int	$0x80"					\
		: "=a" (eax)	/* %eax */			\
		: "a"(NUM),	/* %eax */			\
		  "b"((ARG1)),	/* %ebx */			\
		  "c"((ARG2)),	/* %ecx */			\
		  "d"((ARG3)),	/* %edx */			\
		  "S"((ARG4)),	/* %esi */			\
		  "D"((ARG5))	/* %edi */			\
		: "memory"					\
	);							\
	eax;							\
})


/*
 * On i386, the 6th argument of syscall goes in %ebp. However, both Clang
 * and GCC cannot use %ebp in the clobber list and in the "r" constraint
 * without using -fomit-frame-pointer. To make it always available for
 * any kind of compilation, the below workaround is implemented:
 *
 *  1) Push the 6-th argument.
 *  2) Push %ebp.
 *  3) Load the 6-th argument from 4(%esp) to %ebp.
 *  4) Do the syscall (int $0x80).
 *  5) Pop %ebp (restore the old value of %ebp).
 *  6) Add %esp by 4 (undo the stack pointer).
 *
 * WARNING:
 *   Don't use register variables for __do_syscall6(), there is a known
 *   GCC bug that results in an endless loop.
 *
 * BugLink: https://gcc.gnu.org/bugzilla/show_bug.cgi?id=105032
 *
 */
#define __do_syscall6(NUM, ARG1, ARG2, ARG3, ARG4, ARG5, ARG6) ({	\
	intptr_t eax  = (intptr_t)(NUM);				\
	intptr_t arg6 = (intptr_t)(ARG6); /* Always in memory */	\
	__asm__ volatile (						\
		"pushl	%[_arg6]\n\t"					\
		"pushl	%%ebp\n\t"					\
		"movl	4(%%esp),%%ebp\n\t"				\
		"int	$0x80\n\t"					\
		"popl	%%ebp\n\t"					\
		"addl	$4,%%esp"					\
		: "+a"(eax)		/* %eax */			\
		: "b"(ARG1),		/* %ebx */			\
		  "c"(ARG2),		/* %ecx */			\
		  "d"(ARG3),		/* %edx */			\
		  "S"(ARG4),		/* %esi */			\
		  "D"(ARG5),		/* %edi */			\
		  [_arg6]"m"(arg6)	/* memory */			\
		: "memory", "cc"					\
	);								\
	eax;								\
})

#include "../syscall-defs.h"

#else /* #ifdef CONFIG_NOLIBC */

#include "../generic/syscall.h"

#endif /* #ifdef CONFIG_NOLIBC */

#endif /* #if defined(__x86_64__) */

#endif /* #ifndef LIBURING_ARCH_X86_SYSCALL_H */
