/* SPDX-License-Identifier: MIT */

#ifndef LIBURING_ARCH_SYSCALL_DEFS_H
#define LIBURING_ARCH_SYSCALL_DEFS_H

#include <fcntl.h>

static inline int __sys_open(const char *pathname, int flags, mode_t mode)
{
	/*
	 * Some architectures don't have __NR_open, but __NR_openat.
	 */
#ifdef __NR_open
	return (int) __do_syscall3(__NR_open, pathname, flags, mode);
#else
	return (int) __do_syscall4(__NR_openat, AT_FDCWD, pathname, flags, mode);
#endif
}

static inline ssize_t __sys_read(int fd, void *buffer, size_t size)
{
	return (ssize_t) __do_syscall3(__NR_read, fd, buffer, size);
}

static inline void *__sys_mmap(void *addr, size_t length, int prot, int flags,
			       int fd, off_t offset)
{
	int nr;

#if defined(__NR_mmap2)
	nr = __NR_mmap2;
	offset >>= 12;
#else
	nr = __NR_mmap;
#endif
	return (void *) __do_syscall6(nr, addr, length, prot, flags, fd, offset);
}

static inline int __sys_munmap(void *addr, size_t length)
{
	return (int) __do_syscall2(__NR_munmap, addr, length);
}

static inline int __sys_madvise(void *addr, size_t length, int advice)
{
	return (int) __do_syscall3(__NR_madvise, addr, length, advice);
}

static inline int __sys_getrlimit(int resource, struct rlimit *rlim)
{
	return (int) __do_syscall2(__NR_getrlimit, resource, rlim);
}

static inline int __sys_setrlimit(int resource, const struct rlimit *rlim)
{
	return (int) __do_syscall2(__NR_setrlimit, resource, rlim);
}

static inline int __sys_close(int fd)
{
	return (int) __do_syscall1(__NR_close, fd);
}

static inline int __sys_io_uring_register(unsigned int fd, unsigned int opcode,
					  const void *arg, unsigned int nr_args)
{
	return (int) __do_syscall4(__NR_io_uring_register, fd, opcode, arg,
				   nr_args);
}

static inline int __sys_io_uring_setup(unsigned int entries,
				       struct io_uring_params *p)
{
	return (int) __do_syscall2(__NR_io_uring_setup, entries, p);
}

static inline int __sys_io_uring_enter2(unsigned int fd, unsigned int to_submit,
					unsigned int min_complete,
					unsigned int flags, sigset_t *sig,
					size_t sz)
{
	return (int) __do_syscall6(__NR_io_uring_enter, fd, to_submit,
				   min_complete, flags, sig, sz);
}

static inline int __sys_io_uring_enter(unsigned int fd, unsigned int to_submit,
				       unsigned int min_complete,
				       unsigned int flags, sigset_t *sig)
{
	return __sys_io_uring_enter2(fd, to_submit, min_complete, flags, sig,
				     _NSIG / 8);
}

#endif
