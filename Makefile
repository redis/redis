# Top level makefile, the real shit is at src/Makefile

default: all

.DEFAULT:
	cd src && $(MAKE) $@

deps:
	cd deps && $(MAKE) $@

install: deps
	cd src && $(MAKE) $@

.PHONY: install
