#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#define	JEMALLOC_MANGLE
#include "jemalloc_test.h"

#define CHUNK 0x400000
/* #define MAXALIGN ((size_t)0x80000000000LLU) */
#define MAXALIGN ((size_t)0x2000000LLU)
#define NITER 4

int
main(void)
{
	size_t alignment, size, total;
	unsigned i;
	int err;
	void *p, *ps[NITER];

	fprintf(stderr, "Test begin\n");

	/* Test error conditions. */
	for (alignment = 0; alignment < sizeof(void *); alignment++) {
		err = JEMALLOC_P(posix_memalign)(&p, alignment, 1);
		if (err != EINVAL) {
			fprintf(stderr,
			    "Expected error for invalid alignment %zu\n",
			    alignment);
		}
	}

	for (alignment = sizeof(size_t); alignment < MAXALIGN;
	    alignment <<= 1) {
		err = JEMALLOC_P(posix_memalign)(&p, alignment + 1, 1);
		if (err == 0) {
			fprintf(stderr,
			    "Expected error for invalid alignment %zu\n",
			    alignment + 1);
		}
	}

#if LG_SIZEOF_PTR == 3
	alignment = 0x8000000000000000LLU;
	size      = 0x8000000000000000LLU;
#else
	alignment = 0x80000000LU;
	size      = 0x80000000LU;
#endif
	err = JEMALLOC_P(posix_memalign)(&p, alignment, size);
	if (err == 0) {
		fprintf(stderr,
		    "Expected error for posix_memalign(&p, %zu, %zu)\n",
		    alignment, size);
	}

#if LG_SIZEOF_PTR == 3
	alignment = 0x4000000000000000LLU;
	size      = 0x8400000000000001LLU;
#else
	alignment = 0x40000000LU;
	size      = 0x84000001LU;
#endif
	err = JEMALLOC_P(posix_memalign)(&p, alignment, size);
	if (err == 0) {
		fprintf(stderr,
		    "Expected error for posix_memalign(&p, %zu, %zu)\n",
		    alignment, size);
	}

	alignment = 0x10LLU;
#if LG_SIZEOF_PTR == 3
	size = 0xfffffffffffffff0LLU;
#else
	size = 0xfffffff0LU;
#endif
	err = JEMALLOC_P(posix_memalign)(&p, alignment, size);
	if (err == 0) {
		fprintf(stderr,
		    "Expected error for posix_memalign(&p, %zu, %zu)\n",
		    alignment, size);
	}

	for (i = 0; i < NITER; i++)
		ps[i] = NULL;

	for (alignment = 8;
	    alignment <= MAXALIGN;
	    alignment <<= 1) {
		total = 0;
		fprintf(stderr, "Alignment: %zu\n", alignment);
		for (size = 1;
		    size < 3 * alignment && size < (1U << 31);
		    size += (alignment >> (LG_SIZEOF_PTR-1)) - 1) {
			for (i = 0; i < NITER; i++) {
				err = JEMALLOC_P(posix_memalign)(&ps[i],
				    alignment, size);
				if (err) {
					fprintf(stderr,
					    "Error for size %zu (0x%zx): %s\n",
					    size, size, strerror(err));
					exit(1);
				}
				total += JEMALLOC_P(malloc_usable_size)(ps[i]);
				if (total >= (MAXALIGN << 1))
					break;
			}
			for (i = 0; i < NITER; i++) {
				if (ps[i] != NULL) {
					JEMALLOC_P(free)(ps[i]);
					ps[i] = NULL;
				}
			}
		}
	}

	fprintf(stderr, "Test end\n");
	return (0);
}
