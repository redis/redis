# Redis CRoaring dependency Makefile

STD = -std=gnu11
WARN = -Wall
OPT = -O3

AR = ar
ARFLAGS = rcs

CROARING_CFLAGS = $(STD) $(WARN) $(OPT) $(ENABLE_LTO) $(CFLAGS) -fPIC \
	-DROARING_DISABLE_X64=1 -DDISABLENEON=1 -Iinclude -Isrc

ROARING_SRC = \
	src/isadetection.c \
	src/array_util.c \
	src/bitset_util.c \
	src/art/art.c \
	src/bitset.c \
	src/containers/array.c \
	src/containers/bitset.c \
	src/containers/containers.c \
	src/containers/convert.c \
	src/containers/mixed_intersection.c \
	src/containers/mixed_union.c \
	src/containers/mixed_equal.c \
	src/containers/mixed_subset.c \
	src/containers/mixed_negation.c \
	src/containers/mixed_xor.c \
	src/containers/mixed_andnot.c \
	src/containers/run.c \
	src/memory.c \
	src/roaring.c \
	src/roaring64.c \
	src/roaring_priority_queue.c \
	src/roaring_array.c

ROARING_OBJ = $(ROARING_SRC:.c=.o)

libroaring.a: $(ROARING_OBJ)
	$(AR) $(ARFLAGS) $@ $+

%.o: %.c
	$(CC) $(CROARING_CFLAGS) -c -o $@ $<

clean:
	rm -f $(ROARING_OBJ)
	rm -f libroaring.a

.PHONY: clean
