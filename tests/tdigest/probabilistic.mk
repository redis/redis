test-probabilistic: test-tdigest
.PHONY: test-tdigest

tdigest-unit: ../tests/tdigest/td_test.c ../tests/tdigest/minunit.h probabilistic/tdigest.o zmalloc.o
	$(REDIS_CC) $(FINAL_LDFLAGS) -I. -o $@ ../tests/tdigest/td_test.c probabilistic/tdigest.o zmalloc.o $(FINAL_LIBS)

tdigest-capacity-unit: ../tests/tdigest/td_capacity_test.c probabilistic/tdigest.c probabilistic/tdigest.h zmalloc.o
	$(REDIS_CC) $(FINAL_LDFLAGS) -I. -o $@ ../tests/tdigest/td_capacity_test.c zmalloc.o $(FINAL_LIBS)

tdigest-sort-unit: ../tests/tdigest/td_sort_complexity_test.c probabilistic/tdigest.c probabilistic/tdigest.h zmalloc.o
	$(REDIS_CC) $(FINAL_LDFLAGS) -I. -o $@ ../tests/tdigest/td_sort_complexity_test.c zmalloc.o $(FINAL_LIBS)

test-tdigest: $(REDIS_SERVER_NAME) tdigest-unit tdigest-capacity-unit tdigest-sort-unit
	./tdigest-unit
	./tdigest-capacity-unit
	./tdigest-sort-unit
	$(PYTHON) ../tests/tdigest/run.py

.PHONY: test-tdigest
