/******************************************************************************/
#ifdef JEMALLOC_H_TYPES

#endif /* JEMALLOC_H_TYPES */
/******************************************************************************/
#ifdef JEMALLOC_H_STRUCTS

#endif /* JEMALLOC_H_STRUCTS */
/******************************************************************************/
#ifdef JEMALLOC_H_EXTERNS

#endif /* JEMALLOC_H_EXTERNS */
/******************************************************************************/
#ifdef JEMALLOC_H_INLINES

#ifndef JEMALLOC_ENABLE_INLINE
uint64_t	hash(const void *key, size_t len, uint64_t seed);
#endif

#if (defined(JEMALLOC_ENABLE_INLINE) || defined(JEMALLOC_HASH_C_))
/*
 * The following hash function is based on MurmurHash64A(), placed into the
 * public domain by Austin Appleby.  See http://murmurhash.googlepages.com/ for
 * details.
 */
JEMALLOC_INLINE uint64_t
hash(const void *key, size_t len, uint64_t seed)
{
	const uint64_t m = 0xc6a4a7935bd1e995LLU;
	const int r = 47;
	uint64_t h = seed ^ (len * m);
	const uint64_t *data = (const uint64_t *)key;
	const uint64_t *end = data + (len/8);
	const unsigned char *data2;

	assert(((uintptr_t)key & 0x7) == 0);

	while(data != end) {
		uint64_t k = *data++;

		k *= m;
		k ^= k >> r;
		k *= m;

		h ^= k;
		h *= m;
	}

	data2 = (const unsigned char *)data;
	switch(len & 7) {
		case 7: h ^= ((uint64_t)(data2[6])) << 48;
		case 6: h ^= ((uint64_t)(data2[5])) << 40;
		case 5: h ^= ((uint64_t)(data2[4])) << 32;
		case 4: h ^= ((uint64_t)(data2[3])) << 24;
		case 3: h ^= ((uint64_t)(data2[2])) << 16;
		case 2: h ^= ((uint64_t)(data2[1])) << 8;
		case 1: h ^= ((uint64_t)(data2[0]));
			h *= m;
	}

	h ^= h >> r;
	h *= m;
	h ^= h >> r;

	return (h);
}
#endif

#endif /* JEMALLOC_H_INLINES */
/******************************************************************************/
