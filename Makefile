# Redis Makefile
# Copyright (C) 2009 Salvatore Sanfilippo <antirez at gmail dot com>
# This file is released under the BSD license, see the COPYING file

DEBUG?= -g
CFLAGS?= -std=c99 -pedantic -O -Wall -W -DSDS_ABORT_ON_OOM
CCOPT= $(CFLAGS)

OBJ = adlist.o ae.o anet.o dict.o redis.o sds.o zmalloc.o lzf_c.o lzf_d.o pqsort.o
BENCHOBJ = ae.o anet.o benchmark.o sds.o adlist.o zmalloc.o
CLIOBJ = anet.o sds.o adlist.o redis-cli.o zmalloc.o

PRGNAME = redis-server
BENCHPRGNAME = redis-benchmark
CLIPRGNAME = redis-cli

all: redis-server redis-benchmark redis-cli

# Deps (use make dep to generate this)
adlist.o: adlist.c adlist.h
ae.o: ae.c ae.h
anet.o: anet.c anet.h
benchmark.o: benchmark.c ae.h anet.h sds.h adlist.h
dict.o: dict.c dict.h
redis-cli.o: redis-cli.c anet.h sds.h adlist.h
redis.o: redis.c ae.h sds.h anet.h dict.h adlist.h zmalloc.c zmalloc.h
sds.o: sds.c sds.h
sha1.o: sha1.c sha1.h
zmalloc.o: zmalloc.c

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
