# Gate data type: loadable Redis module, unit tests and simulations.
#
#   make                 build gate.so
#   make test-unit       build and run the standalone C unit tests
#   make sim             build and run the Monte Carlo simulations
#   make test            unit tests + Redis integration tests (needs a built
#                        ../../src/redis-server and tclsh)
#   make clean
#
# Sanitizers: make SANITIZER=address|undefined test-unit

CC ?= cc

ifdef SANITIZER
ifeq ($(SANITIZER),address)
	SAN=-fsanitize=address -fno-omit-frame-pointer
else
ifeq ($(SANITIZER),undefined)
	SAN=-fsanitize=undefined -fno-sanitize-recover=undefined
else
    $(error "unknown sanitizer=${SANITIZER}")
endif
endif
endif

WARN = -W -Wall -Wextra -Wno-missing-field-initializers
CFLAGS ?= -O2 -g -ggdb
# Replicas repeat the primary's arithmetic and must get the same bits on
# every architecture: no fused multiply-add behind our back.
STD = -std=gnu11 -ffp-contract=off

uname_S := $(shell sh -c 'uname -s 2>/dev/null || echo not')
ifeq ($(uname_S),Darwin)
	SHOBJ_CFLAGS ?= -dynamic -fno-common
	SHOBJ_LDFLAGS ?= -bundle -undefined dynamic_lookup
else
	SHOBJ_CFLAGS ?= -fno-common
	SHOBJ_LDFLAGS ?= -shared
endif

# The library: plain C, no Redis dependency.
LIB_SRCS = gate_alloc.c gate_stats.c gate_model.c gate_features.c gate_core.c gate_codec.c gate_args.c
LIB_HDRS = gate_defs.h gate_stats.h gate_model.h gate_features.h gate_core.h gate_codec.h gate_args.h
MODULE_OBJS = gate.xo $(LIB_SRCS:.c=.xo)

all: gate.so

%.xo: %.c $(LIB_HDRS) ../../src/redismodule.h
	$(CC) -I. -I../../src $(STD) $(WARN) $(CFLAGS) $(SHOBJ_CFLAGS) $(SAN) -fPIC -c $< -o $@

gate.so: $(MODULE_OBJS)
	$(CC) -o $@ $^ $(SHOBJ_LDFLAGS) $(SAN) -lm

# ---------------------------------------------------------------- unit tests
# Each test program links only the library sources it exercises, so the
# layers can be built and tested independently.
T = tests/unit
TEST_CC = $(CC) -I. -I$(T) $(STD) $(WARN) $(CFLAGS) $(SAN)
CORE_SRCS = gate_alloc.c gate_stats.c gate_model.c gate_core.c

TEST_DEPS = $(LIB_HDRS) $(T)/testutil.h

$(T)/test_stats: $(T)/test_stats.c gate_alloc.c gate_stats.c $(TEST_DEPS)
	$(TEST_CC) -o $@ $(T)/test_stats.c gate_alloc.c gate_stats.c -lm

$(T)/test_model: $(T)/test_model.c gate_alloc.c gate_model.c $(TEST_DEPS)
	$(TEST_CC) -o $@ $(T)/test_model.c gate_alloc.c gate_model.c -lm

$(T)/test_features: $(T)/test_features.c gate_alloc.c gate_features.c $(TEST_DEPS)
	$(TEST_CC) -o $@ $(T)/test_features.c gate_alloc.c gate_features.c -lm

$(T)/test_core: $(T)/test_core.c $(CORE_SRCS) $(TEST_DEPS)
	$(TEST_CC) -o $@ $(T)/test_core.c $(CORE_SRCS) -lm

$(T)/test_args: $(T)/test_args.c $(CORE_SRCS) gate_args.c $(TEST_DEPS)
	$(TEST_CC) -o $@ $(T)/test_args.c $(CORE_SRCS) gate_args.c -lm

$(T)/test_codec: $(T)/test_codec.c $(CORE_SRCS) gate_codec.c $(TEST_DEPS)
	$(TEST_CC) -o $@ $(T)/test_codec.c $(CORE_SRCS) gate_codec.c -lm

UNIT_TESTS = $(T)/test_stats $(T)/test_model $(T)/test_features $(T)/test_core $(T)/test_args $(T)/test_codec

test-unit: $(UNIT_TESTS)
	@set -e; for t in $(UNIT_TESTS); do echo "== $$t"; ./$$t; done

# --------------------------------------------------------------- simulations
S = tests/sim

$(S)/sim_certify: $(S)/sim_certify.c gate_alloc.c gate_stats.c $(TEST_DEPS)
	$(TEST_CC) -o $@ $(S)/sim_certify.c gate_alloc.c gate_stats.c -lm

$(S)/sim_protocol: $(S)/sim_protocol.c $(CORE_SRCS) $(TEST_DEPS)
	$(TEST_CC) -o $@ $(S)/sim_protocol.c $(CORE_SRCS) -lm

sim: $(S)/sim_certify $(S)/sim_protocol
	./$(S)/sim_certify
	./$(S)/sim_protocol

# --------------------------------------------------------- integration tests
# Driven by the Redis Tcl test suite from the repository root.
I = ../modules/gate/tests/integration
test-integration: gate.so
	cd ../.. && ./runtest --single $(I)/commands --single $(I)/lifecycle \
	    --single $(I)/persistence --single $(I)/replication

test: test-unit test-integration

clean:
	rm -f *.xo *.so $(UNIT_TESTS) $(S)/sim_certify $(S)/sim_protocol

.PHONY: all clean test test-unit test-integration sim
