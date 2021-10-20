
# find the OS
uname_S := $(shell sh -c 'uname -s 2>/dev/null || echo not')

# Compile flags for linux / osx
ifeq ($(uname_S),Linux)
	SHOBJ_CFLAGS ?= -W -Wall -fno-common -g -ggdb -std=c99 -O2
	SHOBJ_LDFLAGS ?= -shared
else
	SHOBJ_CFLAGS ?= -W -Wall -dynamic -fno-common -g -ggdb -std=c99 -O2
	SHOBJ_LDFLAGS ?= -bundle -undefined dynamic_lookup
endif

# OS X 11.x doesn't have /usr/lib/libSystem.dylib and needs an explicit setting.
ifeq ($(uname_S),Darwin)
ifeq ("$(wildcard /usr/lib/libSystem.dylib)","")
LIBS = -L /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/lib -lsystem
endif
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
    zset.so \
    getkeys.so


.PHONY: all

all: $(TEST_MODULES)

32bit:
	$(MAKE) CFLAGS="-m32" LDFLAGS="-melf_i386"

%.xo: %.c ../../src/redismodule.h
	$(CC) -I../../src $(CFLAGS) $(SHOBJ_CFLAGS) -fPIC -c $< -o $@

%.so: %.xo
	$(LD) -o $@ $< $(SHOBJ_LDFLAGS) $(LDFLAGS) $(LIBS) -lc

.PHONY: clean

clean:
	rm -f $(TEST_MODULES) $(TEST_MODULES:.so=.xo)
