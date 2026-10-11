test-probabilistic: test-cuckoo
.PHONY: test-cuckoo

cuckoo-unit: ../tests/cuckoo/unit.c probabilistic/cuckoo.o probabilistic/bloom_murmur.o zmalloc.o
	$(REDIS_CC) $(FINAL_LDFLAGS) -I. -o $@ ../tests/cuckoo/unit.c probabilistic/cuckoo.o probabilistic/bloom_murmur.o zmalloc.o $(FINAL_LIBS)

test-cuckoo: $(REDIS_SERVER_NAME) cuckoo-unit
	./cuckoo-unit
	$(PYTHON) ../tests/cuckoo/run.py

.PHONY: test-cuckoo
