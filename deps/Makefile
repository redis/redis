# Redis dependency Makefile

uname_S:= $(shell sh -c 'uname -s 2>/dev/null || echo not')

LUA_DEBUG?=no
LUA_COVERAGE?=no

CCCOLOR="\033[34m"
LINKCOLOR="\033[34;1m"
SRCCOLOR="\033[33m"
BINCOLOR="\033[37;1m"
MAKECOLOR="\033[32;1m"
ENDCOLOR="\033[0m"

default:
	@echo "Explicit target required"

.PHONY: default

# Prerequisites target
.make-prerequisites:
	@touch $@

# Clean everything when CFLAGS is different
ifneq ($(shell sh -c '[ -f .make-cflags ] && cat .make-cflags || echo none'), $(CFLAGS))
.make-cflags: distclean
	-(echo "$(CFLAGS)" > .make-cflags)
.make-prerequisites: .make-cflags
endif

# Clean everything when LDFLAGS is different
ifneq ($(shell sh -c '[ -f .make-ldflags ] && cat .make-ldflags || echo none'), $(LDFLAGS))
.make-ldflags: distclean
	-(echo "$(LDFLAGS)" > .make-ldflags)
.make-prerequisites: .make-ldflags
endif

distclean:
	-(cd hiredis && $(MAKE) clean) > /dev/null || true
	-(cd linenoise && $(MAKE) clean) > /dev/null || true
	-(cd lua && $(MAKE) clean) > /dev/null || true
	-(cd jemalloc && [ -f Makefile ] && $(MAKE) distclean) > /dev/null || true
	-(cd hdr_histogram && $(MAKE) clean) > /dev/null || true
	-(cd fpconv && $(MAKE) clean) > /dev/null || true
	-(rm -f .make-*)

.PHONY: distclean

ifneq (,$(filter $(BUILD_TLS),yes module))
    HIREDIS_MAKE_FLAGS = USE_SSL=1
endif

hiredis: .make-prerequisites
	@printf '%b %b\n' $(MAKECOLOR)MAKE$(ENDCOLOR) $(BINCOLOR)$@$(ENDCOLOR)
	cd hiredis && $(MAKE) static $(HIREDIS_MAKE_FLAGS)

.PHONY: hiredis

linenoise: .make-prerequisites
	@printf '%b %b\n' $(MAKECOLOR)MAKE$(ENDCOLOR) $(BINCOLOR)$@$(ENDCOLOR)
	cd linenoise && $(MAKE)

.PHONY: linenoise

hdr_histogram: .make-prerequisites
	@printf '%b %b\n' $(MAKECOLOR)MAKE$(ENDCOLOR) $(BINCOLOR)$@$(ENDCOLOR)
	cd hdr_histogram && $(MAKE)

.PHONY: hdr_histogram

fpconv: .make-prerequisites
	@printf '%b %b\n' $(MAKECOLOR)MAKE$(ENDCOLOR) $(BINCOLOR)$@$(ENDCOLOR)
	cd fpconv && $(MAKE)

.PHONY: fpconv

ifeq ($(uname_S),SunOS)
	# Make isinf() available
	LUA_CFLAGS= -D__C99FEATURES__=1
endif

LUA_CFLAGS+= -Wall -DLUA_ANSI -DENABLE_CJSON_GLOBAL -DREDIS_STATIC='' -DLUA_USE_MKSTEMP $(CFLAGS)
LUA_LDFLAGS+= $(LDFLAGS)
ifeq ($(LUA_DEBUG),yes)
	LUA_CFLAGS+= -O0 -g -DLUA_USE_APICHECK
else
	LUA_CFLAGS+= -O2 
endif
ifeq ($(LUA_COVERAGE),yes)
	LUA_CFLAGS += -fprofile-arcs -ftest-coverage
	LUA_LDFLAGS += -fprofile-arcs -ftest-coverage
endif

# lua's Makefile defines AR="ar rcu", which is unusual, and makes it more
# challenging to cross-compile lua (and redis).  These defines make it easier
# to fit redis into cross-compilation environments, which typically set AR.
AR=ar
ARFLAGS=rc

lua: .make-prerequisites
	@printf '%b %b\n' $(MAKECOLOR)MAKE$(ENDCOLOR) $(BINCOLOR)$@$(ENDCOLOR)
	cd lua/src && $(MAKE) all CFLAGS="$(LUA_CFLAGS)" MYLDFLAGS="$(LUA_LDFLAGS)" AR="$(AR) $(ARFLAGS)"

.PHONY: lua

JEMALLOC_CFLAGS=$(CFLAGS)
JEMALLOC_LDFLAGS=$(LDFLAGS)

ifneq ($(DEB_HOST_GNU_TYPE),)
JEMALLOC_CONFIGURE_OPTS += --host=$(DEB_HOST_GNU_TYPE)
endif

jemalloc: .make-prerequisites
	@printf '%b %b\n' $(MAKECOLOR)MAKE$(ENDCOLOR) $(BINCOLOR)$@$(ENDCOLOR)
	cd jemalloc && ./configure --disable-cxx --with-version=5.3.0-0-g0 --with-lg-quantum=3 --disable-cache-oblivious --with-jemalloc-prefix=je_ CFLAGS="$(JEMALLOC_CFLAGS)" LDFLAGS="$(JEMALLOC_LDFLAGS)" $(JEMALLOC_CONFIGURE_OPTS)
	cd jemalloc && $(MAKE) lib/libjemalloc.a

.PHONY: jemalloc
