# Top level makefile, the real stuff is at ./src/Makefile and in ./modules/Makefile

SUBDIRS = src
ifeq ($(BUILD_WITH_MODULES), yes)
	ifeq ($(MAKECMDGOALS),32bit)
    	$(error BUILD_WITH_MODULES=yes is not supported on 32 bit systems)
	endif
	SUBDIRS += modules
endif

default: all

.DEFAULT:
	for dir in $(SUBDIRS); do $(MAKE) -C $$dir $@; done

install:
	for dir in $(SUBDIRS); do $(MAKE) -C $$dir $@; done

.PHONY: install
