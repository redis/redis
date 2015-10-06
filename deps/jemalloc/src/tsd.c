#define	JEMALLOC_TSD_C_
#include "jemalloc/internal/jemalloc_internal.h"

/******************************************************************************/
/* Data. */

static unsigned ncleanups;
static malloc_tsd_cleanup_t cleanups[MALLOC_TSD_CLEANUPS_MAX];

malloc_tsd_data(, , tsd_t, TSD_INITIALIZER)

/******************************************************************************/

void *
malloc_tsd_malloc(size_t size)
{

	return (a0malloc(CACHELINE_CEILING(size)));
}

void
malloc_tsd_dalloc(void *wrapper)
{

	a0dalloc(wrapper);
}

void
malloc_tsd_no_cleanup(void *arg)
{

	not_reached();
}

#if defined(JEMALLOC_MALLOC_THREAD_CLEANUP) || defined(_WIN32)
#ifndef _WIN32
JEMALLOC_EXPORT
#endif
void
_malloc_thread_cleanup(void)
{
	bool pending[MALLOC_TSD_CLEANUPS_MAX], again;
	unsigned i;

	for (i = 0; i < ncleanups; i++)
		pending[i] = true;

	do {
		again = false;
		for (i = 0; i < ncleanups; i++) {
			if (pending[i]) {
				pending[i] = cleanups[i]();
				if (pending[i])
					again = true;
			}
		}
	} while (again);
}
#endif

void
malloc_tsd_cleanup_register(bool (*f)(void))
{

	assert(ncleanups < MALLOC_TSD_CLEANUPS_MAX);
	cleanups[ncleanups] = f;
	ncleanups++;
}

void
tsd_cleanup(void *arg)
{
	tsd_t *tsd = (tsd_t *)arg;

	switch (tsd->state) {
	case tsd_state_nominal:
#define O(n, t)								\
		n##_cleanup(tsd);
MALLOC_TSD
#undef O
		tsd->state = tsd_state_purgatory;
		tsd_set(tsd);
		break;
	case tsd_state_purgatory:
		/*
		 * The previous time this destructor was called, we set the
		 * state to tsd_state_purgatory so that other destructors
		 * wouldn't cause re-creation of the tsd.  This time, do
		 * nothing, and do not request another callback.
		 */
		break;
	case tsd_state_reincarnated:
		/*
		 * Another destructor deallocated memory after this destructor
		 * was called.  Reset state to tsd_state_purgatory and request
		 * another callback.
		 */
		tsd->state = tsd_state_purgatory;
		tsd_set(tsd);
		break;
	default:
		not_reached();
	}
}

bool
malloc_tsd_boot0(void)
{

	ncleanups = 0;
	if (tsd_boot0())
		return (true);
	*tsd_arenas_cache_bypassp_get(tsd_fetch()) = true;
	return (false);
}

void
malloc_tsd_boot1(void)
{

	tsd_boot1();
	*tsd_arenas_cache_bypassp_get(tsd_fetch()) = false;
}

#ifdef _WIN32
static BOOL WINAPI
_tls_callback(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{

	switch (fdwReason) {
#ifdef JEMALLOC_LAZY_LOCK
	case DLL_THREAD_ATTACH:
		isthreaded = true;
		break;
#endif
	case DLL_THREAD_DETACH:
		_malloc_thread_cleanup();
		break;
	default:
		break;
	}
	return (true);
}

#ifdef _MSC_VER
#  ifdef _M_IX86
#    pragma comment(linker, "/INCLUDE:__tls_used")
#  else
#    pragma comment(linker, "/INCLUDE:_tls_used")
#  endif
#  pragma section(".CRT$XLY",long,read)
#endif
JEMALLOC_SECTION(".CRT$XLY") JEMALLOC_ATTR(used)
static BOOL	(WINAPI *const tls_callback)(HINSTANCE hinstDLL,
    DWORD fdwReason, LPVOID lpvReserved) = _tls_callback;
#endif

#if (!defined(JEMALLOC_MALLOC_THREAD_CLEANUP) && !defined(JEMALLOC_TLS) && \
    !defined(_WIN32))
void *
tsd_init_check_recursion(tsd_init_head_t *head, tsd_init_block_t *block)
{
	pthread_t self = pthread_self();
	tsd_init_block_t *iter;

	/* Check whether this thread has already inserted into the list. */
	malloc_mutex_lock(&head->lock);
	ql_foreach(iter, &head->blocks, link) {
		if (iter->thread == self) {
			malloc_mutex_unlock(&head->lock);
			return (iter->data);
		}
	}
	/* Insert block into list. */
	ql_elm_new(block, link);
	block->thread = self;
	ql_tail_insert(&head->blocks, block, link);
	malloc_mutex_unlock(&head->lock);
	return (NULL);
}

void
tsd_init_finish(tsd_init_head_t *head, tsd_init_block_t *block)
{

	malloc_mutex_lock(&head->lock);
	ql_remove(&head->blocks, block, link);
	malloc_mutex_unlock(&head->lock);
}
#endif
