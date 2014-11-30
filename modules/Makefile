# Redis Module Makefile
# Copyright (C) 2009 Nickey Woo <nickeywoo at gmail dot com>
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

FINAL_CFLAGS+= -I../deps/hiredis -I../deps/linenoise -I../deps/lua/src
REDIS_CC=$(CC) $(FINAL_CFLAGS)

REDIS_MODULE=libsystem_module.so

all: $(REDIS_MODULE)
	@echo "build success"

clean:
	rm -f *.o *.so

.PHONY: all

# redis-module
$(REDIS_MODULE): module_system.c
	$(REDIS_CC) -shared -fPIC -o $@ $^ -I../src/




