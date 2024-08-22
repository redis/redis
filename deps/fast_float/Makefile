# Fallback to gcc/g++ when $CC or $CXX is not in $PATH.
CC ?= gcc
CXX ?= g++

CFLAGS=-Wall -std=c++11 -O3 -DFASTFLOAT_ALLOWS_LEADING_PLUS
LDFLAGS=

libfast_float: fast_float_strtod.o
	$(AR) -r libfast_float.a fast_float_strtod.o

32bit: CFLAGS += -m32
32bit: LDFLAGS += -m32
32bit: libfast_float

fast_float_strtod.o: fast_float_strtod.cpp
	$(CXX) $(CFLAGS) -c fast_float_strtod.cpp $(LDFLAGS)

clean:
	rm -f *.o
	rm -f *.a
	rm -f *.h.gch
	rm -rf *.dSYM
