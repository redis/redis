# Top level makefile, the real shit is at src/Makefile

TARGETS=32bit noopt test install

all:
	cd src && $(MAKE) $@

$(TARGETS) clean:
	cd src && $(MAKE) $@
