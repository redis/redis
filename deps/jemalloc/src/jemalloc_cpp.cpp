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

JEMALLOC_NOINLINE
static void *
handleOOM(std::size_t size, bool nothrow) {
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
JEMALLOC_ALWAYS_INLINE
void *
newImpl(std::size_t size) noexcept(IsNoExcept) {
	void *ptr = je_malloc(size);
	if (likely(ptr != nullptr))
		return ptr;

	return handleOOM(size, IsNoExcept);
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

void
operator delete(void *ptr, std::size_t size) noexcept {
	if (unlikely(ptr == nullptr)) {
		return;
	}
	je_sdallocx(ptr, size, /*flags=*/0);
}

void operator delete[](void *ptr, std::size_t size) noexcept {
	if (unlikely(ptr == nullptr)) {
		return;
	}
	je_sdallocx(ptr, size, /*flags=*/0);
}

#endif  // __cpp_sized_deallocation
