# Hiredis Makefile
# Copyright (C) 2010-2011 Salvatore Sanfilippo <antirez at gmail dot com>
# Copyright (C) 2010-2011 Pieter Noordhuis <pcnoordhuis at gmail dot com>
# This file is released under the BSD license, see the COPYING file

OBJ=alloc.o net.o hiredis.o sds.o async.o read.o sockcompat.o
SSL_OBJ=ssl.o
EXAMPLES=hiredis-example hiredis-example-libevent hiredis-example-libev hiredis-example-glib hiredis-example-push
ifeq ($(USE_SSL),1)
EXAMPLES+=hiredis-example-ssl hiredis-example-libevent-ssl
endif
TESTS=hiredis-test
LIBNAME=libhiredis
PKGCONFNAME=hiredis.pc
SSL_LIBNAME=libhiredis_ssl
SSL_PKGCONFNAME=hiredis_ssl.pc

HIREDIS_MAJOR=$(shell grep HIREDIS_MAJOR hiredis.h | awk '{print $$3}')
HIREDIS_MINOR=$(shell grep HIREDIS_MINOR hiredis.h | awk '{print $$3}')
HIREDIS_PATCH=$(shell grep HIREDIS_PATCH hiredis.h | awk '{print $$3}')
HIREDIS_SONAME=$(shell grep HIREDIS_SONAME hiredis.h | awk '{print $$3}')

# Installation related variables and target
PREFIX?=/usr/local
INCLUDE_PATH?=include/hiredis
LIBRARY_PATH?=lib
PKGCONF_PATH?=pkgconfig
INSTALL_INCLUDE_PATH= $(DESTDIR)$(PREFIX)/$(INCLUDE_PATH)
INSTALL_LIBRARY_PATH= $(DESTDIR)$(PREFIX)/$(LIBRARY_PATH)
INSTALL_PKGCONF_PATH= $(INSTALL_LIBRARY_PATH)/$(PKGCONF_PATH)

# redis-server configuration used for testing
REDIS_PORT=56379
REDIS_SERVER=redis-server
define REDIS_TEST_CONFIG
	daemonize yes
	pidfile /tmp/hiredis-test-redis.pid
	port $(REDIS_PORT)
	bind 127.0.0.1
	unixsocket /tmp/hiredis-test-redis.sock
endef
export REDIS_TEST_CONFIG

# Fallback to gcc when $CC is not in $PATH.
CC:=$(shell sh -c 'type $${CC%% *} >/dev/null 2>/dev/null && echo $(CC) || echo gcc')
CXX:=$(shell sh -c 'type $${CXX%% *} >/dev/null 2>/dev/null && echo $(CXX) || echo g++')
OPTIMIZATION?=-O3
WARNINGS=-Wall -W -Wstrict-prototypes -Wwrite-strings -Wno-missing-field-initializers
DEBUG_FLAGS?= -g -ggdb
REAL_CFLAGS=$(OPTIMIZATION) -fPIC $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(DEBUG_FLAGS)
REAL_LDFLAGS=$(LDFLAGS)

DYLIBSUFFIX=so
STLIBSUFFIX=a
DYLIB_MINOR_NAME=$(LIBNAME).$(DYLIBSUFFIX).$(HIREDIS_SONAME)
DYLIB_MAJOR_NAME=$(LIBNAME).$(DYLIBSUFFIX).$(HIREDIS_MAJOR)
DYLIBNAME=$(LIBNAME).$(DYLIBSUFFIX)

DYLIB_MAKE_CMD=$(CC) -shared -Wl,-soname,$(DYLIB_MINOR_NAME)
STLIBNAME=$(LIBNAME).$(STLIBSUFFIX)
STLIB_MAKE_CMD=$(AR) rcs

SSL_DYLIB_MINOR_NAME=$(SSL_LIBNAME).$(DYLIBSUFFIX).$(HIREDIS_SONAME)
SSL_DYLIB_MAJOR_NAME=$(SSL_LIBNAME).$(DYLIBSUFFIX).$(HIREDIS_MAJOR)
SSL_DYLIBNAME=$(SSL_LIBNAME).$(DYLIBSUFFIX)
SSL_STLIBNAME=$(SSL_LIBNAME).$(STLIBSUFFIX)
SSL_DYLIB_MAKE_CMD=$(CC) -shared -Wl,-soname,$(SSL_DYLIB_MINOR_NAME)

# Platform-specific overrides
uname_S := $(shell sh -c 'uname -s 2>/dev/null || echo not')

USE_SSL?=0

# This is required for test.c only
ifeq ($(USE_SSL),1)
  CFLAGS+=-DHIREDIS_TEST_SSL
endif

ifeq ($(uname_S),Linux)
  ifdef OPENSSL_PREFIX
    CFLAGS+=-I$(OPENSSL_PREFIX)/include
    SSL_LDFLAGS+=-L$(OPENSSL_PREFIX)/lib -lssl -lcrypto
  else
    SSL_LDFLAGS=-lssl -lcrypto
  endif
else
  OPENSSL_PREFIX?=/usr/local/opt/openssl
  CFLAGS+=-I$(OPENSSL_PREFIX)/include
  SSL_LDFLAGS+=-L$(OPENSSL_PREFIX)/lib -lssl -lcrypto
endif

ifeq ($(uname_S),SunOS)
  IS_SUN_CC=$(shell sh -c '$(CC) -V 2>&1 |egrep -i -c "sun|studio"')
  ifeq ($(IS_SUN_CC),1)
    SUN_SHARED_FLAG=-G
  else
    SUN_SHARED_FLAG=-shared
  endif
  REAL_LDFLAGS+= -ldl -lnsl -lsocket
  DYLIB_MAKE_CMD=$(CC) $(SUN_SHARED_FLAG) -o $(DYLIBNAME) -h $(DYLIB_MINOR_NAME) $(LDFLAGS)
  SSL_DYLIB_MAKE_CMD=$(CC) $(SUN_SHARED_FLAG) -o $(SSL_DYLIBNAME) -h $(SSL_DYLIB_MINOR_NAME) $(LDFLAGS) $(SSL_LDFLAGS)
endif
ifeq ($(uname_S),Darwin)
  DYLIBSUFFIX=dylib
  DYLIB_MINOR_NAME=$(LIBNAME).$(HIREDIS_SONAME).$(DYLIBSUFFIX)
  DYLIB_MAKE_CMD=$(CC) -dynamiclib -Wl,-install_name,$(PREFIX)/$(LIBRARY_PATH)/$(DYLIB_MINOR_NAME) -o $(DYLIBNAME) $(LDFLAGS)
  SSL_DYLIB_MAKE_CMD=$(CC) -dynamiclib -Wl,-install_name,$(PREFIX)/$(LIBRARY_PATH)/$(SSL_DYLIB_MINOR_NAME) -o $(SSL_DYLIBNAME) $(LDFLAGS) $(SSL_LDFLAGS)
  DYLIB_PLUGIN=-Wl,-undefined -Wl,dynamic_lookup
endif

all: $(DYLIBNAME) $(STLIBNAME) hiredis-test $(PKGCONFNAME)
ifeq ($(USE_SSL),1)
all: $(SSL_DYLIBNAME) $(SSL_STLIBNAME) $(SSL_PKGCONFNAME)
endif

# Deps (use make dep to generate this)
alloc.o: alloc.c fmacros.h alloc.h
async.o: async.c fmacros.h alloc.h async.h hiredis.h read.h sds.h net.h dict.c dict.h win32.h async_private.h
dict.o: dict.c fmacros.h alloc.h dict.h
hiredis.o: hiredis.c fmacros.h hiredis.h read.h sds.h alloc.h net.h async.h win32.h
net.o: net.c fmacros.h net.h hiredis.h read.h sds.h alloc.h sockcompat.h win32.h
read.o: read.c fmacros.h alloc.h read.h sds.h win32.h
sds.o: sds.c sds.h sdsalloc.h alloc.h
sockcompat.o: sockcompat.c sockcompat.h
ssl.o: ssl.c hiredis.h read.h sds.h alloc.h async.h win32.h async_private.h
test.o: test.c fmacros.h hiredis.h read.h sds.h alloc.h net.h sockcompat.h win32.h

$(DYLIBNAME): $(OBJ)
	$(DYLIB_MAKE_CMD) -o $(DYLIBNAME) $(OBJ) $(REAL_LDFLAGS)

$(STLIBNAME): $(OBJ)
	$(STLIB_MAKE_CMD) $(STLIBNAME) $(OBJ)

$(SSL_DYLIBNAME): $(SSL_OBJ)
	$(SSL_DYLIB_MAKE_CMD) $(DYLIB_PLUGIN) -o $(SSL_DYLIBNAME) $(SSL_OBJ) $(REAL_LDFLAGS) $(LDFLAGS) $(SSL_LDFLAGS)

$(SSL_STLIBNAME): $(SSL_OBJ)
	$(STLIB_MAKE_CMD) $(SSL_STLIBNAME) $(SSL_OBJ)

dynamic: $(DYLIBNAME)
static: $(STLIBNAME)
ifeq ($(USE_SSL),1)
dynamic: $(SSL_DYLIBNAME)
static: $(SSL_STLIBNAME)
endif

# Binaries:
hiredis-example-libevent: examples/example-libevent.c adapters/libevent.h $(STLIBNAME)
	$(CC) -o examples/$@ $(REAL_CFLAGS) -I. $< -levent $(STLIBNAME) $(REAL_LDFLAGS)

hiredis-example-libevent-ssl: examples/example-libevent-ssl.c adapters/libevent.h $(STLIBNAME) $(SSL_STLIBNAME)
	$(CC) -o examples/$@ $(REAL_CFLAGS) -I. $< -levent $(STLIBNAME) $(SSL_STLIBNAME) $(REAL_LDFLAGS) $(SSL_LDFLAGS)

hiredis-example-libev: examples/example-libev.c adapters/libev.h $(STLIBNAME)
	$(CC) -o examples/$@ $(REAL_CFLAGS) -I. $< -lev $(STLIBNAME) $(REAL_LDFLAGS)

hiredis-example-glib: examples/example-glib.c adapters/glib.h $(STLIBNAME)
	$(CC) -o examples/$@ $(REAL_CFLAGS) -I. $< $(shell pkg-config --cflags --libs glib-2.0) $(STLIBNAME) $(REAL_LDFLAGS)

hiredis-example-ivykis: examples/example-ivykis.c adapters/ivykis.h $(STLIBNAME)
	$(CC) -o examples/$@ $(REAL_CFLAGS) -I. $< -livykis $(STLIBNAME) $(REAL_LDFLAGS)

hiredis-example-macosx: examples/example-macosx.c adapters/macosx.h $(STLIBNAME)
	$(CC) -o examples/$@ $(REAL_CFLAGS) -I. $< -framework CoreFoundation $(STLIBNAME) $(REAL_LDFLAGS)

hiredis-example-ssl: examples/example-ssl.c $(STLIBNAME) $(SSL_STLIBNAME)
	$(CC) -o examples/$@ $(REAL_CFLAGS) -I. $< $(STLIBNAME) $(SSL_STLIBNAME) $(REAL_LDFLAGS) $(SSL_LDFLAGS)


ifndef AE_DIR
hiredis-example-ae:
	@echo "Please specify AE_DIR (e.g. <redis repository>/src)"
	@false
else
hiredis-example-ae: examples/example-ae.c adapters/ae.h $(STLIBNAME)
	$(CC) -o examples/$@ $(REAL_CFLAGS) $(REAL_LDFLAGS) -I. -I$(AE_DIR) $< $(AE_DIR)/ae.o $(AE_DIR)/zmalloc.o $(AE_DIR)/../deps/jemalloc/lib/libjemalloc.a -pthread $(STLIBNAME)
endif

ifndef LIBUV_DIR
hiredis-example-libuv:
	@echo "Please specify LIBUV_DIR (e.g. ../libuv/)"
	@false
else
hiredis-example-libuv: examples/example-libuv.c adapters/libuv.h $(STLIBNAME)
	$(CC) -o examples/$@ $(REAL_CFLAGS) -I. -I$(LIBUV_DIR)/include $< $(LIBUV_DIR)/.libs/libuv.a -lpthread -lrt $(STLIBNAME) $(REAL_LDFLAGS)
endif

ifeq ($(and $(QT_MOC),$(QT_INCLUDE_DIR),$(QT_LIBRARY_DIR)),)
hiredis-example-qt:
	@echo "Please specify QT_MOC, QT_INCLUDE_DIR AND QT_LIBRARY_DIR"
	@false
else
hiredis-example-qt: examples/example-qt.cpp adapters/qt.h $(STLIBNAME)
	$(QT_MOC) adapters/qt.h -I. -I$(QT_INCLUDE_DIR) -I$(QT_INCLUDE_DIR)/QtCore | \
	    $(CXX) -x c++ -o qt-adapter-moc.o -c - $(REAL_CFLAGS) -I. -I$(QT_INCLUDE_DIR) -I$(QT_INCLUDE_DIR)/QtCore
	$(QT_MOC) examples/example-qt.h -I. -I$(QT_INCLUDE_DIR) -I$(QT_INCLUDE_DIR)/QtCore | \
	    $(CXX) -x c++ -o qt-example-moc.o -c - $(REAL_CFLAGS) -I. -I$(QT_INCLUDE_DIR) -I$(QT_INCLUDE_DIR)/QtCore
	$(CXX) -o examples/$@ $(REAL_CFLAGS) $(REAL_LDFLAGS) -I. -I$(QT_INCLUDE_DIR) -I$(QT_INCLUDE_DIR)/QtCore -L$(QT_LIBRARY_DIR) qt-adapter-moc.o qt-example-moc.o $< -pthread $(STLIBNAME) -lQtCore
endif

hiredis-example: examples/example.c $(STLIBNAME)
	$(CC) -o examples/$@ $(REAL_CFLAGS) -I. $< $(STLIBNAME) $(REAL_LDFLAGS)

hiredis-example-push: examples/example-push.c $(STLIBNAME)
	$(CC) -o examples/$@ $(REAL_CFLAGS) -I. $< $(STLIBNAME) $(REAL_LDFLAGS)

examples: $(EXAMPLES)

TEST_LIBS = $(STLIBNAME)
ifeq ($(USE_SSL),1)
    TEST_LIBS += $(SSL_STLIBNAME)
    TEST_LDFLAGS = $(SSL_LDFLAGS) -lssl -lcrypto -lpthread
endif

hiredis-test: test.o $(TEST_LIBS)
	$(CC) -o $@ $(REAL_CFLAGS) -I. $^ $(REAL_LDFLAGS) $(TEST_LDFLAGS)

hiredis-%: %.o $(STLIBNAME)
	$(CC) $(REAL_CFLAGS) -o $@ $< $(TEST_LIBS) $(REAL_LDFLAGS)

test: hiredis-test
	./hiredis-test

check: hiredis-test
	TEST_SSL=$(USE_SSL) ./test.sh

.c.o:
	$(CC) -std=c99 -pedantic -c $(REAL_CFLAGS) $<

clean:
	rm -rf $(DYLIBNAME) $(STLIBNAME) $(SSL_DYLIBNAME) $(SSL_STLIBNAME) $(TESTS) $(PKGCONFNAME) examples/hiredis-example* *.o *.gcda *.gcno *.gcov

dep:
	$(CC) $(CPPFLAGS) $(CFLAGS) -MM *.c

INSTALL?= cp -pPR

$(PKGCONFNAME): hiredis.h
	@echo "Generating $@ for pkgconfig..."
	@echo prefix=$(PREFIX) > $@
	@echo exec_prefix=\$${prefix} >> $@
	@echo libdir=$(PREFIX)/$(LIBRARY_PATH) >> $@
	@echo includedir=$(PREFIX)/$(INCLUDE_PATH) >> $@
	@echo >> $@
	@echo Name: hiredis >> $@
	@echo Description: Minimalistic C client library for Redis. >> $@
	@echo Version: $(HIREDIS_MAJOR).$(HIREDIS_MINOR).$(HIREDIS_PATCH) >> $@
	@echo Libs: -L\$${libdir} -lhiredis >> $@
	@echo Cflags: -I\$${includedir} -D_FILE_OFFSET_BITS=64 >> $@

$(SSL_PKGCONFNAME): hiredis_ssl.h
	@echo "Generating $@ for pkgconfig..."
	@echo prefix=$(PREFIX) > $@
	@echo exec_prefix=\$${prefix} >> $@
	@echo libdir=$(PREFIX)/$(LIBRARY_PATH) >> $@
	@echo includedir=$(PREFIX)/$(INCLUDE_PATH) >> $@
	@echo >> $@
	@echo Name: hiredis_ssl >> $@
	@echo Description: SSL Support for hiredis. >> $@
	@echo Version: $(HIREDIS_MAJOR).$(HIREDIS_MINOR).$(HIREDIS_PATCH) >> $@
	@echo Requires: hiredis >> $@
	@echo Libs: -L\$${libdir} -lhiredis_ssl >> $@
	@echo Libs.private: -lssl -lcrypto >> $@

install: $(DYLIBNAME) $(STLIBNAME) $(PKGCONFNAME)
	mkdir -p $(INSTALL_INCLUDE_PATH) $(INSTALL_INCLUDE_PATH)/adapters $(INSTALL_LIBRARY_PATH)
	$(INSTALL) hiredis.h async.h read.h sds.h alloc.h $(INSTALL_INCLUDE_PATH)
	$(INSTALL) adapters/*.h $(INSTALL_INCLUDE_PATH)/adapters
	$(INSTALL) $(DYLIBNAME) $(INSTALL_LIBRARY_PATH)/$(DYLIB_MINOR_NAME)
	cd $(INSTALL_LIBRARY_PATH) && ln -sf $(DYLIB_MINOR_NAME) $(DYLIBNAME)
	$(INSTALL) $(STLIBNAME) $(INSTALL_LIBRARY_PATH)
	mkdir -p $(INSTALL_PKGCONF_PATH)
	$(INSTALL) $(PKGCONFNAME) $(INSTALL_PKGCONF_PATH)

ifeq ($(USE_SSL),1)
install: install-ssl

install-ssl: $(SSL_DYLIBNAME) $(SSL_STLIBNAME) $(SSL_PKGCONFNAME)
	mkdir -p $(INSTALL_INCLUDE_PATH) $(INSTALL_LIBRARY_PATH)
	$(INSTALL) hiredis_ssl.h $(INSTALL_INCLUDE_PATH)
	$(INSTALL) $(SSL_DYLIBNAME) $(INSTALL_LIBRARY_PATH)/$(SSL_DYLIB_MINOR_NAME)
	cd $(INSTALL_LIBRARY_PATH) && ln -sf $(SSL_DYLIB_MINOR_NAME) $(SSL_DYLIBNAME)
	$(INSTALL) $(SSL_STLIBNAME) $(INSTALL_LIBRARY_PATH)
	mkdir -p $(INSTALL_PKGCONF_PATH)
	$(INSTALL) $(SSL_PKGCONFNAME) $(INSTALL_PKGCONF_PATH)
endif

32bit:
	@echo ""
	@echo "WARNING: if this fails under Linux you probably need to install libc6-dev-i386"
	@echo ""
	$(MAKE) CFLAGS="-m32" LDFLAGS="-m32"

32bit-vars:
	$(eval CFLAGS=-m32)
	$(eval LDFLAGS=-m32)

gprof:
	$(MAKE) CFLAGS="-pg" LDFLAGS="-pg"

gcov:
	$(MAKE) CFLAGS="-fprofile-arcs -ftest-coverage" LDFLAGS="-fprofile-arcs"

coverage: gcov
	make check
	mkdir -p tmp/lcov
	lcov -d . -c -o tmp/lcov/hiredis.info
	genhtml --legend -o tmp/lcov/report tmp/lcov/hiredis.info

noopt:
	$(MAKE) OPTIMIZATION=""

.PHONY: all test check clean dep install 32bit 32bit-vars gprof gcov noopt
