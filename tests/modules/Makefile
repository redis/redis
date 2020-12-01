
# find the OS
uname_S := $(shell sh -c 'uname -s 2>/dev/null || echo not')

# "which" on Solaris is an extremely broken tcsh script
which = $(firstword $(realpath $(foreach x,$(subst :, ,../../src:${PATH}),${x}/${1})))

# The default is "cc" which isn't always gcc (eg, Solaris).
# Since SHOBJ_CFLAGS uses the GNU-specific "-ggdb", this choice
# makes sense.
CC := $(if $(call which,gcc),gcc,cc)

redis_server_path := $(call which,redis-server)

ifneq (,$(shell file ${redis_server_path} | grep -i 32-bit))
	32_or_64bit := -m32
else
	32_or_64bit := -m64
endif

# Compile flags
ifneq ($(uname_S),Mac)
	SHOBJ_CFLAGS ?= -W -Wall -fno-common -g -ggdb -std=c99 -O2
	SHOBJ_LDFLAGS ?= -shared ${32_or_64bit}
else
	SHOBJ_CFLAGS ?= -W -Wall -dynamic -fno-common -g -ggdb -std=c99 -O2 ${32_or_64bit}
	SHOBJ_LDFLAGS ?= -bundle -undefined dynamic_lookup ${32_or_64bit}
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
    scan.so \
    datatype.so \
    auth.so \
    keyspace_events.so \
    blockedclient.so \
    getkeys.so \
    test_lazyfree.so \
    timer.so \


.PHONY: all

all 32bit: $(TEST_MODULES)

%.xo: %.c ../../src/redismodule.h
	$(CC) -I../../src $(CFLAGS) $(SHOBJ_CFLAGS) -fPIC -c $< -o $@

%.so: %.xo
	$(CC) -o $@ $< $(SHOBJ_LDFLAGS) $(LDFLAGS) $(LIBS) -lc

.PHONY: clean

clean:
	rm -f $(TEST_MODULES) $(TEST_MODULES:.so=.xo)
