STD= -std=c99
WARN= -Wall
OPT= -Os

R_CFLAGS= $(STD) $(WARN) $(OPT) $(DEBUG) $(CFLAGS) -DHDR_MALLOC_INCLUDE=\"hdr_redis_malloc.h\"
R_LDFLAGS= $(LDFLAGS)
DEBUG= -g

R_CC=$(CC) $(R_CFLAGS)
R_LD=$(CC) $(R_LDFLAGS)

AR= ar
ARFLAGS= rcs

libhdrhistogram.a: hdr_histogram.o
	$(AR) $(ARFLAGS) $@ $+

hdr_histogram.o: hdr_histogram.h hdr_histogram.c

.c.o:
	$(R_CC) -c  $< 

clean:
	rm -f *.o
	rm -f *.a


