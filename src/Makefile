# Redis Makefile
# Copyright (C) 2009 Salvatore Sanfilippo <antirez at gmail dot com>
# This file is released under the BSD license, see the COPYING file
#
# The Makefile composes the final FINAL_CFLAGS and FINAL_LDFLAGS using
# what is needed for Redis plus the standard CFLAGS and LDFLAGS passed.
# However when building the dependencies (Jemalloc, Lua, Hiredis, ...)
# CFLAGS and LDFLAGS are propagated to the dependencies, so to pass
# flags only to be used when compiling / linking Redis itself REDIS_CFLAGS
# and REDIS_LDFLAGS are used instead (this is the case of 'make gcov').
#
# Dependencies are stored in the Makefile.dep file. To rebuild this file
# Just use 'make dep', but this is only needed by developers.

ifeq (,$(filter obj.%,$(notdir $(CURDIR))))
include target.mk
else

VPATH = .:$(SRCDIR)
BUILD_ARCH ?= $(shell uname -m)
export BUILD_ARCH

ifeq ($(BUILD_ARCH), i686)
CFLAGS="-m32"
LDFLAGS="-m32"
BIN_SUFFIX=.i686
endif

release_hdr := $(shell sh -c 'cd $(SRCDIR) ; ./mkreleasehdr.sh')
uname_S := $(shell sh -c 'uname -s 2>/dev/null || echo not')
OPTIMIZATION?=-O2
DEPENDENCY_TARGETS=hiredis linenoise lua

# Default settings
STD= -std=c99 -pedantic
WARN= -Wall
OPT= $(OPTIMIZATION)

# Default allocator
ifeq ($(uname_S),Linux)
  MALLOC=jemalloc
else
  MALLOC=libc
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

# Override default settings if possible
-include .make-settings

ifeq ($(uname_S),SunOS)
  FINAL_CFLAGS= $(STD) $(WARN) $(OPT) $(DEBUG) $(CFLAGS) $(REDIS_CFLAGS) -D__EXTENSIONS__ -D_XPG6
  FINAL_LDFLAGS= $(LDFLAGS) $(REDIS_LDFLAGS) -g -ggdb
  FINAL_LIBS= -ldl -lnsl -lsocket -lm -lpthread
  DEBUG= -g -ggdb
else
  FINAL_CFLAGS= $(STD) $(WARN) $(OPT) $(DEBUG) $(CFLAGS) $(REDIS_CFLAGS)
  FINAL_LDFLAGS= $(LDFLAGS) $(REDIS_LDFLAGS) -g -rdynamic -ggdb
  FINAL_LIBS= -lm -pthread
  DEBUG= -g -rdynamic -ggdb
endif

# Include paths to dependencies
FINAL_CFLAGS+= -I$(SRCDIR)/../deps/hiredis -I$(SRCDIR)/../deps/linenoise -I$(SRCDIR)/../deps/lua/src

ifeq ($(MALLOC),tcmalloc)
  FINAL_CFLAGS+= -DUSE_TCMALLOC
  FINAL_LIBS+= -ltcmalloc
endif

ifeq ($(MALLOC),tcmalloc_minimal)
  FINAL_CFLAGS+= -DUSE_TCMALLOC
  FINAL_LIBS+= -ltcmalloc_minimal
endif

ifeq ($(MALLOC),jemalloc)
  DEPENDENCY_TARGETS+= jemalloc
  FINAL_CFLAGS+= -DUSE_JEMALLOC -I$(SRCDIR)/../deps/jemalloc/$(OBJDIR)/include
  FINAL_LIBS+= $(SRCDIR)/../deps/jemalloc/$(OBJDIR)/lib/libjemalloc.a -ldl
endif

REDIS_CC=$(QUIET_CC)$(CC) $(FINAL_CFLAGS)
REDIS_LD=$(QUIET_LINK)$(CC) $(FINAL_LDFLAGS)
REDIS_INSTALL=$(QUIET_INSTALL)$(INSTALL)

PREFIX?=/usr/local
INSTALL_BIN= $(PREFIX)/bin
INSTALL= cp -pf

CCCOLOR="\033[34m"
LINKCOLOR="\033[34;1m"
SRCCOLOR="\033[33m"
BINCOLOR="\033[37;1m"
MAKECOLOR="\033[32;1m"
ENDCOLOR="\033[0m"

ifndef V
QUIET_CC = @printf '    %b %b\n' $(CCCOLOR)CC$(ENDCOLOR) $(SRCCOLOR)$@$(ENDCOLOR) 1>&2;
QUIET_LINK = @printf '    %b %b\n' $(LINKCOLOR)LINK$(ENDCOLOR) $(BINCOLOR)$@$(ENDCOLOR) 1>&2;
QUIET_INSTALL = @printf '    %b %b\n' $(LINKCOLOR)INSTALL$(ENDCOLOR) $(BINCOLOR)$@$(ENDCOLOR) 1>&2;
endif

REDIS_SERVER_NAME= redis-server
REDIS_SENTINEL_NAME= redis-sentinel
REDIS_SERVER_OBJ= adlist.o ae.o anet.o dict.o redis.o sds.o zmalloc.o lzf_c.o lzf_d.o pqsort.o zipmap.o sha1.o ziplist.o release.o networking.o util.o object.o db.o replication.o rdb.o t_string.o t_list.o t_set.o t_zset.o t_hash.o config.o aof.o pubsub.o multi.o debug.o sort.o intset.o syncio.o migrate.o endianconv.o slowlog.o scripting.o bio.o rio.o rand.o memtest.o crc64.o bitops.o sentinel.o
REDIS_CLI_NAME= redis-cli
REDIS_CLI_OBJ= anet.o sds.o adlist.o redis-cli.o zmalloc.o release.o anet.o ae.o
REDIS_BENCHMARK_NAME= redis-benchmark
REDIS_BENCHMARK_OBJ= ae.o anet.o redis-benchmark.o sds.o adlist.o zmalloc.o redis-benchmark.o
REDIS_CHECK_DUMP_NAME= redis-check-dump
REDIS_CHECK_DUMP_OBJ= redis-check-dump.o lzf_c.o lzf_d.o crc64.o
REDIS_CHECK_AOF_NAME= redis-check-aof
REDIS_CHECK_AOF_OBJ= redis-check-aof.o

# Common dependencies
LIBHIREDIS=$(SRCDIR)/../deps/hiredis/$(OBJDIR)/libhiredis.a
LINENOISE=$(SRCDIR)/../deps/linenoise/$(OBJDIR)/linenoise.o
LIBLUA=$(SRCDIR)/../deps/lua/$(OBJDIR)/liblua.a

all: $(REDIS_SERVER_NAME) $(REDIS_SENTINEL_NAME) $(REDIS_CLI_NAME) $(REDIS_BENCHMARK_NAME) $(REDIS_CHECK_DUMP_NAME) $(REDIS_CHECK_AOF_NAME)
	@echo ""
	@echo "Hint: To run 'make test' is a good idea ;)"
	@echo ""

.PHONY: all

# Deps (use make dep to generate this)
include $(SRCDIR)/Makefile.dep

dep:
	$(REDIS_CC) -MM *.c > $(SRCDIR)/Makefile.dep

.PHONY: dep

persist-settings: distclean
	echo STD=$(STD) >> .make-settings
	echo WARN=$(WARN) >> .make-settings
	echo OPT=$(OPT) >> .make-settings
	echo MALLOC=$(MALLOC) >> .make-settings
	echo CFLAGS=$(CFLAGS) >> .make-settings
	echo LDFLAGS=$(LDFLAGS) >> .make-settings
	echo REDIS_CFLAGS=$(REDIS_CFLAGS) >> .make-settings
	echo REDIS_LDFLAGS=$(REDIS_LDFLAGS) >> .make-settings
	echo PREV_FINAL_CFLAGS=$(FINAL_CFLAGS) >> .make-settings
	echo PREV_FINAL_LDFLAGS=$(FINAL_LDFLAGS) >> .make-settings
	-(cd $(SRCDIR)/../deps && $(MAKE) $(DEPENDENCY_TARGETS) CFLAGS=$(CFLAGS) LDFLAGS=$(LDFLAGS))

.PHONY: persist-settings

# Prerequisites target
.make-prerequisites:
	@touch $@

# Clean everything, persist settings and build dependencies if anything changed
ifneq ($(strip $(PREV_FINAL_CFLAGS)), $(strip $(FINAL_CFLAGS)))
.make-prerequisites: persist-settings
endif

ifneq ($(strip $(PREV_FINAL_LDFLAGS)), $(strip $(FINAL_LDFLAGS)))
.make-prerequisites: persist-settings
endif

# redis-server
$(REDIS_SERVER_NAME): $(REDIS_SERVER_OBJ)
	$(REDIS_LD) -o $@ $^ $(LIBHIREDIS) $(LIBLUA) $(FINAL_LIBS)

# redis-sentinel
$(REDIS_SENTINEL_NAME): $(REDIS_SERVER_NAME)
	$(REDIS_INSTALL) $(REDIS_SERVER_NAME) $(REDIS_SENTINEL_NAME)

# redis-cli
$(REDIS_CLI_NAME): $(REDIS_CLI_OBJ)
	$(REDIS_LD) -o $@ $^ $(LIBHIREDIS) $(LINENOISE) $(FINAL_LIBS)

# redis-benchmark
$(REDIS_BENCHMARK_NAME): $(REDIS_BENCHMARK_OBJ)
	$(REDIS_LD) -o $@ $^ $(LIBHIREDIS) $(FINAL_LIBS)

# redis-check-dump
$(REDIS_CHECK_DUMP_NAME): $(REDIS_CHECK_DUMP_OBJ)
	$(REDIS_LD) -o $@ $^ $(FINAL_LIBS)

# redis-check-aof
$(REDIS_CHECK_AOF_NAME): $(REDIS_CHECK_AOF_OBJ)
	$(REDIS_LD) -o $@ $^ $(FINAL_LIBS)

# Because the jemalloc.h header is generated as a part of the jemalloc build,
# building it should complete before building any other object. Instead of
# depending on a single artifact, build all dependencies first.
%.o: %.c .make-prerequisites
	$(REDIS_CC) -c $<

clean:
	rm -rf $(REDIS_SERVER_NAME) $(REDIS_SENTINEL_NAME) $(REDIS_CLI_NAME) $(REDIS_BENCHMARK_NAME) $(REDIS_CHECK_DUMP_NAME) $(REDIS_CHECK_AOF_NAME) *.o *.gcda *.gcno *.gcov redis.info lcov-html

.PHONY: clean

distclean: clean
	-(cd $(SRCDIR)/../deps && $(MAKE) distclean)
	-(rm -f .make-*)

.PHONY: distclean

test: $(REDIS_SERVER_NAME) $(REDIS_CHECK_AOF_NAME)
	@(cd $(SRCDIR)/..; ./runtest)

lcov:
	$(MAKE) gcov
	@(set -e; cd $(SRCDIR)/..; ./runtest --clients 1)
	@geninfo -o redis.info .
	@genhtml --legend -o lcov-html redis.info

.PHONY: lcov

bench: $(REDIS_BENCHMARK_NAME)
	./$(REDIS_BENCHMARK_NAME)

32bit:
	@echo "To build a 32-bit version, use BUILD_ARCH=<32-bit architecture>."
	@echo "For example, on Intel platforms use 'make BUILD_ARCH=i686'"

gcov:
	$(MAKE) REDIS_CFLAGS="-fprofile-arcs -ftest-coverage -DCOVERAGE_TEST" REDIS_LDFLAGS="-fprofile-arcs -ftest-coverage"

noopt:
	$(MAKE) OPT="-O0"

src/help.h:
	@$(SRCDIR)/../utils/generate-command-help.rb > help.h

install: all
	mkdir -p $(INSTALL_BIN)
	$(REDIS_INSTALL) $(REDIS_SERVER_NAME) $(INSTALL_BIN)/$(REDIS_SERVER_NAME)$(BIN_SUFFIX)
	$(REDIS_INSTALL) $(REDIS_BENCHMARK_NAME) $(INSTALL_BIN)/$(REDIS_BENCHMARK_NAME)$(BIN_SUFFIX)
	$(REDIS_INSTALL) $(REDIS_CLI_NAME) $(INSTALL_BIN)/$(REDIS_CLI_NAME)$(BIN_SUFFIX)
	$(REDIS_INSTALL) $(REDIS_CHECK_DUMP_NAME) $(INSTALL_BIN)/$(REDIS_CHECK_DUMP_NAME)$(BIN_SUFFIX)
	$(REDIS_INSTALL) $(REDIS_CHECK_AOF_NAME) $(INSTALL_BIN)/$(REDIS_CHECK_AOF_NAME)$(BIN_SUFFIX)

endif
