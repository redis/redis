# Gate data type: loadable Redis module, unit tests and simulations.
#
#   make                 build gate.so
#   make test-unit       build and run the standalone C unit tests
#   make sim             build and run the certification Monte Carlo
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
STD = -std=gnu11

uname_S := $(shell sh -c 'uname -s 2>/dev/null || echo not')
ifeq ($(uname_S),Darwin)
	SHOBJ_CFLAGS ?= -dynamic -fno-common
	SHOBJ_LDFLAGS ?= -bundle -undefined dynamic_lookup
else
	SHOBJ_CFLAGS ?= -fno-common
	SHOBJ_LDFLAGS ?= -shared
endif

# The library: plain C, no Redis dependency.
LIB_SRCS = gate_alloc.c gate_stats.c gate_model.c gate_features.c gate_config.c gate_core.c gate_codec.c gate_args.c
LIB_HDRS = gate_defs.h gate_stats.h gate_model.h gate_features.h gate_core.h gate_internal.h gate_codec.h gate_args.h
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

$(T)/test_stats: $(T)/test_stats.c gate_alloc.c gate_stats.c gate_defs.h gate_stats.h $(T)/testutil.h
	$(TEST_CC) -o $@ $(T)/test_stats.c gate_alloc.c gate_stats.c -lm

$(T)/test_model: $(T)/test_model.c gate_alloc.c gate_model.c gate_defs.h gate_model.h $(T)/testutil.h
	$(TEST_CC) -o $@ $(T)/test_model.c gate_alloc.c gate_model.c -lm

$(T)/test_features: $(T)/test_features.c gate_alloc.c gate_features.c gate_defs.h gate_features.h $(T)/testutil.h
	$(TEST_CC) -o $@ $(T)/test_features.c gate_alloc.c gate_features.c -lm

CORE_SRCS = gate_alloc.c gate_stats.c gate_model.c gate_config.c gate_core.c
$(T)/test_core: $(T)/test_core.c $(CORE_SRCS) $(LIB_HDRS) $(T)/testutil.h
	$(TEST_CC) -o $@ $(T)/test_core.c $(CORE_SRCS) -lm

CODEC_SRCS = $(CORE_SRCS) gate_codec.c
$(T)/test_codec: $(T)/test_codec.c $(CODEC_SRCS) $(LIB_HDRS) $(T)/testutil.h
	$(TEST_CC) -o $@ $(T)/test_codec.c $(CODEC_SRCS) -lm

ARGS_SRCS = gate_alloc.c gate_model.c gate_config.c gate_args.c
$(T)/test_args: $(T)/test_args.c $(ARGS_SRCS) $(LIB_HDRS) $(T)/testutil.h
	$(TEST_CC) -o $@ $(T)/test_args.c $(ARGS_SRCS) -lm

UNIT_TESTS = $(T)/test_stats $(T)/test_model $(T)/test_features $(T)/test_core $(T)/test_codec $(T)/test_args

test-unit: $(UNIT_TESTS)
	@set -e; for t in $(UNIT_TESTS); do echo "== $$t"; ./$$t; done

# --------------------------------------------------------------- simulations
S = tests/sim

$(S)/sim_certify: $(S)/sim_certify.c gate_alloc.c gate_stats.c gate_defs.h gate_stats.h $(T)/testutil.h
	$(TEST_CC) -o $@ $(S)/sim_certify.c gate_alloc.c gate_stats.c -lm

$(S)/sim_protocol: $(S)/sim_protocol.c $(CORE_SRCS) $(LIB_HDRS) $(T)/testutil.h
	$(TEST_CC) -o $@ $(S)/sim_protocol.c $(CORE_SRCS) -lm

sim: $(S)/sim_certify $(S)/sim_protocol
	./$(S)/sim_certify
	./$(S)/sim_protocol

# --------------------------------------------------------- integration tests
# Driven by the Redis Tcl test suite from the repository root.
test-integration: gate.so
	cd ../.. && ./runtest --single gate/commands --single gate/protocol \
	    --single gate/classifier --single gate/persistence \
	    --single gate/replication

test: test-unit test-integration

clean:
	rm -f *.xo *.so $(UNIT_TESTS) $(S)/sim_certify $(S)/sim_protocol

.PHONY: all clean test test-unit test-integration sim
