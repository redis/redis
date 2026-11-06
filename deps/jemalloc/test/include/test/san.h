#if defined(JEMALLOC_UAF_DETECTION) || defined(JEMALLOC_DEBUG)
#  define TEST_SAN_UAF_ALIGN_ENABLE "lg_san_uaf_align:12"
#  define TEST_SAN_UAF_ALIGN_DISABLE "lg_san_uaf_align:-1"
#else
#  define TEST_SAN_UAF_ALIGN_ENABLE ""
#  define TEST_SAN_UAF_ALIGN_DISABLE ""
#endif

static inline bool
extent_is_guarded(tsdn_t *tsdn, void *ptr) {
	edata_t *edata = emap_edata_lookup(tsdn, &arena_emap_global, ptr);
	return edata_guarded_get(edata);
}

