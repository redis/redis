
# find the OS
uname_S := $(shell sh -c 'uname -s 2>/dev/null || echo not')

ifeq ($(uname_S),Darwin)
	SHOBJ_CFLAGS ?= -W -Wall -dynamic -fno-common -g -ggdb -std=c99 -O2
	SHOBJ_LDFLAGS ?= -bundle -undefined dynamic_lookup
else	# Linux, others
	SHOBJ_CFLAGS ?= -W -Wall -fno-common -g -ggdb -std=c99 -O2
	SHOBJ_LDFLAGS ?= -shared
endif

# Needed to satisfy __stack_chk_fail_local on Linux with -m32, due to gcc
# -fstack-protector by default. Breaks on FreeBSD so we exclude it.
ifneq ($(uname_S),FreeBSD)
    LIBS = -lc
endif

TEST_MODULES = \
    commandfilter.so \
    testrdb.so \
    fork.so \
    infotest.so \
    propagate.so \
    misc.so \
    hooks.so \
    blockonkeys.so \
    blockonbackground.so \
    scan.so \
    datatype.so \
    auth.so \
    keyspace_events.so \
    blockedclient.so \
    getkeys.so \
    test_lazyfree.so \
    timer.so \
    defragtest.so \
    hash.so \
    zset.so \
    stream.so \
    subscribe_channel.so


.PHONY: all

all: $(TEST_MODULES)

32bit:
	$(MAKE) CFLAGS="-m32" LDFLAGS="-melf_i386"

%.xo: %.c ../../src/redismodule.h
	$(CC) -I../../src $(CFLAGS) $(SHOBJ_CFLAGS) -fPIC -c $< -o $@

%.so: %.xo
	$(LD) -o $@ $< $(SHOBJ_LDFLAGS) $(LDFLAGS) $(LIBS)

.PHONY: clean

clean:
	rm -f $(TEST_MODULES) $(TEST_MODULES:.so=.xo)
