/* SPDX-License-Identifier: MIT */
#ifndef LIBURING_BARRIER_H
#define LIBURING_BARRIER_H

/*
From the kernel documentation file refcount-vs-atomic.rst:

A RELEASE memory ordering guarantees that all prior loads and
stores (all po-earlier instructions) on the same CPU are completed
before the operation. It also guarantees that all po-earlier
stores on the same CPU and all propagated stores from other CPUs
must propagate to all other CPUs before the release operation
(A-cumulative property). This is implemented using
:c:func:`smp_store_release`.

An ACQUIRE memory ordering guarantees that all post loads and
stores (all po-later instructions) on the same CPU are
completed after the acquire operation. It also guarantees that all
po-later stores on the same CPU must propagate to all other CPUs
after the acquire operation executes. This is implemented using
:c:func:`smp_acquire__after_ctrl_dep`.
*/

#ifdef __cplusplus
#include <atomic>

template <typename T>
static inline void IO_URING_WRITE_ONCE(T &var, T val)
{
	std::atomic_store_explicit(reinterpret_cast<std::atomic<T> *>(&var),
				   val, std::memory_order_relaxed);
}
template <typename T>
static inline T IO_URING_READ_ONCE(const T &var)
{
	return std::atomic_load_explicit(
		reinterpret_cast<const std::atomic<T> *>(&var),
		std::memory_order_relaxed);
}

template <typename T>
static inline void io_uring_smp_store_release(T *p, T v)
{
	std::atomic_store_explicit(reinterpret_cast<std::atomic<T> *>(p), v,
				   std::memory_order_release);
}

template <typename T>
static inline T io_uring_smp_load_acquire(const T *p)
{
	return std::atomic_load_explicit(
		reinterpret_cast<const std::atomic<T> *>(p),
		std::memory_order_acquire);
}

static inline void io_uring_smp_mb()
{
	std::atomic_thread_fence(std::memory_order_seq_cst);
}
#else
#include <stdatomic.h>

#define IO_URING_WRITE_ONCE(var, val)				\
	atomic_store_explicit((_Atomic __typeof__(var) *)&(var),	\
			      (val), memory_order_relaxed)
#define IO_URING_READ_ONCE(var)					\
	atomic_load_explicit((_Atomic __typeof__(var) *)&(var),	\
			     memory_order_relaxed)

#define io_uring_smp_store_release(p, v)			\
	atomic_store_explicit((_Atomic __typeof__(*(p)) *)(p), (v), \
			      memory_order_release)
#define io_uring_smp_load_acquire(p)				\
	atomic_load_explicit((_Atomic __typeof__(*(p)) *)(p),	\
			     memory_order_acquire)

#define io_uring_smp_mb()					\
	atomic_thread_fence(memory_order_seq_cst)
#endif

#endif /* defined(LIBURING_BARRIER_H) */
