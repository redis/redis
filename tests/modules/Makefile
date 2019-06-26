
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

.SUFFIXES: .c .so .xo .o

all: commandfilter.so

.c.xo:
	$(CC) -I../../src $(CFLAGS) $(SHOBJ_CFLAGS) -fPIC -c $< -o $@

commandfilter.xo: ../../src/redismodule.h

commandfilter.so: commandfilter.xo
	$(LD) -o $@ $< $(SHOBJ_LDFLAGS) $(LIBS) -lc
