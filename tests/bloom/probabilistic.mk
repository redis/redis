test-probabilistic: test-bloom
.PHONY: test-bloom

bloom-unit: ../tests/bloom/unit.c ../tests/probabilistic/unit_support.h probabilistic/bloom.o probabilistic/bloom_filter.o probabilistic/bloom_murmur.o zmalloc.o
	$(REDIS_CC) $(FINAL_LDFLAGS) -I. -o $@ ../tests/bloom/unit.c probabilistic/bloom.o probabilistic/bloom_filter.o probabilistic/bloom_murmur.o zmalloc.o $(FINAL_LIBS)

bloom-perf: ../tests/bloom/perf.c probabilistic/bloom.o probabilistic/bloom_filter.o probabilistic/bloom_murmur.o zmalloc.o
	$(REDIS_CC) $(FINAL_LDFLAGS) -I. -o $@ ../tests/bloom/perf.c probabilistic/bloom.o probabilistic/bloom_filter.o probabilistic/bloom_murmur.o zmalloc.o $(FINAL_LIBS)

test-bloom: $(REDIS_SERVER_NAME) bloom-unit
	./bloom-unit
	$(PYTHON) ../tests/bloom/run.py
