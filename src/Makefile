# Redis Makefile
# Copyright (C) 2009 Salvatore Sanfilippo <antirez at gmail dot com>
# This file is released under the BSD license, see the COPYING file

release_hdr := $(shell sh -c './mkreleasehdr.sh')
uname_S := $(shell sh -c 'uname -s 2>/dev/null || echo not')
OPTIMIZATION?=-O2
DEPENDENCY_TARGETS=hiredis linenoise lua

STD= -std=c99 -pedantic
WARN= -Wall
OPT= $(OPTIMIZATION)

ifeq ($(uname_S),SunOS)
  R_CFLAGS= $(STD) $(WARN) $(OPT) $(DEBUG) $(CFLAGS) -D__EXTENSIONS__ -D_XPG6
  R_LDFLAGS= $(LDFLAGS)
  R_LIBS= $(LIBS) -ldl -lnsl -lsocket -lm -lpthread
  DEBUG= -g -ggdb
else
  R_CFLAGS= $(STD) $(WARN) $(OPT) $(DEBUG) $(CFLAGS)
  R_LDFLAGS= $(LDFLAGS)
  R_LIBS= $(LIBS) -lm -pthread
  DEBUG= -g -rdynamic -ggdb
endif

# Include paths to dependencies
R_CFLAGS+= -I../deps/hiredis -I../deps/linenoise -I../deps/lua/src

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
  R_CFLAGS+= -DUSE_TCMALLOC
  R_LIBS+= -ltcmalloc
endif

ifeq ($(MALLOC),tcmalloc_minimal)
  R_CFLAGS+= -DUSE_TCMALLOC
  R_LIBS+= -ltcmalloc_minimal
endif

ifeq ($(MALLOC),jemalloc)
  DEPENDENCY_TARGETS+= jemalloc
  R_CFLAGS+= -DUSE_JEMALLOC -I../deps/jemalloc/include
  R_LIBS+= ../deps/jemalloc/lib/libjemalloc.a -ldl
endif

R_CC=$(QUIET_CC)$(CC) $(R_CFLAGS)
R_LD=$(QUIET_LINK)$(CC) $(R_LDFLAGS)

PREFIX= /usr/local
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
endif

R_SERVER_NAME= redis-server
R_SERVER_OBJ= adlist.o ae.o anet.o dict.o redis.o sds.o zmalloc.o lzf_c.o lzf_d.o pqsort.o zipmap.o sha1.o ziplist.o release.o networking.o util.o object.o db.o replication.o rdb.o t_string.o t_list.o t_set.o t_zset.o t_hash.o config.o aof.o pubsub.o multi.o debug.o sort.o intset.o syncio.o migrate.o crc16.o endianconv.o slowlog.o scripting.o bio.o rio.o rand.o memtest.o crc64.o
R_CLI_NAME= redis-cli
R_CLI_OBJ= anet.o sds.o adlist.o redis-cli.o zmalloc.o release.o
R_BENCHMARK_NAME= redis-benchmark
R_BENCHMARK_OBJ= ae.o anet.o redis-benchmark.o sds.o adlist.o zmalloc.o redis-benchmark.o
R_CHECK_DUMP_NAME= redis-check-dump
R_CHECK_DUMP_OBJ= redis-check-dump.o lzf_c.o lzf_d.o
R_CHECK_AOF_NAME= redis-check-aof
R_CHECK_AOF_OBJ= redis-check-aof.o

all: $(R_SERVER_NAME) $(R_CLI_NAME) $(R_BENCHMARK_NAME) $(R_CHECK_DUMP_NAME) $(R_CHECK_AOF_NAME)
	@echo ""
	@echo "Hint: To run 'make test' is a good idea ;)"
	@echo ""

.PHONY: all

# Deps (use make dep to generate this)
include Makefile.dep

dep:
	$(R_CC) -MM *.c

.PHONY: dep

# Prerequisites target
.make-prerequisites:
	@touch $@

# Clean local objects and build dependencies when R_CFLAGS is different
ifneq ($(shell sh -c '[ -f .make-cflags ] && cat .make-cflags || echo none'), $(R_CFLAGS))
.make-cflags: clean
	-(cd ../deps && $(MAKE) $(DEPENDENCY_TARGETS))
	-(echo "$(R_CFLAGS)" > .make-cflags)
.make-prerequisites: .make-cflags
endif

# Clean local objects when R_LDFLAGS is different
ifneq ($(shell sh -c '[ -f .make-ldflags ] && cat .make-ldflags || echo none'), $(R_LDFLAGS))
.make-ldflags: clean
	-(echo "$(R_LDFLAGS)" > .make-ldflags)
.make-prerequisites: .make-ldflags
endif

# Clean local objects when MALLOC is different
ifneq ($(shell sh -c '[ -f .make-malloc ] && cat .make-malloc || echo none'), $(MALLOC))
.make-malloc: clean
	-(echo "$(MALLOC)" > .make-malloc)
.make-prerequisites: .make-malloc
endif

# redis-server
$(R_SERVER_NAME): $(R_SERVER_OBJ)
	$(R_LD) -o $@ $^ ../deps/lua/src/liblua.a $(R_LIBS)

# redis-cli
$(R_CLI_NAME): $(R_CLI_OBJ)
	$(R_LD) -o $@ $^ ../deps/hiredis/libhiredis.a ../deps/linenoise/linenoise.o $(R_LIBS)

# redis-benchmark
$(R_BENCHMARK_NAME): $(R_BENCHMARK_OBJ)
	$(R_LD) -o $@ $^ ../deps/hiredis/libhiredis.a $(R_LIBS)

# redis-check-dump
$(R_CHECK_DUMP_NAME): $(R_CHECK_DUMP_OBJ)
	$(R_LD) -o $@ $^ $(R_LIBS)

# redis-check-aof
$(R_CHECK_AOF_NAME): $(R_CHECK_AOF_OBJ)
	$(R_LD) -o $@ $^ $(R_LIBS)

# Because the jemalloc.h header is generated as a part of the jemalloc build,
# building it should complete before building any other object. Instead of
# depending on a single artifact, build all dependencies first.
%.o: %.c .make-prerequisites
	$(R_CC) -c $<

clean:
	rm -rf $(R_SERVER_NAME) $(R_CLI_NAME) $(R_BENCHMARK_NAME) $(R_CHECK_DUMP_NAME) $(R_CHECK_AOF_NAME) *.o *.gcda *.gcno *.gcov redis.info lcov-html

.PHONY: clean

distclean: clean
	-(cd ../deps && $(MAKE) distclean)
	-(rm -f .make-*)

.PHONY: distclean

test: $(R_SERVER_NAME) $(R_CHECK_AOF_NAME)
	@(cd ..; ./runtest)

lcov:
	$(MAKE) clean gcov
	@(set -e; cd ..; ./runtest --clients 1)
	@geninfo -o redis.info .
	@genhtml --legend -o lcov-html redis.info

.PHONY: lcov

bench: $(R_BENCHMARK_NAME)
	./$(R_BENCHMARK_NAME)

log:
	git log '--pretty=format:%ad %s (%cn)' --date=short > ../Changelog

32bit:
	@echo ""
	@echo "WARNING: if it fails under Linux you probably need to install libc6-dev-i386"
	@echo ""
	$(MAKE) CFLAGS="$(CFLAGS) -m32" LDFLAGS="$(LDFLAGS) -m32"

gprof:
	$(MAKE) CFLAGS="$(CFLAGS) -pg" LDFLAGS="$(LDFLAGS) -pg"

gcov:
	$(MAKE) CFLAGS="$(CFLAGS) -fprofile-arcs -ftest-coverage -DCOVERAGE_TEST"

noopt:
	$(MAKE) OPT="-O0"

32bitgprof:
	$(MAKE) CFLAGS="$(CFLAGS) -m32" LDFLAGS="$(LDFLAGS) -m32" gprof

src/help.h:
	@../utils/generate-command-help.rb > help.h

install: all
	mkdir -p $(INSTALL_BIN)
	$(INSTALL) $(R_SERVER_NAME) $(INSTALL_BIN)
	$(INSTALL) $(R_BENCHMARK_NAME) $(INSTALL_BIN)
	$(INSTALL) $(R_CLI_NAME) $(INSTALL_BIN)
	$(INSTALL) $(R_CHECK_DUMP_NAME) $(INSTALL_BIN)
	$(INSTALL) $(R_CHECK_AOF_NAME) $(INSTALL_BIN)
