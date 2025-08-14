
# find the OS
uname_S := $(shell sh -c 'uname -s 2>/dev/null || echo not')

warning_cflags = -W -Wall -Wno-missing-field-initializers
ifeq ($(uname_S),Darwin)
	SHOBJ_CFLAGS ?= $(warning_cflags) -dynamic -fno-common -g -ggdb -std=gnu11 -O2
	SHOBJ_LDFLAGS ?= -bundle -undefined dynamic_lookup
else	# Linux, others
	SHOBJ_CFLAGS ?= $(warning_cflags) -fno-common -g -ggdb -std=gnu11 -O2
	SHOBJ_LDFLAGS ?= -shared
endif

CLANG := $(findstring clang,$(shell sh -c '$(CC) --version | head -1'))

ifeq ($(SANITIZER),memory)
ifeq (clang, $(CLANG))
	LD=clang
	MALLOC=libc
	CFLAGS+=-fsanitize=memory -fsanitize-memory-track-origins=2 -fno-sanitize-recover=all -fno-omit-frame-pointer
	LDFLAGS+=-fsanitize=memory
else
    $(error "MemorySanitizer needs to be compiled and linked with clang. Please use CC=clang")
endif
endif


# This is a hack to override the default CC. When running with SANITIZER=memory
# tough we want to keep the compiler as clang as MSan is not supported for gcc
ifeq ($(uname_S),Linux)
ifneq ($(SANITIZER),memory)
	LD = gcc
	CC = gcc
endif
endif

# OS X 11.x doesn't have /usr/lib/libSystem.dylib and needs an explicit setting.
ifeq ($(uname_S),Darwin)
ifeq ("$(wildcard /usr/lib/libSystem.dylib)","")
LIBS = -L /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/lib -lsystem
endif
endif

TEST_MODULES = \
    commandfilter.so \
    basics.so \
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
    datatype2.so \
    auth.so \
    keyspace_events.so \
    blockedclient.so \
    getkeys.so \
    getchannels.so \
    test_lazyfree.so \
    timer.so \
    defragtest.so \
    keyspecs.so \
    hash.so \
    zset.so \
    stream.so \
    mallocsize.so \
    aclcheck.so \
    list.so \
    subcommands.so \
    reply.so \
    cmdintrospection.so \
    eventloop.so \
    moduleconfigs.so \
    moduleconfigstwo.so \
    publish.so \
    usercall.so \
    postnotifications.so \
    moduleauthtwo.so \
    rdbloadsave.so \
    crash.so \
    internalsecret.so \
    configaccess.so

.PHONY: all

all: $(TEST_MODULES)

32bit:
	$(MAKE) CFLAGS="-m32" LDFLAGS="-m32"

%.xo: %.c ../../src/redismodule.h
	$(CC) -I../../src $(CFLAGS) $(SHOBJ_CFLAGS) -fPIC -c $< -o $@

%.so: %.xo
	$(LD) -o $@ $^ $(SHOBJ_LDFLAGS) $(LDFLAGS) $(LIBS)

.PHONY: clean

clean:
	rm -f $(TEST_MODULES) $(TEST_MODULES:.so=.xo)
