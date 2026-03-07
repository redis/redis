# Fallback to gcc when $CC is not in $PATH.
CC ?= gcc

WARN=-Wall
OPT=-O3
STD=-std=c99

FAST_FLOAT_CFLAGS=$(WARN) $(OPT) $(STD) $(CFLAGS)
FAST_FLOAT_LDFLAGS=$(LDFLAGS)

libfast_float: fast_float_strtod.o
	$(AR) -r libfast_float.a fast_float_strtod.o

32bit: FAST_FLOAT_CFLAGS += -m32
32bit: FAST_FLOAT_LDFLAGS += -m32
32bit: libfast_float

fast_float_strtod.o: fast_float_strtod.c ffc.h
	$(CC) $(FAST_FLOAT_CFLAGS) -c fast_float_strtod.c -o fast_float_strtod.o

clean:
	rm -f *.o
	rm -f *.a
	rm -rf *.dSYM
