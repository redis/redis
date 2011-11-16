# Redis dependency Makefile

UNAME_S:=$(shell sh -c 'uname -s 2> /dev/null || echo not')

LUA_CFLAGS=-O2 -Wall $(ARCH)
ifeq ($(UNAME_S),SunOS)
  # Make isinf() available
  LUA_CFLAGS+= -D__C99FEATURES__=1
endif

JEMALLOC_CFLAGS=
ifeq ($(ARCH),-m32)
  JEMALLOC_CFLAGS+=CFLAGS="-std=gnu99 -Wall -pipe -g3 -fvisibility=hidden -O3 -funroll-loops -m32"
endif

CCCOLOR="\033[34m"
LINKCOLOR="\033[34;1m"
SRCCOLOR="\033[33m"
BINCOLOR="\033[37;1m"
MAKECOLOR="\033[32;1m"
ENDCOLOR="\033[0m"

default:
	@echo "Explicit target required"

# Clean everything when ARCH is different
ifneq ($(shell sh -c '[ -f .make-arch ] && cat .make-arch'), $(ARCH))
.make-arch: distclean
else
.make-arch:
endif

.make-arch:
	-(echo $(ARCH) > .make-arch)

distclean:
	-(cd hiredis && $(MAKE) clean) > /dev/null || true
	-(cd linenoise && $(MAKE) clean) > /dev/null || true
	-(cd lua && $(MAKE) clean) > /dev/null || true
	-(cd jemalloc && [ -f Makefile ] && $(MAKE) distclean) > /dev/null || true
	-(rm -f .make-arch)

hiredis: .make-arch
	@printf '%b %b\n' $(MAKECOLOR)MAKE$(ENDCOLOR) $(BINCOLOR)hiredis$(ENDCOLOR)
	cd hiredis && $(MAKE) static ARCH="$(ARCH)"

linenoise: .make-arch
	@printf '%b %b\n' $(MAKECOLOR)MAKE$(ENDCOLOR) $(BINCOLOR)linenoise$(ENDCOLOR)
	cd linenoise && $(MAKE) ARCH="$(ARCH)"

lua: .make-arch
	@printf '%b %b\n' $(MAKECOLOR)MAKE$(ENDCOLOR) $(BINCOLOR)lua$(ENDCOLOR)
	cd lua && $(MAKE) CFLAGS="$(LUA_CFLAGS)" MYLDFLAGS="$(ARCH)" ansi

jemalloc: .make-arch
	@printf '%b %b\n' $(MAKECOLOR)MAKE$(ENDCOLOR) $(BINCOLOR)jemalloc$(ENDCOLOR)
	cd jemalloc && ./configure $(JEMALLOC_CFLAGS) --with-jemalloc-prefix=je_ --enable-cc-silence && $(MAKE) lib/libjemalloc.a

.PHONY: default conditional_clean hiredis linenoise lua jemalloc
