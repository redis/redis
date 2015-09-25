
uname_S:= $(shell sh -c 'uname -s 2>/dev/null || echo not')
STD=
WARN= -Wall
OPT= -O2

ifeq ($(uname_S),AIX)
  DEBUG=-qdbxextra -qtbtable=full
  R_CFLAGS=-qcheck=all -qpic=large -qlanglvl=extc99 $(DEBUG) $(CFLAGS)  
else
  R_CFLAGS= $(STD) $(WARN) $(OPT) $(DEBUG) $(CFLAGS)
  DEBUG= -g
endif


R_LDFLAGS= $(LDFLAGS)
R_CC=$(CC) $(R_CFLAGS)
R_LD=$(CC) $(R_LDFLAGS)

all: geohash.o geohash_helper.o

.PHONY: all

geohash.o: geohash.h geohash.c
geohash_helper.o: geohash.h geohash_helper.h geohash_helper.c

.c.o:
	$(R_CC) -c $<

clean:
	rm -f *.o
