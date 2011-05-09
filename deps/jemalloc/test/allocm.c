#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define	JEMALLOC_MANGLE
#include "jemalloc_test.h"

#define CHUNK 0x400000
/* #define MAXALIGN ((size_t)0x80000000000LLU) */
#define MAXALIGN ((size_t)0x2000000LLU)
#define NITER 4

int
main(void)
{
	int r;
	void *p;
	size_t sz, alignment, total, tsz;
	unsigned i;
	void *ps[NITER];

	fprintf(stderr, "Test begin\n");

	sz = 0;
	r = JEMALLOC_P(allocm)(&p, &sz, 42, 0);
	if (r != ALLOCM_SUCCESS) {
		fprintf(stderr, "Unexpected allocm() error\n");
		abort();
	}
	if (sz < 42)
		fprintf(stderr, "Real size smaller than expected\n");
	if (JEMALLOC_P(dallocm)(p, 0) != ALLOCM_SUCCESS)
		fprintf(stderr, "Unexpected dallocm() error\n");

	r = JEMALLOC_P(allocm)(&p, NULL, 42, 0);
	if (r != ALLOCM_SUCCESS) {
		fprintf(stderr, "Unexpected allocm() error\n");
		abort();
	}
	if (JEMALLOC_P(dallocm)(p, 0) != ALLOCM_SUCCESS)
		fprintf(stderr, "Unexpected dallocm() error\n");

	r = JEMALLOC_P(allocm)(&p, NULL, 42, ALLOCM_ZERO);
	if (r != ALLOCM_SUCCESS) {
		fprintf(stderr, "Unexpected allocm() error\n");
		abort();
	}
	if (JEMALLOC_P(dallocm)(p, 0) != ALLOCM_SUCCESS)
		fprintf(stderr, "Unexpected dallocm() error\n");

#if LG_SIZEOF_PTR == 3
	alignment = 0x8000000000000000LLU;
	sz        = 0x8000000000000000LLU;
#else
	alignment = 0x80000000LU;
	sz        = 0x80000000LU;
#endif
	r = JEMALLOC_P(allocm)(&p, NULL, sz, ALLOCM_ALIGN(alignment));
	if (r == ALLOCM_SUCCESS) {
		fprintf(stderr,
		    "Expected error for allocm(&p, %zu, 0x%x)\n",
		    sz, ALLOCM_ALIGN(alignment));
	}

#if LG_SIZEOF_PTR == 3
	alignment = 0x4000000000000000LLU;
	sz        = 0x8400000000000001LLU;
#else
	alignment = 0x40000000LU;
	sz        = 0x84000001LU;
#endif
	r = JEMALLOC_P(allocm)(&p, NULL, sz, ALLOCM_ALIGN(alignment));
	if (r == ALLOCM_SUCCESS) {
		fprintf(stderr,
		    "Expected error for allocm(&p, %zu, 0x%x)\n",
		    sz, ALLOCM_ALIGN(alignment));
	}

	alignment = 0x10LLU;
#if LG_SIZEOF_PTR == 3
	sz   = 0xfffffffffffffff0LLU;
#else
	sz   = 0xfffffff0LU;
#endif
	r = JEMALLOC_P(allocm)(&p, NULL, sz, ALLOCM_ALIGN(alignment));
	if (r == ALLOCM_SUCCESS) {
		fprintf(stderr,
		    "Expected error for allocm(&p, %zu, 0x%x)\n",
		    sz, ALLOCM_ALIGN(alignment));
	}

	for (i = 0; i < NITER; i++)
		ps[i] = NULL;

	for (alignment = 8;
	    alignment <= MAXALIGN;
	    alignment <<= 1) {
		total = 0;
		fprintf(stderr, "Alignment: %zu\n", alignment);
		for (sz = 1;
		    sz < 3 * alignment && sz < (1U << 31);
		    sz += (alignment >> (LG_SIZEOF_PTR-1)) - 1) {
			for (i = 0; i < NITER; i++) {
				r = JEMALLOC_P(allocm)(&ps[i], NULL, sz,
				    ALLOCM_ALIGN(alignment) | ALLOCM_ZERO);
				if (r != ALLOCM_SUCCESS) {
					fprintf(stderr,
					    "Error for size %zu (0x%zx): %d\n",
					    sz, sz, r);
					exit(1);
				}
				if ((uintptr_t)p & (alignment-1)) {
					fprintf(stderr,
					    "%p inadequately aligned for"
					    " alignment: %zu\n", p, alignment);
				}
				JEMALLOC_P(sallocm)(ps[i], &tsz, 0);
				total += tsz;
				if (total >= (MAXALIGN << 1))
					break;
			}
			for (i = 0; i < NITER; i++) {
				if (ps[i] != NULL) {
					JEMALLOC_P(dallocm)(ps[i], 0);
					ps[i] = NULL;
				}
			}
		}
	}

	fprintf(stderr, "Test end\n");
	return (0);
}
