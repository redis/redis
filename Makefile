# Top level makefile, the real shit is at src/Makefile

default: all

.DEFAULT:
		cd src && $(MAKE) $@

install:
		cd src && $(MAKE) $@

clean:
		@rm -f ./get_glibc_version

.PHONY: install

