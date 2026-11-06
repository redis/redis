#include <mutex>
#include <new>

#define JEMALLOC_CPP_CPP_
#ifdef __cplusplus
extern "C" {
#endif

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#ifdef __cplusplus
}
#endif

// All operators in this file are exported.

// Possibly alias hidden versions of malloc and sdallocx to avoid an extra plt
// thunk?
//
// extern __typeof (sdallocx) sdallocx_int
//  __attribute ((alias ("sdallocx"),
//		visibility ("hidden")));
//
// ... but it needs to work with jemalloc namespaces.

void	*operator new(std::size_t size);
void	*operator new[](std::size_t size);
void	*operator new(std::size_t size, const std::nothrow_t &) noexcept;
void	*operator new[](std::size_t size, const std::nothrow_t &) noexcept;
void	operator delete(void *ptr) noexcept;
void	operator delete[](void *ptr) noexcept;
void	operator delete(void *ptr, const std::nothrow_t &) noexcept;
void	operator delete[](void *ptr, const std::nothrow_t &) noexcept;

#if __cpp_sized_deallocation >= 201309
/* C++14's sized-delete operators. */
void	operator delete(void *ptr, std::size_t size) noexcept;
void	operator delete[](void *ptr, std::size_t size) noexcept;
#endif

#if __cpp_aligned_new >= 201606
/* C++17's over-aligned operators. */
void	*operator new(std::size_t size, std::align_val_t);
void	*operator new(std::size_t size, std::align_val_t, const std::nothrow_t &) noexcept;
void	*operator new[](std::size_t size, std::align_val_t);
void	*operator new[](std::size_t size, std::align_val_t, const std::nothrow_t &) noexcept;
void	operator delete(void* ptr, std::align_val_t) noexcept;
void	operator delete(void* ptr, std::align_val_t, const std::nothrow_t &) noexcept;
void	operator delete(void* ptr, std::size_t size, std::align_val_t al) noexcept;
void	operator delete[](void* ptr, std::align_val_t) noexcept;
void	operator delete[](void* ptr, std::align_val_t, const std::nothrow_t &) noexcept;
void	operator delete[](void* ptr, std::size_t size, std::align_val_t al) noexcept;
#endif

JEMALLOC_NOINLINE
static void *
handleOOM(std::size_t size, bool nothrow) {
	if (opt_experimental_infallible_new) {
		safety_check_fail("<jemalloc>: Allocation failed and "
		    "opt.experimental_infallible_new is true. Aborting.\n");
		return nullptr;
	}

	void *ptr = nullptr;

	while (ptr == nullptr) {
		std::new_handler handler;
		// GCC-4.8 and clang 4.0 do not have std::get_new_handler.
		{
			static std::mutex mtx;
			std::lock_guard<std::mutex> lock(mtx);

			handler = std::set_new_handler(nullptr);
			std::set_new_handler(handler);
		}
		if (handler == nullptr)
			break;

		try {
			handler();
		} catch (const std::bad_alloc &) {
			break;
		}

		ptr = je_malloc(size);
	}

	if (ptr == nullptr && !nothrow)
		std::__throw_bad_alloc();
	return ptr;
}

template <bool IsNoExcept>
JEMALLOC_NOINLINE
static void *
fallback_impl(std::size_t size, std::size_t *usize) noexcept(IsNoExcept) {
	void *ptr = malloc_default(size, NULL);
	if (likely(ptr != nullptr)) {
		return ptr;
	}
	return handleOOM(size, IsNoExcept);
}

template <bool IsNoExcept>
JEMALLOC_ALWAYS_INLINE
void *
newImpl(std::size_t size) noexcept(IsNoExcept) {
	return imalloc_fastpath(size, &fallback_impl<IsNoExcept>, NULL);
}

void *
operator new(std::size_t size) {
	return newImpl<false>(size);
}

void *
operator new[](std::size_t size) {
	return newImpl<false>(size);
}

void *
operator new(std::size_t size, const std::nothrow_t &) noexcept {
	return newImpl<true>(size);
}

void *
operator new[](std::size_t size, const std::nothrow_t &) noexcept {
	return newImpl<true>(size);
}

#if __cpp_aligned_new >= 201606

template <bool IsNoExcept>
JEMALLOC_ALWAYS_INLINE
void *
alignedNewImpl(std::size_t size, std::align_val_t alignment) noexcept(IsNoExcept) {
	void *ptr = je_aligned_alloc(static_cast<std::size_t>(alignment), size);
	if (likely(ptr != nullptr)) {
		return ptr;
	}

	return handleOOM(size, IsNoExcept);
}

void *
operator new(std::size_t size, std::align_val_t alignment) {
	return alignedNewImpl<false>(size, alignment);
}

void *
operator new[](std::size_t size, std::align_val_t alignment) {
	return alignedNewImpl<false>(size, alignment);
}

void *
operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t &) noexcept {
	return alignedNewImpl<true>(size, alignment);
}

void *
operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t &) noexcept {
	return alignedNewImpl<true>(size, alignment);
}

#endif  // __cpp_aligned_new

void
operator delete(void *ptr) noexcept {
	je_free(ptr);
}

void
operator delete[](void *ptr) noexcept {
	je_free(ptr);
}

void
operator delete(void *ptr, const std::nothrow_t &) noexcept {
	je_free(ptr);
}

void operator delete[](void *ptr, const std::nothrow_t &) noexcept {
	je_free(ptr);
}

#if __cpp_sized_deallocation >= 201309

JEMALLOC_ALWAYS_INLINE
void
sizedDeleteImpl(void* ptr, std::size_t size) noexcept {
	if (unlikely(ptr == nullptr)) {
		return;
	}
	je_sdallocx_noflags(ptr, size);
}

void
operator delete(void *ptr, std::size_t size) noexcept {
	sizedDeleteImpl(ptr, size);
}

void
operator delete[](void *ptr, std::size_t size) noexcept {
	sizedDeleteImpl(ptr, size);
}

#endif  // __cpp_sized_deallocation

#if __cpp_aligned_new >= 201606

JEMALLOC_ALWAYS_INLINE
void
alignedSizedDeleteImpl(void* ptr, std::size_t size, std::align_val_t alignment) noexcept {
	if (config_debug) {
		assert(((size_t)alignment & ((size_t)alignment - 1)) == 0);
	}
	if (unlikely(ptr == nullptr)) {
		return;
	}
	je_sdallocx(ptr, size, MALLOCX_ALIGN(alignment));
}

void
operator delete(void* ptr, std::align_val_t) noexcept {
	je_free(ptr);
}

void
operator delete[](void* ptr, std::align_val_t) noexcept {
	je_free(ptr);
}

void
operator delete(void* ptr, std::align_val_t, const std::nothrow_t&) noexcept {
	je_free(ptr);
}

void
operator delete[](void* ptr, std::align_val_t, const std::nothrow_t&) noexcept {
	je_free(ptr);
}

void
operator delete(void* ptr, std::size_t size, std::align_val_t alignment) noexcept {
	alignedSizedDeleteImpl(ptr, size, alignment);
}

void
operator delete[](void* ptr, std::size_t size, std::align_val_t alignment) noexcept {
	alignedSizedDeleteImpl(ptr, size, alignment);
}

#endif  // __cpp_aligned_new
