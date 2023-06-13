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

release_hdr := $(shell sh -c './mkreleasehdr.sh')
uname_S := $(shell sh -c 'uname -s 2>/dev/null || echo not')
uname_M := $(shell sh -c 'uname -m 2>/dev/null || echo not')
CLANG := $(findstring clang,$(shell sh -c '$(CC) --version | head -1'))
OPTIMIZATION?=-O3
ifeq ($(OPTIMIZATION),-O3)
	ifeq (clang,$(CLANG))
		REDIS_CFLAGS+=-flto
	else
		REDIS_CFLAGS+=-flto=auto
	endif
	REDIS_LDFLAGS+=-flto
endif
DEPENDENCY_TARGETS=hiredis linenoise lua hdr_histogram fpconv
NODEPS:=clean distclean

# Default settings
STD=-pedantic -DREDIS_STATIC=''

# Use -Wno-c11-extensions on clang, either where explicitly used or on
# platforms we can assume it's being used.
ifeq (clang,$(CLANG))
  STD+=-Wno-c11-extensions
else
ifneq (,$(findstring FreeBSD,$(uname_S)))
  STD+=-Wno-c11-extensions
endif
endif
WARN=-Wall -W -Wno-missing-field-initializers -Werror=deprecated-declarations -Wstrict-prototypes
OPT=$(OPTIMIZATION)

# Detect if the compiler supports C11 _Atomic.
# NUMBER_SIGN_CHAR is a workaround to support both GNU Make 4.3 and older versions.
NUMBER_SIGN_CHAR := \#
C11_ATOMIC := $(shell sh -c 'echo "$(NUMBER_SIGN_CHAR)include <stdatomic.h>" > foo.c; \
	$(CC) -std=gnu11 -c foo.c -o foo.o > /dev/null 2>&1; \
	if [ -f foo.o ]; then echo "yes"; rm foo.o; fi; rm foo.c')
ifeq ($(C11_ATOMIC),yes)
	STD+=-std=gnu11
else
	STD+=-std=c99
endif

PREFIX?=/usr/local
INSTALL_BIN=$(PREFIX)/bin
INSTALL=install
PKG_CONFIG?=pkg-config

ifndef PYTHON
PYTHON := $(shell which python3 || which python)
endif

# Default allocator defaults to Jemalloc on Linux and libc otherwise
MALLOC=libc
ifeq ($(uname_S),Linux)
	MALLOC=jemalloc
endif

# To get ARM stack traces if Redis crashes we need a special C flag.
ifneq (,$(filter aarch64 armv%,$(uname_M)))
        CFLAGS+=-funwind-tables
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

ifeq ($(USE_JEMALLOC),no)
	MALLOC=libc
endif

ifdef SANITIZER
ifeq ($(SANITIZER),address)
	MALLOC=libc
	CFLAGS+=-fsanitize=address -fno-sanitize-recover=all -fno-omit-frame-pointer
	LDFLAGS+=-fsanitize=address
else
ifeq ($(SANITIZER),undefined)
	MALLOC=libc
	CFLAGS+=-fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
	LDFLAGS+=-fsanitize=undefined
else
ifeq ($(SANITIZER),thread)
	CFLAGS+=-fsanitize=thread -fno-sanitize-recover=all -fno-omit-frame-pointer
	LDFLAGS+=-fsanitize=thread
else
    $(error "unknown sanitizer=${SANITIZER}")
endif
endif
endif
endif

# Override default settings if possible
-include .make-settings

FINAL_CFLAGS=$(STD) $(WARN) $(OPT) $(DEBUG) $(CFLAGS) $(REDIS_CFLAGS)
FINAL_LDFLAGS=$(LDFLAGS) $(REDIS_LDFLAGS) $(DEBUG)
FINAL_LIBS=-lm
DEBUG=-g -ggdb

# Linux ARM32 needs -latomic at linking time
ifneq (,$(findstring armv,$(uname_M)))
        FINAL_LIBS+=-latomic
endif

ifeq ($(uname_S),SunOS)
	# SunOS
	ifeq ($(findstring -m32,$(FINAL_CFLAGS)),)
		CFLAGS+=-m64
	endif
	ifeq ($(findstring -m32,$(FINAL_LDFLAGS)),)
		LDFLAGS+=-m64
	endif
	DEBUG=-g
	DEBUG_FLAGS=-g
	export CFLAGS LDFLAGS DEBUG DEBUG_FLAGS
	INSTALL=cp -pf
	FINAL_CFLAGS+= -D__EXTENSIONS__ -D_XPG6
	FINAL_LIBS+= -ldl -lnsl -lsocket -lresolv -lpthread -lrt
	ifeq ($(USE_BACKTRACE),yes)
	    FINAL_CFLAGS+= -DUSE_BACKTRACE
	endif
else
ifeq ($(uname_S),Darwin)
	# Darwin
	FINAL_LIBS+= -ldl
	# Homebrew's OpenSSL is not linked to /usr/local to avoid
	# conflicts with the system's LibreSSL installation so it
	# must be referenced explicitly during build.
ifeq ($(uname_M),arm64)
	# Homebrew arm64 uses /opt/homebrew as HOMEBREW_PREFIX
	OPENSSL_PREFIX?=/opt/homebrew/opt/openssl
else
	# Homebrew x86/ppc uses /usr/local as HOMEBREW_PREFIX
	OPENSSL_PREFIX?=/usr/local/opt/openssl
endif
else
ifeq ($(uname_S),AIX)
        # AIX
        FINAL_LDFLAGS+= -Wl,-bexpall
        FINAL_LIBS+=-ldl -pthread -lcrypt -lbsd
else
ifeq ($(uname_S),OpenBSD)
	# OpenBSD
	FINAL_LIBS+= -lpthread
	ifeq ($(USE_BACKTRACE),yes)
	    FINAL_CFLAGS+= -DUSE_BACKTRACE -I/usr/local/include
	    FINAL_LDFLAGS+= -L/usr/local/lib
	    FINAL_LIBS+= -lexecinfo
    	endif

else
ifeq ($(uname_S),NetBSD)
	# NetBSD
	FINAL_LIBS+= -lpthread
	ifeq ($(USE_BACKTRACE),yes)
	    FINAL_CFLAGS+= -DUSE_BACKTRACE -I/usr/pkg/include
	    FINAL_LDFLAGS+= -L/usr/pkg/lib
	    FINAL_LIBS+= -lexecinfo
    	endif
else
ifeq ($(uname_S),FreeBSD)
	# FreeBSD
	FINAL_LIBS+= -lpthread -lexecinfo
else
ifeq ($(uname_S),DragonFly)
	# DragonFly
	FINAL_LIBS+= -lpthread -lexecinfo
else
ifeq ($(uname_S),OpenBSD)
	# OpenBSD
	FINAL_LIBS+= -lpthread -lexecinfo
else
ifeq ($(uname_S),NetBSD)
	# NetBSD
	FINAL_LIBS+= -lpthread -lexecinfo
else
ifeq ($(uname_S),Haiku)
	# Haiku
	FINAL_CFLAGS+= -DBSD_SOURCE
	FINAL_LDFLAGS+= -lbsd -lnetwork
	FINAL_LIBS+= -lpthread
else
	# All the other OSes (notably Linux)
	FINAL_LDFLAGS+= -rdynamic
	FINAL_LIBS+=-ldl -pthread -lrt
endif
endif
endif
endif
endif
endif
endif
endif
endif
endif

ifdef OPENSSL_PREFIX
	OPENSSL_CFLAGS=-I$(OPENSSL_PREFIX)/include
	OPENSSL_LDFLAGS=-L$(OPENSSL_PREFIX)/lib
	# Also export OPENSSL_PREFIX so it ends up in deps sub-Makefiles
	export OPENSSL_PREFIX
endif

# Include paths to dependencies
FINAL_CFLAGS+= -I../deps/hiredis -I../deps/linenoise -I../deps/lua/src -I../deps/hdr_histogram -I../deps/fpconv

# Determine systemd support and/or build preference (defaulting to auto-detection)
BUILD_WITH_SYSTEMD=no
LIBSYSTEMD_LIBS=-lsystemd

# If 'USE_SYSTEMD' in the environment is neither "no" nor "yes", try to
# auto-detect libsystemd's presence and link accordingly.
ifneq ($(USE_SYSTEMD),no)
	LIBSYSTEMD_PKGCONFIG := $(shell $(PKG_CONFIG) --exists libsystemd && echo $$?)
# If libsystemd cannot be detected, continue building without support for it
# (unless a later check tells us otherwise)
ifeq ($(LIBSYSTEMD_PKGCONFIG),0)
	BUILD_WITH_SYSTEMD=yes
	LIBSYSTEMD_LIBS=$(shell $(PKG_CONFIG) --libs libsystemd)
endif
endif

# If 'USE_SYSTEMD' is set to "yes" use pkg-config if available or fall back to
# default -lsystemd.
ifeq ($(USE_SYSTEMD),yes)
	BUILD_WITH_SYSTEMD=yes
endif

ifeq ($(BUILD_WITH_SYSTEMD),yes)
	FINAL_LIBS+=$(LIBSYSTEMD_LIBS)
	FINAL_CFLAGS+= -DHAVE_LIBSYSTEMD
endif

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
	FINAL_CFLAGS+= -DUSE_JEMALLOC -I../deps/jemalloc/include
	FINAL_LIBS := ../deps/jemalloc/lib/libjemalloc.a $(FINAL_LIBS)
endif

# LIBSSL & LIBCRYPTO
LIBSSL_LIBS=
LIBSSL_PKGCONFIG := $(shell $(PKG_CONFIG) --exists libssl && echo $$?)
ifeq ($(LIBSSL_PKGCONFIG),0)
	LIBSSL_LIBS=$(shell $(PKG_CONFIG) --libs libssl)
else
	LIBSSL_LIBS=-lssl
endif
LIBCRYPTO_LIBS=
LIBCRYPTO_PKGCONFIG := $(shell $(PKG_CONFIG) --exists libcrypto && echo $$?)
ifeq ($(LIBCRYPTO_PKGCONFIG),0)
	LIBCRYPTO_LIBS=$(shell $(PKG_CONFIG) --libs libcrypto)
else
	LIBCRYPTO_LIBS=-lcrypto
endif

BUILD_NO:=0
BUILD_YES:=1
BUILD_MODULE:=2
ifeq ($(BUILD_TLS),yes)
	FINAL_CFLAGS+=-DUSE_OPENSSL=$(BUILD_YES) $(OPENSSL_CFLAGS) -DBUILD_TLS_MODULE=$(BUILD_NO)
	FINAL_LDFLAGS+=$(OPENSSL_LDFLAGS)
	FINAL_LIBS += ../deps/hiredis/libhiredis_ssl.a $(LIBSSL_LIBS) $(LIBCRYPTO_LIBS)
endif

TLS_MODULE=
TLS_MODULE_NAME:=redis-tls$(PROG_SUFFIX).so
TLS_MODULE_CFLAGS:=$(FINAL_CFLAGS)
ifeq ($(BUILD_TLS),module)
	FINAL_CFLAGS+=-DUSE_OPENSSL=$(BUILD_MODULE) $(OPENSSL_CFLAGS)
	TLS_CLIENT_LIBS = ../deps/hiredis/libhiredis_ssl.a $(LIBSSL_LIBS) $(LIBCRYPTO_LIBS)
	TLS_MODULE=$(TLS_MODULE_NAME)
	TLS_MODULE_CFLAGS+=-DUSE_OPENSSL=$(BUILD_MODULE) $(OPENSSL_CFLAGS) -DBUILD_TLS_MODULE=$(BUILD_MODULE)
endif

ifndef V
    define MAKE_INSTALL
        @printf '    %b %b\n' $(LINKCOLOR)INSTALL$(ENDCOLOR) $(BINCOLOR)$(1)$(ENDCOLOR) 1>&2
        @$(INSTALL) $(1) $(2)
    endef
else
    define MAKE_INSTALL
        $(INSTALL) $(1) $(2)
    endef
endif

REDIS_CC=$(QUIET_CC)$(CC) $(FINAL_CFLAGS)
REDIS_LD=$(QUIET_LINK)$(CC) $(FINAL_LDFLAGS)
REDIS_INSTALL=$(QUIET_INSTALL)$(INSTALL)

CCCOLOR="\033[34m"
LINKCOLOR="\033[34;1m"
SRCCOLOR="\033[33m"
BINCOLOR="\033[37;1m"
MAKECOLOR="\033[32;1m"
ENDCOLOR="\033[0m"

ifndef V
QUIET_CC = @printf '    %b %b\n' $(CCCOLOR)CC$(ENDCOLOR) $(SRCCOLOR)$@$(ENDCOLOR) 1>&2;
QUIET_GEN = @printf '    %b %b\n' $(CCCOLOR)GEN$(ENDCOLOR) $(SRCCOLOR)$@$(ENDCOLOR) 1>&2;
QUIET_LINK = @printf '    %b %b\n' $(LINKCOLOR)LINK$(ENDCOLOR) $(BINCOLOR)$@$(ENDCOLOR) 1>&2;
QUIET_INSTALL = @printf '    %b %b\n' $(LINKCOLOR)INSTALL$(ENDCOLOR) $(BINCOLOR)$@$(ENDCOLOR) 1>&2;
endif

ifneq (, $(findstring LOG_REQ_RES, $(REDIS_CFLAGS)))
	COMMANDS_DEF_FILENAME=commands_with_reply_schema
	GEN_COMMANDS_FLAGS=--with-reply-schema
else
	COMMANDS_DEF_FILENAME=commands
	GEN_COMMANDS_FLAGS=
endif

REDIS_SERVER_NAME=redis-server$(PROG_SUFFIX)
REDIS_SENTINEL_NAME=redis-sentinel$(PROG_SUFFIX)
REDIS_SERVER_OBJ=adlist.o quicklist.o ae.o anet.o dict.o server.o sds.o zmalloc.o lzf_c.o lzf_d.o pqsort.o zipmap.o sha1.o ziplist.o release.o networking.o util.o object.o db.o replication.o rdb.o t_string.o t_list.o t_set.o t_zset.o t_hash.o config.o aof.o pubsub.o multi.o debug.o sort.o intset.o syncio.o cluster.o crc16.o endianconv.o slowlog.o eval.o bio.o rio.o rand.o memtest.o syscheck.o crcspeed.o crc64.o bitops.o sentinel.o notify.o setproctitle.o blocked.o hyperloglog.o latency.o sparkline.o redis-check-rdb.o redis-check-aof.o geo.o lazyfree.o module.o evict.o expire.o geohash.o geohash_helper.o childinfo.o defrag.o siphash.o rax.o t_stream.o listpack.o localtime.o lolwut.o lolwut5.o lolwut6.o acl.o tracking.o socket.o tls.o sha256.o timeout.o setcpuaffinity.o monotonic.o mt19937-64.o resp_parser.o call_reply.o script_lua.o script.o functions.o function_lua.o commands.o strl.o connection.o unix.o logreqres.o
REDIS_CLI_NAME=redis-cli$(PROG_SUFFIX)
REDIS_CLI_OBJ=anet.o adlist.o dict.o redis-cli.o zmalloc.o release.o ae.o redisassert.o crcspeed.o crc64.o siphash.o crc16.o monotonic.o cli_common.o mt19937-64.o strl.o cli_commands.o
REDIS_BENCHMARK_NAME=redis-benchmark$(PROG_SUFFIX)
REDIS_BENCHMARK_OBJ=ae.o anet.o redis-benchmark.o adlist.o dict.o zmalloc.o redisassert.o release.o crcspeed.o crc64.o siphash.o crc16.o monotonic.o cli_common.o mt19937-64.o strl.o
REDIS_CHECK_RDB_NAME=redis-check-rdb$(PROG_SUFFIX)
REDIS_CHECK_AOF_NAME=redis-check-aof$(PROG_SUFFIX)
ALL_SOURCES=$(sort $(patsubst %.o,%.c,$(REDIS_SERVER_OBJ) $(REDIS_CLI_OBJ) $(REDIS_BENCHMARK_OBJ)))

all: $(REDIS_SERVER_NAME) $(REDIS_SENTINEL_NAME) $(REDIS_CLI_NAME) $(REDIS_BENCHMARK_NAME) $(REDIS_CHECK_RDB_NAME) $(REDIS_CHECK_AOF_NAME) $(TLS_MODULE)
	@echo ""
	@echo "Hint: It's a good idea to run 'make test' ;)"
	@echo ""

Makefile.dep:
	-$(REDIS_CC) -MM $(ALL_SOURCES) > Makefile.dep 2> /dev/null || true

ifeq (0, $(words $(findstring $(MAKECMDGOALS), $(NODEPS))))
-include Makefile.dep
endif

.PHONY: all

persist-settings: distclean
	echo STD=$(STD) >> .make-settings
	echo WARN=$(WARN) >> .make-settings
	echo OPT=$(OPT) >> .make-settings
	echo MALLOC=$(MALLOC) >> .make-settings
	echo BUILD_TLS=$(BUILD_TLS) >> .make-settings
	echo USE_SYSTEMD=$(USE_SYSTEMD) >> .make-settings
	echo CFLAGS=$(CFLAGS) >> .make-settings
	echo LDFLAGS=$(LDFLAGS) >> .make-settings
	echo REDIS_CFLAGS=$(REDIS_CFLAGS) >> .make-settings
	echo REDIS_LDFLAGS=$(REDIS_LDFLAGS) >> .make-settings
	echo PREV_FINAL_CFLAGS=$(FINAL_CFLAGS) >> .make-settings
	echo PREV_FINAL_LDFLAGS=$(FINAL_LDFLAGS) >> .make-settings
	-(cd ../deps && $(MAKE) $(DEPENDENCY_TARGETS))

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
	$(REDIS_LD) -o $@ $^ ../deps/hiredis/libhiredis.a ../deps/lua/src/liblua.a ../deps/hdr_histogram/libhdrhistogram.a ../deps/fpconv/libfpconv.a $(FINAL_LIBS)

# redis-sentinel
$(REDIS_SENTINEL_NAME): $(REDIS_SERVER_NAME)
	$(REDIS_INSTALL) $(REDIS_SERVER_NAME) $(REDIS_SENTINEL_NAME)

# redis-check-rdb
$(REDIS_CHECK_RDB_NAME): $(REDIS_SERVER_NAME)
	$(REDIS_INSTALL) $(REDIS_SERVER_NAME) $(REDIS_CHECK_RDB_NAME)

# redis-check-aof
$(REDIS_CHECK_AOF_NAME): $(REDIS_SERVER_NAME)
	$(REDIS_INSTALL) $(REDIS_SERVER_NAME) $(REDIS_CHECK_AOF_NAME)

# redis-tls.so
$(TLS_MODULE_NAME): $(REDIS_SERVER_NAME)
	$(QUIET_CC)$(CC) -o $@ tls.c -shared -fPIC $(TLS_MODULE_CFLAGS) $(TLS_CLIENT_LIBS)

# redis-cli
$(REDIS_CLI_NAME): $(REDIS_CLI_OBJ)
	$(REDIS_LD) -o $@ $^ ../deps/hiredis/libhiredis.a ../deps/linenoise/linenoise.o $(FINAL_LIBS) $(TLS_CLIENT_LIBS)

# redis-benchmark
$(REDIS_BENCHMARK_NAME): $(REDIS_BENCHMARK_OBJ)
	$(REDIS_LD) -o $@ $^ ../deps/hiredis/libhiredis.a ../deps/hdr_histogram/libhdrhistogram.a $(FINAL_LIBS) $(TLS_CLIENT_LIBS)

DEP = $(REDIS_SERVER_OBJ:%.o=%.d) $(REDIS_CLI_OBJ:%.o=%.d) $(REDIS_BENCHMARK_OBJ:%.o=%.d)
-include $(DEP)

# Because the jemalloc.h header is generated as a part of the jemalloc build,
# building it should complete before building any other object. Instead of
# depending on a single artifact, build all dependencies first.
%.o: %.c .make-prerequisites
	$(REDIS_CC) -MMD -o $@ -c $<

# The file commands.def is checked in and doesn't normally need to be rebuilt. It
# is built only if python is available and its prereqs are modified.
ifneq (,$(PYTHON))
$(COMMANDS_DEF_FILENAME).def: commands/*.json ../utils/generate-command-code.py
	$(QUIET_GEN)$(PYTHON) ../utils/generate-command-code.py $(GEN_COMMANDS_FLAGS)
endif

commands.c: $(COMMANDS_DEF_FILENAME).def

clean:
	rm -rf $(REDIS_SERVER_NAME) $(REDIS_SENTINEL_NAME) $(REDIS_CLI_NAME) $(REDIS_BENCHMARK_NAME) $(REDIS_CHECK_RDB_NAME) $(REDIS_CHECK_AOF_NAME) *.o *.gcda *.gcno *.gcov redis.info lcov-html Makefile.dep *.so
	rm -f $(DEP)

.PHONY: clean

distclean: clean
	-(cd ../deps && $(MAKE) distclean)
	-(cd modules && $(MAKE) clean)
	-(cd ../tests/modules && $(MAKE) clean)
	-(rm -f .make-*)

.PHONY: distclean

test: $(REDIS_SERVER_NAME) $(REDIS_CHECK_AOF_NAME) $(REDIS_CLI_NAME) $(REDIS_BENCHMARK_NAME)
	@(cd ..; ./runtest)

test-modules: $(REDIS_SERVER_NAME)
	@(cd ..; ./runtest-moduleapi)

test-sentinel: $(REDIS_SENTINEL_NAME) $(REDIS_CLI_NAME)
	@(cd ..; ./runtest-sentinel)

test-cluster: $(REDIS_SERVER_NAME) $(REDIS_CLI_NAME)
	@(cd ..; ./runtest-cluster)

check: test

lcov:
	$(MAKE) gcov
	@(set -e; cd ..; ./runtest --clients 1)
	@geninfo -o redis.info .
	@genhtml --legend -o lcov-html redis.info

.PHONY: lcov

bench: $(REDIS_BENCHMARK_NAME)
	./$(REDIS_BENCHMARK_NAME)

32bit:
	@echo ""
	@echo "WARNING: if it fails under Linux you probably need to install libc6-dev-i386"
	@echo ""
	$(MAKE) CFLAGS="-m32" LDFLAGS="-m32"

gcov:
	$(MAKE) REDIS_CFLAGS="-fprofile-arcs -ftest-coverage -DCOVERAGE_TEST" REDIS_LDFLAGS="-fprofile-arcs -ftest-coverage"

noopt:
	$(MAKE) OPTIMIZATION="-O0"

valgrind:
	$(MAKE) OPTIMIZATION="-O0" MALLOC="libc"

helgrind:
	$(MAKE) OPTIMIZATION="-O0" MALLOC="libc" CFLAGS="-D__ATOMIC_VAR_FORCE_SYNC_MACROS" REDIS_CFLAGS="-I/usr/local/include" REDIS_LDFLAGS="-L/usr/local/lib"

install: all
	@mkdir -p $(INSTALL_BIN)
	$(call MAKE_INSTALL,$(REDIS_SERVER_NAME),$(INSTALL_BIN))
	$(call MAKE_INSTALL,$(REDIS_BENCHMARK_NAME),$(INSTALL_BIN))
	$(call MAKE_INSTALL,$(REDIS_CLI_NAME),$(INSTALL_BIN))
	@ln -sf $(REDIS_SERVER_NAME) $(INSTALL_BIN)/$(REDIS_CHECK_RDB_NAME)
	@ln -sf $(REDIS_SERVER_NAME) $(INSTALL_BIN)/$(REDIS_CHECK_AOF_NAME)
	@ln -sf $(REDIS_SERVER_NAME) $(INSTALL_BIN)/$(REDIS_SENTINEL_NAME)

uninstall:
	rm -f $(INSTALL_BIN)/{$(REDIS_SERVER_NAME),$(REDIS_BENCHMARK_NAME),$(REDIS_CLI_NAME),$(REDIS_CHECK_RDB_NAME),$(REDIS_CHECK_AOF_NAME),$(REDIS_SENTINEL_NAME)}
