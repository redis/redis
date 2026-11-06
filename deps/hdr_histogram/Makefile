STD= -std=c99
WARN= -Wall
OPT= -O3

R_CFLAGS= $(STD) $(WARN) $(OPT) $(DEBUG) $(CFLAGS) -DHDR_MALLOC_INCLUDE=\"hdr_redis_malloc.h\"
# GCC 4.8's intrinsic headers require global AVX flags, which would bypass
# runtime CPU dispatch. Use the upstream scalar fallback on that toolchain.
ifneq ($(filter 4.8 4.8.%,$(shell $(CC) -dumpversion 2>/dev/null)),)
R_CFLAGS+= -DHDR_DISABLE_AVX2
endif

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


