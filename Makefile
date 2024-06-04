# Top-level Makefile
# The main Makefile is located in the src directory

default: all

.DEFAULT:
	cd src && $(MAKE) $@

install:
	cd src && $(MAKE) $@

.PHONY: install
