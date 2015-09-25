uname_S:= $(shell sh -c 'uname -s 2>/dev/null || echo not')
STD=
WARN= -Wall
OPT= -Os


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

linenoise.o: linenoise.h linenoise.c

linenoise_example: linenoise.o example.o
	$(R_LD) -o $@ $^

.c.o:
	$(R_CC) -c $<

clean:
	rm -f linenoise_example *.o
