REDIS_PROBABILISTIC_OBJ += probabilistic/t_bloom.o probabilistic/bloom.o probabilistic/bloom_filter.o probabilistic/bloom_rdb.o
FINAL_CFLAGS += -DINCLUDE_BLOOM=1
