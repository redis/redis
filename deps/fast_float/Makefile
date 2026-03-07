# Fallback to gcc/g++ when $CC or $CXX is not in $PATH.
CC ?= gcc

WARN=-Wall
OPT=-O3
STD=-std=c99
DEFS=-DFASTFLOAT_ALLOWS_LEADING_PLUS

FASTFLOAT_CFLAGS=$(WARN) $(OPT) $(STD) $(DEFS) $(CFLAGS)
FASTFLOAT_LDFLAGS=$(LDFLAGS)

libfast_float: fast_float_strtod.o
	$(AR) -r libfast_float.a fast_float_strtod.o

32bit: FASTFLOAT_CFLAGS += -m32
32bit: FASTFLOAT_LDFLAGS += -m32
32bit: libfast_float

fast_float_strtod.o: fast_float_strtod.c
	$(CC) $(FASTFLOAT_CFLAGS) -c fast_float_strtod.c $(FASTFLOAT_LDFLAGS)

clean:
	rm -f *.o
	rm -f *.a
	rm -f *.h.gch
	rm -rf *.dSYM
