uname_S := $(shell sh -c 'uname -s 2>/dev/null || echo not')
OPTIMIZATION?=-O2

STD= -std=c99 -pedantic
WARN= -Wall
OPT= $(OPTIMIZATION)

ifeq ($(uname_S),SunOS)
  REDIS_CFLAGS= $(STD) $(WARN) $(OPT) $(DEBUG) $(CFLAGS) $(ADD_CFLAGS) -D__EXTENSIONS__ -D_XPG6
  REDIS_LDFLAGS= $(LDFLAGS) $(ADD_LDFLAGS)
  REDIS_LIBS= $(LIBS) -ldl -lnsl -lsocket -lm -lpthread
  DEBUG= -g -ggdb
else
  REDIS_CFLAGS= $(STD) $(WARN) $(OPT) $(DEBUG) $(CFLAGS) $(ADD_CFLAGS)
  REDIS_LDFLAGS= $(LDFLAGS) $(ADD_LDFLAGS)
  REDIS_LIBS= $(LIBS) -lm -pthread
  DEBUG= -g -rdynamic -ggdb
endif

# Include paths to dependencies
REDIS_CFLAGS+= -I../src -I../deps/hiredis -I../deps/linenoise -I../deps/lua/src

# Default allocator
ifeq ($(uname_S),Linux)
  MALLOC?=jemalloc
else
  MALLOC?=libc
endif

# Backwards compatibility for selecting an allocator
ifeq ($(USE_TCMALLOC),yes)
  MALLOC=tcmalloc
endif

ifeq ($(USE_TCMALLOC_MINIMAL),yes)
  MALLOC=tcmalloc_minimal
endif

ifeq ($(USE_JEMALLOC),yes)
  MALLOC=jemalloc
endif

ifeq ($(MALLOC),tcmalloc)
  REDIS_CFLAGS+= -DUSE_TCMALLOC
  REDIS_LIBS+= -ltcmalloc
endif

ifeq ($(MALLOC),tcmalloc_minimal)
  REDIS_CFLAGS+= -DUSE_TCMALLOC
  REDIS_LIBS+= -ltcmalloc_minimal
endif

ifeq ($(MALLOC),jemalloc)
  DEPENDENCY_TARGETS+= jemalloc
  REDIS_CFLAGS+= -DUSE_JEMALLOC -I../deps/jemalloc/include
  REDIS_LIBS+= ../deps/jemalloc/lib/libjemalloc.a -ldl
endif

REDIS_CC=$(QUIET_CC)$(CC) $(REDIS_CFLAGS)
REDIS_LD=$(QUIET_LINK)$(CC) $(REDIS_LDFLAGS)

plugin-hello.so: plugin-hello.c
	$(REDIS_CC) -flat_namespace -bundle -undefined suppress -o libhello.so plugin-hello.c
