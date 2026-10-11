test-probabilistic: test-cms
.PHONY: test-cms

cms-unit: ../tests/cms/unit.c probabilistic/cms.o probabilistic/bloom_murmur.o zmalloc.o
	$(REDIS_CC) $(FINAL_LDFLAGS) -I. -o $@ ../tests/cms/unit.c probabilistic/cms.o probabilistic/bloom_murmur.o zmalloc.o $(FINAL_LIBS)

test-cms: $(REDIS_SERVER_NAME) cms-unit
	./cms-unit
	$(PYTHON) ../tests/cms/run.py

.PHONY: test-cms
