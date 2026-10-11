test-probabilistic: test-topk
.PHONY: test-topk

topk-unit: ../tests/topk/unit.c probabilistic/topk.o probabilistic/bloom_murmur.o zmalloc.o
	$(REDIS_CC) $(FINAL_LDFLAGS) -I. -o $@ ../tests/topk/unit.c probabilistic/topk.o probabilistic/bloom_murmur.o zmalloc.o $(FINAL_LIBS)

test-topk: $(REDIS_SERVER_NAME) topk-unit
	./topk-unit
	$(PYTHON) ../tests/topk/run.py

.PHONY: test-topk
