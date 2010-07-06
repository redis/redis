# Top level makefile, the real shit is at src/Makefile

TARGETS=32bit noopt

all:
	cd src && $(MAKE) $@

$(TARGETS) clean:
	cd src && $(MAKE) $@
