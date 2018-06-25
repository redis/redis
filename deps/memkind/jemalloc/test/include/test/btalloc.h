/* btalloc() provides a mechanism for allocating via permuted backtraces. */
void	*btalloc(size_t size, unsigned bits);

#define	btalloc_n_proto(n)						\
void	*btalloc_##n(size_t size, unsigned bits);
btalloc_n_proto(0)
btalloc_n_proto(1)

#define	btalloc_n_gen(n)						\
void *									\
btalloc_##n(size_t size, unsigned bits)					\
{									\
	void *p;							\
									\
	if (bits == 0)							\
		p = mallocx(size, 0);					\
	else {								\
		switch (bits & 0x1U) {					\
		case 0:							\
			p = (btalloc_0(size, bits >> 1));		\
			break;						\
		case 1:							\
			p = (btalloc_1(size, bits >> 1));		\
			break;						\
		default: not_reached();					\
		}							\
	}								\
	/* Intentionally sabotage tail call optimization. */		\
	assert_ptr_not_null(p, "Unexpected mallocx() failure");		\
	return (p);							\
}
