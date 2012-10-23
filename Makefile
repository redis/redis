# Top level makefile, the real shit is at src/Makefile

uname_S := $(shell sh -c 'uname -s 2>/dev/null || echo not')

default: all

.DEFAULT:
		gcc -o get_glibc_version ./utils/get_glibc_version.c
		./get_glibc_version > src/config_def.h
		cd src && $(MAKE) $@

install:
		cd src && $(MAKE) $@

clean:
		@rm -f ./get_glibc_version

.PHONY: install

