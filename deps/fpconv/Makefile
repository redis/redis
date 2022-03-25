STD=
WARN= -Wall
OPT= -Os

R_CFLAGS= $(STD) $(WARN) $(OPT) $(DEBUG) $(CFLAGS)
R_LDFLAGS= $(LDFLAGS)
DEBUG= -g

R_CC=$(CC) $(R_CFLAGS)
R_LD=$(CC) $(R_LDFLAGS)

AR= ar
ARFLAGS= rcs

libfpconv.a: fpconv.o
	$(AR) $(ARFLAGS) $@ $+

fpconv.o: fpconv.h fpconv.c

.c.o:
	$(R_CC) -c  $< 

clean:
	rm -f *.o
	rm -f *.a


