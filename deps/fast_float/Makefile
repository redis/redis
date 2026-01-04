# fast_float - C implementation for Redis
# This replaces the C++ version with a pure C implementation.

CC ?= gcc

WARN=-Wall -Wextra
OPT=-O3

FAST_FLOAT_CFLAGS=$(WARN) $(OPT) $(CFLAGS)
FAST_FLOAT_LDFLAGS=$(LDFLAGS)

libfast_float: fast_float_strtod.o
	$(AR) rcs libfast_float.a fast_float_strtod.o

32bit: FAST_FLOAT_CFLAGS += -m32
32bit: FAST_FLOAT_LDFLAGS += -m32
32bit: libfast_float

fast_float_strtod.o: fast_float_strtod.c fast_float_strtod.h
	$(CC) $(FAST_FLOAT_CFLAGS) -c fast_float_strtod.c -o fast_float_strtod.o $(FAST_FLOAT_LDFLAGS)

clean:
	rm -f *.o
	rm -f *.a
