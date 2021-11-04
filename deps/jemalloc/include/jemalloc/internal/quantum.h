#ifndef JEMALLOC_INTERNAL_QUANTUM_H
#define JEMALLOC_INTERNAL_QUANTUM_H

/*
 * Minimum allocation alignment is 2^LG_QUANTUM bytes (ignoring tiny size
 * classes).
 */
#ifndef LG_QUANTUM
#  if (defined(__i386__) || defined(_M_IX86))
#    define LG_QUANTUM		4
#  endif
#  ifdef __ia64__
#    define LG_QUANTUM		4
#  endif
#  ifdef __alpha__
#    define LG_QUANTUM		4
#  endif
#  if (defined(__sparc64__) || defined(__sparcv9) || defined(__sparc_v9__))
#    define LG_QUANTUM		4
#  endif
#  if (defined(__amd64__) || defined(__x86_64__) || defined(_M_X64))
#    define LG_QUANTUM		4
#  endif
#  ifdef __arm__
#    define LG_QUANTUM		3
#  endif
#  ifdef __aarch64__
#    define LG_QUANTUM		4
#  endif
#  ifdef __hppa__
#    define LG_QUANTUM		4
#  endif
#  ifdef __m68k__
#    define LG_QUANTUM		3
#  endif
#  ifdef __mips__
#    define LG_QUANTUM		3
#  endif
#  ifdef __nios2__
#    define LG_QUANTUM		3
#  endif
#  ifdef __or1k__
#    define LG_QUANTUM		3
#  endif
#  ifdef __powerpc__
#    define LG_QUANTUM		4
#  endif
#  if defined(__riscv) || defined(__riscv__)
#    define LG_QUANTUM		4
#  endif
#  ifdef __s390__
#    define LG_QUANTUM		4
#  endif
#  if (defined (__SH3E__) || defined(__SH4_SINGLE__) || defined(__SH4__) || \
	defined(__SH4_SINGLE_ONLY__))
#    define LG_QUANTUM		4
#  endif
#  ifdef __tile__
#    define LG_QUANTUM		4
#  endif
#  ifdef __le32__
#    define LG_QUANTUM		4
#  endif
#  ifndef LG_QUANTUM
#    error "Unknown minimum alignment for architecture; specify via "
	 "--with-lg-quantum"
#  endif
#endif

#define QUANTUM			((size_t)(1U << LG_QUANTUM))
#define QUANTUM_MASK		(QUANTUM - 1)

/* Return the smallest quantum multiple that is >= a. */
#define QUANTUM_CEILING(a)						\
	(((a) + QUANTUM_MASK) & ~QUANTUM_MASK)

#endif /* JEMALLOC_INTERNAL_QUANTUM_H */
