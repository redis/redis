# Top level makefile, the real shit is at src/Makefile
JEMALLOC_PATH=../deps/memkind
export JEMALLOC_PATH

default: all

.DEFAULT:
	cd src && $(MAKE) $@

install:
	cd src && $(MAKE) $@

.PHONY: install
