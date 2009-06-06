# Redis Makefile
# Copyright (C) 2009 Salvatore Sanfilippo <antirez at gmail dot com>
# This file is released under the BSD license, see the COPYING file

DEBUG?= -g -rdynamic -ggdb 
CFLAGS?= -std=c99 -pedantic -O0 -Wall -W -DSDS_ABORT_ON_OOM
CCOPT= $(CFLAGS)

OBJ = adlist.o ae.o anet.o dict.o redis.o sds.o zmalloc.o lzf_c.o lzf_d.o pqsort.o
BENCHOBJ = ae.o anet.o benchmark.o sds.o adlist.o zmalloc.o
CLIOBJ = anet.o sds.o adlist.o redis-cli.o zmalloc.o

PRGNAME = redis-server
BENCHPRGNAME = redis-benchmark
CLIPRGNAME = redis-cli

all: redis-server redis-benchmark redis-cli

# Deps (use make dep to generate this)
adlist.o: adlist.c adlist.h zmalloc.h
ae.o: ae.c ae.h zmalloc.h
anet.o: anet.c fmacros.h anet.h
benchmark.o: benchmark.c fmacros.h ae.h anet.h sds.h adlist.h zmalloc.h
dict.o: dict.c fmacros.h dict.h zmalloc.h
lzf_c.o: lzf_c.c lzfP.h
lzf_d.o: lzf_d.c lzfP.h
pqsort.o: pqsort.c
redis-cli.o: redis-cli.c fmacros.h anet.h sds.h adlist.h zmalloc.h
redis.o: redis.c fmacros.h ae.h sds.h anet.h dict.h adlist.h zmalloc.h lzf.h pqsort.h config.h
sds.o: sds.c sds.h zmalloc.h
zmalloc.o: zmalloc.c config.h

redis-server: $(OBJ)
	$(CC) -o $(PRGNAME) $(CCOPT) $(DEBUG) $(OBJ)
	@echo ""
	@echo "Hint: To run the test-redis.tcl script is a good idea."
	@echo "Launch the redis server with ./redis-server, then in another"
	@echo "terminal window enter this directory and run 'make test'."
	@echo ""

redis-benchmark: $(BENCHOBJ)
	$(CC) -o $(BENCHPRGNAME) $(CCOPT) $(DEBUG) $(BENCHOBJ)

redis-cli: $(CLIOBJ)
	$(CC) -o $(CLIPRGNAME) $(CCOPT) $(DEBUG) $(CLIOBJ)

.c.o:
	$(CC) -c $(CCOPT) $(DEBUG) $(COMPILE_TIME) $<

clean:
	rm -rf $(PRGNAME) $(BENCHPRGNAME) $(CLIPRGNAME) *.o

dep:
	$(CC) -MM *.c

test:
	tclsh test-redis.tcl

bench:
	./redis-benchmark

log:
	git log '--pretty=format:%ad %s' --date=short > Changelog
